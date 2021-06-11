#include "MLCDynamicStream.hh"

// Generated by slicc.
#include "mem/ruby/protocol/CoherenceMsg.hh"
#include "mem/ruby/protocol/RequestMsg.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "cpu/gem_forge/accelerator/stream/stream_engine.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/StreamRangeSync.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

MLCDynamicStream::MLCDynamicStream(CacheStreamConfigureDataPtr _configData,
                                   AbstractStreamAwareController *_controller,
                                   MessageBuffer *_responseMsgBuffer,
                                   MessageBuffer *_requestToLLCMsgBuffer)
    : stream(_configData->stream), dynamicStreamId(_configData->dynamicId),
      config(_configData), isPointerChase(_configData->isPointerChase),
      isPseudoOffload(_configData->isPseudoOffload), controller(_controller),
      responseMsgBuffer(_responseMsgBuffer),
      requestToLLCMsgBuffer(_requestToLLCMsgBuffer),
      maxNumSlices(_configData->mlcBufferNumSlices), headSliceIdx(0),
      tailSliceIdx(0),
      advanceStreamEvent([this]() -> void { this->advanceStream(); },
                         "MLC::advanceStream",
                         false /*delete after process. */) {

  /**
   * ! You should never call any virtual function in the
   * ! constructor/deconstructor.
   */

  /**
   * Remember if we require range-sync. The config will also be passed to
   * LLCDynamicStream.
   */
  auto dynS = this->stream->getDynamicStream(this->getDynamicStreamId());
  this->config->rangeSync = (dynS && dynS->shouldRangeSync());

  MLC_S_DPRINTF_(StreamRangeSync, this->getDynamicStreamId(), "%s RangeSync.\n",
                 this->shouldRangeSync() ? "Enabled" : "Disabled");

  // Schedule the first advanceStreamEvent.
  this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                            Cycles(1));

  // Remember the SendTo configs.
  for (auto &depEdge : this->config->depEdges) {
    if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
      this->sendToConfigs.emplace_back(depEdge.data);
    }
  }
}

MLCDynamicStream::~MLCDynamicStream() {
  // We got to deschedule the advanceStreamEvent.
  if (this->advanceStreamEvent.scheduled()) {
    this->stream->getCPUDelegator()->deschedule(&this->advanceStreamEvent);
  }
}

