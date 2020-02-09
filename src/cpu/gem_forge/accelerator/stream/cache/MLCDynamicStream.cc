#include "MLCDynamicStream.hh"

// Generated by slicc.
#include "mem/ruby/protocol/CoherenceMsg.hh"
#include "mem/ruby/protocol/RequestMsg.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStream.hh"

#define DEBUG_TYPE MLCRubyStream
#include "../stream_log.hh"

MLCDynamicStream::MLCDynamicStream(CacheStreamConfigureData *_configData,
                                   AbstractStreamAwareController *_controller,
                                   MessageBuffer *_responseMsgBuffer,
                                   MessageBuffer *_requestToLLCMsgBuffer)
    : stream(_configData->stream), dynamicStreamId(_configData->dynamicId),
      isPointerChase(_configData->isPointerChase), controller(_controller),
      responseMsgBuffer(_responseMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer),
      maxNumSlices(_controller->getMLCStreamBufferInitNumEntries()),
      headSliceIdx(0), tailSliceIdx(0),
      advanceStreamEvent([this]() -> void { this->advanceStream(); },
                         "MLC::advanceStream",
                         false /*delete after process. */) {

  /**
   * ! You should never call any virtual function in the
   * ! constructor/deconstructor.
   */

  // Schedule the first advanceStreamEvent.
  this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                            Cycles(1));
}

MLCDynamicStream::~MLCDynamicStream() {
  // We got to deschedule the advanceStreamEvent.
  if (this->advanceStreamEvent.scheduled()) {
    this->stream->getCPUDelegator()->deschedule(&this->advanceStreamEvent);
  }
}

void MLCDynamicStream::endStream() {
  for (auto &slice : this->slices) {
    if (slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT) {
      // Make a dummy response.
      // Ignore whether the data is ready.
      // ! For indirect stream, the sliceId may not have vaddr.
      // ! In such case, we set it from core's sliceId.
      // TODO: Fix this in a more rigorous way.
      if (slice.sliceId.vaddr == 0) {
        slice.sliceId.vaddr = slice.coreSliceId.vaddr;
      }
      this->makeResponse(slice);
    }
  }
}

void MLCDynamicStream::receiveStreamRequest(
    const DynamicStreamSliceId &sliceId) {
  MLC_SLICE_DPRINTF(sliceId, "Receive request to %#x. Tail %lu.\n",
                    sliceId.vaddr, this->tailSliceIdx);

  auto slice = this->findSliceForCoreRequest(sliceId);
  assert(slice->coreStatus == MLCStreamSlice::CoreStatusE::NONE &&
         "Already seen a request.");
  MLC_SLICE_DPRINTF(slice->sliceId, "Matched to request.\n");
  slice->coreStatus = MLCStreamSlice::CoreStatusE::WAIT;
  slice->coreWaitCycle = this->controller->curCycle();
  slice->coreSliceId = sliceId;
  if (slice->dataReady) {
    // Sanity check the address.
    // ! Core is line address.
    if (slice->coreSliceId.vaddr != makeLineAddress(slice->sliceId.vaddr)) {
      MLC_SLICE_PANIC(sliceId, "Mismatch between Core %#x and LLC %#x.\n",
                      slice->coreSliceId.vaddr, slice->sliceId.vaddr);
    }
    this->makeResponse(*slice);
  }
  this->advanceStream();
}

void MLCDynamicStream::receiveStreamRequestHit(
    const DynamicStreamSliceId &sliceId) {
  MLC_SLICE_DPRINTF(sliceId, "Receive request hit to %#x.\n", sliceId.vaddr);

  auto slice = this->findSliceForCoreRequest(sliceId);
  assert(slice->coreStatus == MLCStreamSlice::CoreStatusE::NONE &&
         "Already seen a request.");
  slice->coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  slice->coreSliceId = sliceId;
  this->advanceStream();
}

void MLCDynamicStream::popStream() {
  /**
   * Maybe let's make release in order.
   * The slice is released once the core status is DONE or FAULTED.
   */
  while (!this->slices.empty()) {
    const auto &slice = this->slices.front();
    if (slice.coreStatus == MLCStreamSlice::CoreStatusE::DONE ||
        slice.coreStatus == MLCStreamSlice::CoreStatusE::FAULTED) {
      MLC_SLICE_DPRINTF(slice.sliceId, "Pop.\n");

      // Update the statistics.
      if (slice.coreWaitCycle != 0 && slice.dataReadyCycle != 0) {
        auto &streamStats = this->stream->statistic;
        if (slice.coreWaitCycle > slice.dataReadyCycle) {
          // Early.
          streamStats.numMLCEarlySlice++;
          streamStats.numMLCEarlyCycle +=
              slice.coreWaitCycle - slice.dataReadyCycle;
        } else {
          // Late.
          streamStats.numMLCLateSlice++;
          streamStats.numMLCLateCycle +=
              slice.dataReadyCycle - slice.coreWaitCycle;
        }
      }

      this->headSliceIdx++;
      this->slices.pop_front();
    } else {
      // We made no progress.
      break;
    }
  }
}

void MLCDynamicStream::makeResponse(MLCStreamSlice &slice) {
  assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT &&
         "Element core status should be WAIT to make response.");
  Addr paddr = this->translateVAddr(slice.sliceId.vaddr);
  auto paddrLine = makeLineAddress(paddr);

  auto selfMachineId = this->controller->getMachineID();
  auto upperMachineId = MachineID(
      static_cast<MachineType>(selfMachineId.type - 1), selfMachineId.num);
  auto msg = std::make_shared<CoherenceMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Class = CoherenceClass_DATA_EXCLUSIVE;
  msg->m_Sender = selfMachineId;
  msg->m_Dest = upperMachineId;
  msg->m_MessageSize = MessageSizeType_Response_Data;

  MLC_SLICE_DPRINTF(slice.sliceId, "Make response.\n");
  // The latency should be consistency with the cache controller.
  // However, I still failed to find a clean way to exponse this info
  // to the stream engine. So far I manually set it to the default
  // value from the L1 cache controller.
  // TODO: Make it consistent with the cache controller.
  Cycles latency(2);
  this->responseMsgBuffer->enqueue(msg, this->controller->clockEdge(),
                                   this->controller->cyclesToTicks(latency));
  // Set the core status to DONE.
  slice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
}

Addr MLCDynamicStream::translateVAddr(Addr vaddr) const {
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }
  return paddr;
}

MachineID MLCDynamicStream::mapPAddrToLLCBank(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto llcMachineId = this->controller->mapAddressToLLC(
      paddr, static_cast<MachineType>(selfMachineId.type + 1));
  return llcMachineId;
}

void MLCDynamicStream::panicDump() const {
  MLC_S_DPRINTF("-------------------Panic Dump--------------------\n");
  for (const auto &slice : this->slices) {
    MLC_SLICE_DPRINTF(
        slice.sliceId, "VAddr %#x Data %d Core %s.\n", slice.sliceId.vaddr,
        slice.dataReady,
        MLCStreamSlice::convertCoreStatusToString(slice.coreStatus).c_str());
  }
}