void MLCDynamicStream::endStream() {
  MLC_S_DPRINTF(this->getDynamicStreamId(), "Ended with # slices %d.\n",
                this->slices.size());
  for (auto &slice : this->slices) {
    MLC_SLICE_DPRINTF(
        slice.sliceId, "Ended with CoreStatus %s.\n",
        MLCStreamSlice::convertCoreStatusToString(slice.coreStatus));
    if (slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA) {
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
  slice->coreStatus = MLCStreamSlice::CoreStatusE::WAIT_DATA;
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
  if (slice->coreStatus != MLCStreamSlice::CoreStatusE::NONE) {
    MLC_SLICE_PANIC(sliceId, "Already seen a request.");
  }
  slice->coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  slice->coreSliceId = sliceId;
  this->advanceStream();
}

void MLCDynamicStream::popStream() {
  /**
   * So far we don't have a synchronization scheme between MLC and LLC if there
   * is no CoreUser, and that causes performance drop due to running too ahead.
   * Therefore, we try to have an ideal check that the LLCStream is ahead of me.
   * We only do this for direct streams.
   */
  uint64_t llcProgressSliceIdx = UINT64_MAX;
  uint64_t llcProgressElementIdx = UINT64_MAX;
  if (this->controller->isStreamIdeaSyncEnabled() &&
      this->getStaticStream()->isDirectMemStream() &&
      !this->shouldRangeSync()) {
    if (auto llcDynS =
            LLCDynamicStream::getLLCStream(this->getDynamicStreamId())) {
      if (llcDynS->getNextAllocSliceIdx() < llcProgressSliceIdx) {
        llcProgressSliceIdx = llcDynS->getNextAllocSliceIdx();
        MLC_S_DPRINTF(this->getDynamicStreamId(),
                      "Smaller LLCProgressSliceIdx %llu.\n",
                      llcProgressSliceIdx);
      }
      /**
       * We are also going to limit llcProgressElementIdx to the unreleased
       * IndirectElementIdx + 1024 / MemElementSize.
       */
      for (auto llcDynIS : llcDynS->getIndStreams()) {
        auto llcDynISUnreleaseElementIdx =
            llcDynIS->idxToElementMap.empty()
                ? llcDynIS->getNextInitElementIdx()
                : llcDynIS->idxToElementMap.begin()->first;
        auto llcDynISUnreleaseElementIdxOffset =
            1024 / llcDynIS->getMemElementSize();
        if (llcDynISUnreleaseElementIdx + llcDynISUnreleaseElementIdxOffset <
            llcProgressElementIdx) {
          MLC_S_DPRINTF(
              this->getDynamicStreamId(),
              "Smaller LLCDynIS %s UnreleaseElement %llu + %llu < %llu.\n",
              llcDynIS->getDynamicStreamId(), llcDynISUnreleaseElementIdx,
              llcDynISUnreleaseElementIdxOffset, llcProgressElementIdx);
          llcProgressElementIdx =
              llcDynISUnreleaseElementIdx + llcDynISUnreleaseElementIdxOffset;
        }
      }
    } else {
      // The LLC stream has not been created.
      llcProgressSliceIdx = 0;
    }
    for (const auto &depEdge : this->config->depEdges) {
      if (depEdge.type == CacheStreamConfigureData::DepEdge::Type::SendTo) {
        if (auto llcDynS =
                LLCDynamicStream::getLLCStream(depEdge.data->dynamicId)) {
          if (llcDynS->getNextInitElementIdx() < llcProgressElementIdx) {
            llcProgressElementIdx = llcDynS->getNextInitElementIdx();
            MLC_S_DPRINTF(this->getDynamicStreamId(),
                          "Smaller SendTo %s LLCProgressElementIdx %llu.\n",
                          depEdge.data->dynamicId, llcProgressElementIdx);
          }
        } else {
          // The LLC stream has not been created.
          llcProgressElementIdx = 0;
        }
      }
    }
  }

  /**
   * Maybe let's make release in order.
   * The slice is released once the core status is DONE or FAULTED.
   */
  while (!this->slices.empty()) {
    const auto &slice = this->slices.front();
    if (slice.coreStatus == MLCStreamSlice::CoreStatusE::DONE ||
        slice.coreStatus == MLCStreamSlice::CoreStatusE::FAULTED) {

      auto mlcHeadSliceIdx = this->tailSliceIdx - this->slices.size();

      if (mlcHeadSliceIdx > llcProgressSliceIdx ||
          slice.sliceId.getEndIdx() > llcProgressElementIdx) {
        MLC_SLICE_DPRINTF(
            slice.sliceId,
            "Delayed poping for IdealSync between MLC %llu and LLC %llu.\n",
            mlcHeadSliceIdx, llcProgressSliceIdx);
        // We need to schedule advanceStream to check later.
        this->scheduleAdvanceStream();
        break;
      }

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
  assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA &&
         "Element core status should be WAIT_DATA to make response.");
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
  msg->m_DataBlk = slice.dataBlock;

  /**
   * Floating AtomicComputeStream and LoadComputeStream must use
   * STREAM_FROM_MLC type as they bypass private cache and must be served by MLC
   * SE.
   */
  if (this->stream->isAtomicComputeStream() ||
      this->stream->isLoadComputeStream()) {
    msg->m_Class = CoherenceClass_STREAM_FROM_MLC;
  }

  // Show the data.
  if (Debug::DEBUG_TYPE) {
    std::stringstream ss;
    auto lineOffset = slice.sliceId.vaddr % RubySystem::getBlockSizeBytes();
    auto dataStr = GemForgeUtils::dataToString(
        slice.dataBlock.getData(lineOffset, slice.sliceId.getSize()),
        slice.sliceId.getSize());
    MLC_SLICE_DPRINTF(slice.sliceId,
                      "Make response vaddr %#x size %d data %s.\n",
                      slice.sliceId.vaddr, slice.sliceId.getSize(), dataStr);
  }
  // The latency should be consistency with the cache controller.
  // However, I still failed to find a clean way to exponse this info
  // to the stream engine. So far I manually set it to the default
  // value from the L1 cache controller.
  // TODO: Make it consistent with the cache controller.
  Cycles latency(2);
  this->responseMsgBuffer->enqueue(msg, this->controller->clockEdge(),
                                   this->controller->cyclesToTicks(latency));

  /**
   * Special case for AtomicStream with RangeSync:
   * we should expect an Ack once committed.
   * So here we transit to WAIT_ACK state.
   */
  if (this->getStaticStream()->isAtomicComputeStream() &&
      this->shouldRangeSync()) {
    slice.coreStatus = MLCStreamSlice::CoreStatusE::WAIT_ACK;
  } else {
    // Set the core status to DONE.
    slice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
  }
  // Update the stats in core SE.
  this->stream->se->numMLCResponse++;
}

void MLCDynamicStream::makeAck(MLCStreamSlice &slice) {
  assert(slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT_ACK &&
         "Element core status should be WAIT_ACK to make ack.");
  slice.coreStatus = MLCStreamSlice::CoreStatusE::ACK_READY;
  MLC_SLICE_DPRINTF(slice.sliceId, "AckReady. Header %s HeaderCoreStatus %s.\n",
                    this->slices.front().sliceId,
                    MLCStreamSlice::convertCoreStatusToString(
                        this->slices.front().coreStatus));
  // Send back ack in order.
  auto dynS = this->stream->getDynamicStream(this->dynamicStreamId);
  for (auto &ackSlice : this->slices) {
    if (ackSlice.coreStatus == MLCStreamSlice::CoreStatusE::DONE) {
      continue;
    }
    if (ackSlice.coreStatus != MLCStreamSlice::CoreStatusE::ACK_READY) {
      break;
    }
    const auto &ackSliceId = ackSlice.sliceId;
    // Set the core status to DONE.
    ackSlice.coreStatus = MLCStreamSlice::CoreStatusE::DONE;
    if (!dynS) {
      // The only exception is the second Ack for RangeSync AtomicStream.
      if (this->shouldRangeSync() && this->stream->isAtomicComputeStream()) {
        continue;
      }
      MLC_SLICE_PANIC(ackSliceId, "MakeAck when dynS has been released.");
    }
    for (auto elementIdx = ackSliceId.getStartIdx();
         elementIdx < ackSliceId.getEndIdx(); ++elementIdx) {
      if (std::dynamic_pointer_cast<LinearAddrGenCallback>(
              this->config->addrGenCallback)) {
        auto elementVAddr =
            this->config->addrGenCallback
                ->genAddr(elementIdx, this->config->addrGenFormalParams,
                          getStreamValueFail)
                .uint64();
        if (elementVAddr + this->config->elementSize >
            ackSliceId.vaddr + ackSliceId.getSize()) {
          // This element spans to next slice, do not ack here.
          MLC_SLICE_DPRINTF(ackSliceId,
                            "Skipping Ack for multi-slice element %llu [%#x, "
                            "+%d) slice [%#x, +%d).\n",
                            elementIdx, elementVAddr, this->config->elementSize,
                            ackSliceId.vaddr, ackSliceId.getSize());
          continue;
        }
      }
      MLC_SLICE_DPRINTF(slice.sliceId, "Ack for element %llu.\n", elementIdx);
      dynS->cacheAckedElements.insert(elementIdx);
    }
  }
}

Addr MLCDynamicStream::translateVAddr(Addr vaddr) const {
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  Addr paddr;
  if (!cpuDelegator->translateVAddrOracle(vaddr, paddr)) {
    panic("Failed translate vaddr %#x.\n", vaddr);
  }
  return paddr;
}

void MLCDynamicStream::readBlob(Addr vaddr, uint8_t *data, int size) const {
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  cpuDelegator->readFromMem(vaddr, size, data);
}

MachineID MLCDynamicStream::mapPAddrToLLCBank(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto llcMachineId = this->controller->mapAddressToLLC(
      paddr, static_cast<MachineType>(selfMachineId.type + 1));
  return llcMachineId;
}

void MLCDynamicStream::receiveStreamRange(
    const DynamicStreamAddressRangePtr &range) {
  // We simply notify the dynamic streams in core for now.
  if (!this->shouldRangeSync()) {
    MLC_S_PANIC(this->getDynamicStreamId(),
                "Receive StreamRange when RangeSync not required.");
  }
  auto *dynS =
      this->getStaticStream()->getDynamicStream(this->getDynamicStreamId());
  if (!dynS) {
    return;
  }
  dynS->receiveStreamRange(range);
}

void MLCDynamicStream::receiveStreamDone(const DynamicStreamSliceId &sliceId) {
  MLC_S_PANIC(this->getDynamicStreamId(), "receiveStreamDone not implemented.");
}

void MLCDynamicStream::scheduleAdvanceStream() {
  if (!this->advanceStreamEvent.scheduled()) {
    this->stream->getCPUDelegator()->schedule(&this->advanceStreamEvent,
                                              Cycles(1));
  }
}

void MLCDynamicStream::panicDump() const {
  MLC_S_HACK(this->dynamicStreamId,
             "-------------------Panic Dump--------------------\n");
  for (const auto &slice : this->slices) {
    MLC_SLICE_HACK(slice.sliceId, "VAddr %#x Data %d Core %s.\n",
                   slice.sliceId.vaddr, slice.dataReady,
                   MLCStreamSlice::convertCoreStatusToString(slice.coreStatus));
  }
}