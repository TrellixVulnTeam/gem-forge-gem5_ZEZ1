#include "MLCDynamicDirectStream.hh"
#include "LLCStreamEngine.hh"
#include "MLCDynamicIndirectStream.hh"
#include "cpu/gem_forge/accelerator/stream/stream.hh"

// Generated by slicc.
#include "mem/ruby/protocol/CoherenceMsg.hh"
#include "mem/ruby/protocol/RequestMsg.hh"
#include "mem/simple_mem.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/MLCRubyStreamBase.hh"
#include "debug/MLCStreamLoopBound.hh"
#include "debug/StreamRangeSync.hh"

#define DEBUG_TYPE MLCRubyStreamBase
#include "../stream_log.hh"

MLCDynamicDirectStream::MLCDynamicDirectStream(
    CacheStreamConfigureDataPtr _configData,
    AbstractStreamAwareController *_controller,
    MessageBuffer *_responseMsgBuffer, MessageBuffer *_requestToLLCMsgBuffer,
    const std::vector<MLCDynamicIndirectStream *> &_indirectStreams)
    : MLCDynamicStream(_configData, _controller, _responseMsgBuffer,
                       _requestToLLCMsgBuffer, true /* isMLCDirect */),
      slicedStream(_configData),
      maxNumSlicesPerSegment(
          std::max(1, _configData->mlcBufferNumSlices /
                          _controller->getMLCStreamBufferToSegmentRatio())),
      indirectStreams(_indirectStreams) {

  /**
   * Initialize the LLC bank.
   * Be careful that for MidwayFloat, we reset the InitPAddr.
   */
  if (!_configData->initPAddrValid) {
    MLC_S_PANIC_NO_DUMP(this->getDynamicStreamId(), "Invalid InitPAddr.");
  }
  if (_configData->firstFloatElementIdx > 0) {
    auto vaddr =
        this->slicedStream.getElementVAddr(_configData->firstFloatElementIdx);
    Addr paddr;
    if (!this->getStaticStream()->getCPUDelegator()->translateVAddrOracle(
            vaddr, paddr)) {
      MLC_S_DPRINTF(this->getDynamicStreamId(),
                    "[MidwayFloat] Fault on FirstFloatElem %llu.\n",
                    _configData->firstFloatElementIdx);
      paddr = this->controller->getAddressToOurLLC();
    } else {
      MLC_S_DPRINTF(this->getDynamicStreamId(),
                    "[MidwayFloat] FirstFloatElem %llu VAddr %#x PAddr %#x.\n",
                    _configData->firstFloatElementIdx, vaddr, paddr);
    }
    _configData->initPAddr = paddr;
  }

  this->tailPAddr = _configData->initPAddr;

  // Set the base stream for indirect streams.
  for (auto dynIS : this->indirectStreams) {
    dynIS->setBaseStream(this);
  }

  /**
   * Initialize the buffer for some slices.
   * Notice that we should enforce the constraint that receiver streams should
   * at least initialize these elements, similar to what we did in
   * sendCreditToLLC().
   *
   * However, so far the initialization happens out-of-order,
   * This causes problem when the sender is very small, e.g. 1 byte.
   * So we limit the maxNumSlicesPerSegment for these streams.
   */
  if (!this->sendToConfigs.empty()) {
    auto S = this->getStaticStream();
    auto memElementSize = S->getMemElementSize();
    auto maxRatio = 1;
    for (const auto &sendToConfig : this->sendToConfigs) {
      auto sendToMemElementSize = sendToConfig->stream->getMemElementSize();
      auto ratio = sendToMemElementSize / memElementSize;
      maxRatio = std::max(maxRatio, ratio);
    }
    auto originalMaxNumSlicesPerSegment = this->maxNumSlicesPerSegment;
    if (maxRatio * 2 >= this->maxNumSlicesPerSegment) {
      this->maxNumSlicesPerSegment = 2;
    } else {
      this->maxNumSlicesPerSegment /= maxRatio;
    }
    MLC_S_DPRINTF(
        this->getDynamicStreamId(),
        "Adjust MaxNumSlicesPerSegment from %llu as MaxRatio %llu to %llu.\n",
        originalMaxNumSlicesPerSegment, maxRatio, this->maxNumSlicesPerSegment);
  }

  /**
   * If this comes with IndirectAtomicComputeStream with RangeSync and
   * CoreIssue, we limit the run ahead length.
   */
  for (auto dynIS : this->indirectStreams) {
    auto IS = dynIS->getStaticStream();
    auto dynCoreIS = IS->getDynamicStream(dynIS->getDynamicStreamId());
    if (dynCoreIS) {
      if (IS->isAtomicComputeStream() && dynIS->shouldRangeSync() &&
          dynCoreIS->shouldCoreSEIssue()) {
        auto elementsPerSlice = this->slicedStream.getElementPerSlice();
        auto newMaxNumSlicesPerSegment =
            std::max(1, static_cast<int>(16 / elementsPerSlice));
        auto newMaxNumSlices =
            std::max(1, static_cast<int>(16 / elementsPerSlice));
        MLC_S_DPRINTF(this->getDynamicStreamId(),
                      "Adjust MaxNumSlicesPerSegment %llu -> %llu, "
                      "MaxNumSlices %llu -> %llu.\n",
                      this->maxNumSlicesPerSegment, newMaxNumSlicesPerSegment,
                      this->maxNumSlices, newMaxNumSlices);
        this->maxNumSlicesPerSegment = newMaxNumSlicesPerSegment;
        this->maxNumSlices = newMaxNumSlices;
      }
    }
  }

  while (this->tailSliceIdx < this->maxNumSlicesPerSegment &&
         !this->slicedStream.hasOverflowed()) {
    this->allocateSlice();
  }

  // Intialize the first segment.
  this->pushNewLLCSegment(_configData->initPAddr, this->headSliceIdx);
  this->llcSegments.front().state = LLCSegmentPosition::State::CREDIT_SENT;

  // Set the CacheStreamConfigureData to inform the LLC stream engine
  // initial credit.
  _configData->initCreditedIdx = this->tailSliceIdx;

  MLC_S_DPRINTF(this->dynamicStreamId, "InitAllocatedSlice %d overflowed %d.\n",
                this->tailSliceIdx, this->slicedStream.hasOverflowed());

  this->scheduleAdvanceStream();
}

void MLCDynamicDirectStream::advanceStream() {
  this->tryPopStream();
  /**
   * In order to synchronize the direct/indirect stream, we want to make sure
   * that the direct stream is only ahead of the indirect stream by a reasonable
   * distance.
   *
   * We also directly check the LLC streams.
   */
  uint64_t maxISElements = 0;
  for (auto dynIS : this->indirectStreams) {
    assert(dynIS->getTailSliceIdx() >= dynIS->getHeadSliceIdx() &&
           "Illegal Head/TailSliceIdx.\n");
    auto ISElements = dynIS->getTailSliceIdx() - dynIS->getHeadSliceIdx();
    if (ISElements > maxISElements) {
      MLC_S_DPRINTF(dynIS->getDynamicStreamId(),
                    "[MLCAdvance] New MaxISElements %llu.\n", ISElements);
      maxISElements = ISElements;
    }
  }
  uint64_t indirectSlicesThreshold =
      2 * this->maxNumSlices *
      std::max(static_cast<uint64_t>(1),
               static_cast<uint64_t>(this->slicedStream.getElementPerSlice()));
  MLC_S_DPRINTF(this->dynamicStreamId, "MaxISElements %llu Threshold %llu.\n",
                maxISElements, indirectSlicesThreshold);
  // Of course we need to allocate more slices.
  if (maxISElements < indirectSlicesThreshold) {

    /**
     * If we require range-sync, we can only release the slice after the segment
     * is committed.
     */
    auto currentHeadSliceIdx = this->headSliceIdx;
    if (this->shouldRangeSync()) {
      for (const auto &segment : this->llcSegments) {
        if (segment.state == LLCSegmentPosition::State::COMMITTED) {
          continue;
        }
        // This is the fisrt segement that has not been committed.
        currentHeadSliceIdx =
            std::min(currentHeadSliceIdx, segment.startSliceIdx);
        break;
      }
    }

    while (this->tailSliceIdx - currentHeadSliceIdx < this->maxNumSlices &&
           !this->hasOverflowed()) {
      this->allocateSlice();
    }
  }

  // We may need to schedule advance stream if the first slice is FAULTED,
  // as no other event will cause it to be released.
  // Same for DONE elements because we may have no core user and not receive
  // data From LLC.
  // Unless we are blocked in popStream.
  if (!this->slices.empty()) {
    auto frontCoreStatus = this->slices.front().coreStatus;
    if (frontCoreStatus == MLCStreamSlice::CoreStatusE::FAULTED ||
        frontCoreStatus == MLCStreamSlice::CoreStatusE::DONE) {
      if (!this->popBlocked) {
        this->scheduleAdvanceStream();
      }
    }
  }

  this->trySendCreditToLLC();
}

void MLCDynamicDirectStream::allocateSlice() {
  auto sliceId = this->slicedStream.getNextSlice();
  this->slices.emplace_back(sliceId);
  this->stream->statistic.numMLCAllocatedSlice++;

  // Update the llc cut information.
  if (this->llcCutLineVAddr == sliceId.vaddr) {
    // This should be cutted.
    this->llcCutSliceIdx = this->tailSliceIdx;
    this->llcCutted = true;
  }

  // Try to handle faulted slice.
  Addr paddr;
  auto cpuDelegator = this->getStaticStream()->getCPUDelegator();
  if (cpuDelegator->translateVAddrOracle(sliceId.vaddr, paddr)) {
    /**
     * This address is valid.
     * Check if we have reuse data.
     */
    auto reuseIter = this->reuseBlockMap.find(sliceId.vaddr);
    if (reuseIter != this->reuseBlockMap.end()) {
      this->slices.back().setData(reuseIter->second,
                                  this->controller->curCycle());
      this->reuseBlockMap.erase(reuseIter);
    }

    /**
     * ! We cheat here to notify the indirect stream immediately,
     * ! to avoid some complicate problem of managing streams.
     */
    assert(this->controller->params()->ruby_system->getAccessBackingStore() &&
           "This only works with backing store.");
    this->notifyIndirectStream(this->slices.back());

    /**
     * The address is valid, but we check if this stream has no core user,
     * or if this is just a PseudoOffload.
     */
    if (this->isWaitingAck()) {
      this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::WAIT_ACK;
    } else if (this->isWaitingNothing()) {
      // We mark this slice done.
      this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::DONE;
    }
  } else {
    // This address is invalid. Mark the slice faulted.
    this->slices.back().coreStatus = MLCStreamSlice::CoreStatusE::FAULTED;
  }
  MLC_SLICE_DPRINTF(sliceId, "Allocated %#x, CoreStatus %s.\n", sliceId.vaddr,
                    MLCStreamSlice::convertCoreStatusToString(
                        this->slices.back().coreStatus));

  // Try to find where the LLC stream would be at this point.
  this->tailSliceIdx++;
  this->nextSegmentSliceIds.add(sliceId);
  this->tailSliceId = this->slicedStream.peekNextSlice();
  if (cpuDelegator->translateVAddrOracle(
          this->slicedStream.peekNextSlice().vaddr, paddr)) {
    // The next slice would be valid.
    this->tailPAddr = paddr;
  } else {
    // This address is invalid.
    // Do not update tailPAddr as the LLC stream would not move.
  }

  this->allocateLLCSegment();
}

void MLCDynamicDirectStream::trySendCreditToLLC() {
  /**
   * Find the first LLCSegment in ALLOCATED state and try issue credit.
   * Unless we are currently blocked by Receiver's element initialization.
   */
  if (this->blockedOnReceiverElementInit) {
    return;
  }

  uint64_t firstDataUnreadyElementIdx = UINT64_MAX;
  if (this->config->isPointerChase) {
    for (const auto &slice : this->slices) {
      firstDataUnreadyElementIdx = slice.sliceId.getStartIdx();
      if (!slice.dataReady) {
        break;
      }
    }
  }

  for (auto &segment : this->llcSegments) {

    if (segment.state != LLCSegmentPosition::State::ALLOCATED) {
      continue;
    }

    /**
     * For PointerChaseStreams, we cannot send out credits until
     * all previous slices are data ready. This is to ensure that
     * we model the timing correctly.
     */
    if (this->config->isPointerChase &&
        firstDataUnreadyElementIdx != UINT64_MAX) {
      if (segment.getStartSliceId().getStartIdx() >=
          firstDataUnreadyElementIdx + 1) {
        MLC_S_DPRINTF(this->getDynamicStreamId(),
                      "Delayed sending PtrChase Credit since "
                      "FirstUnreadyElement %llu + 1 <= CreditElement %llu.\n",
                      firstDataUnreadyElementIdx,
                      segment.getStartSliceId().getStartIdx());
        return;
      }
    }

    /**
     * Additional check for SendTo relationship:
     * We want to make sure that the receiver has the element initialized.
     * If not, we schedule an event to check next cycle.
     * We should also check receiver from my indirect streams.
     * Skip if the receiver is myself, which is used for Two-Level Indirection.
     */
    const auto tailElementIdx =
        segment.sliceIds.sliceIds.back().getEndIdx() - 1;
    LLCDynamicStreamPtr waitForReceiver = nullptr;
    for (const auto &sendToConfig : this->sendToConfigs) {
      if (sendToConfig->dynamicId == this->getDynamicStreamId()) {
        continue;
      }
      auto llcReceiverS =
          LLCDynamicStream::getLLCStream(sendToConfig->dynamicId);
      if (llcReceiverS) {
        if (!llcReceiverS->isElementInitialized(tailElementIdx)) {
          waitForReceiver = llcReceiverS;
          break;
        }
      }
    }
    if (!waitForReceiver) {
      for (auto dynIS : this->indirectStreams) {
        for (const auto &sendToConfig : dynIS->getSendToConfigs()) {
          auto llcReceiverS =
              LLCDynamicStream::getLLCStream(sendToConfig->dynamicId);
          if (llcReceiverS) {
            if (!llcReceiverS->isElementInitialized(tailElementIdx)) {
              waitForReceiver = llcReceiverS;
              break;
            }
          }
        }
        if (waitForReceiver) {
          break;
        }
      }
    }
    if (waitForReceiver) {
      // Register callbacks when the receiver elements are initialized.
      MLC_S_DPRINTF(this->getDynamicStreamId(),
                    "Delayed sending credit to LLC as receiver %s has not "
                    "initialized element %llu.\n",
                    waitForReceiver->getDynamicStreamId(), tailElementIdx);
      auto elementInitCallback = [this](const DynamicStreamId &dynStreamId,
                                        uint64_t elementIdx) -> void {
        this->blockedOnReceiverElementInit = false;
        this->trySendCreditToLLC();
      };
      this->blockedOnReceiverElementInit = true;
      waitForReceiver->registerElementInitCallback(tailElementIdx,
                                                   elementInitCallback);
      return;
    }
    this->sendCreditToLLC(segment);
    segment.state = LLCSegmentPosition::State::CREDIT_SENT;
  }
}

void MLCDynamicDirectStream::sendCreditToLLC(
    const LLCSegmentPosition &segment) {
  /**
   * We look at the last llc segment to determine where the stream is.
   * And we push a new llc segment.
   *
   * This will not work for pointer chasing stream.
   */
  auto llcBank = this->mapPAddrToLLCBank(segment.startPAddr);

  MLC_S_DPRINTF(this->dynamicStreamId,
                "Extended %lu -> %lu, sent credit to LLC%d.\n",
                segment.startSliceIdx, segment.endSliceIdx, llcBank);
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = makeLineAddress(segment.startPAddr);
  msg->m_Type = CoherenceRequestType_STREAM_FLOW;
  msg->m_XXNewRewquestor.add(this->controller->getMachineID());
  msg->m_Destination.add(llcBank);
  msg->m_MessageSize = MessageSizeType_Control;
  DynamicStreamSliceId sliceId;
  sliceId.getDynStreamId() = this->dynamicStreamId;
  sliceId.getStartIdx() = segment.startSliceIdx;
  sliceId.getEndIdx() = segment.endSliceIdx;
  msg->m_sliceIds.add(sliceId);

  /**
   * Immediately initialize all the LLCStreamSlices and LLCStreamElements to
   * simplify the implementation.
   */
  auto llcS = LLCDynamicStream::getLLCStreamPanic(this->getDynamicStreamId());
  llcS->initDirectStreamSlicesUntil(segment.endSliceIdx);

  Cycles latency(1); // Just use 1 cycle latency here.

  if (this->controller->isStreamIdeaFlowEnabled()) {
    auto llcController = this->controller->getController(llcBank);
    auto llcSE = llcController->getLLCStreamEngine();
    llcSE->receiveStreamFlow(sliceId);
  } else {
    this->requestToLLCMsgBuffer->enqueue(
        msg, this->controller->clockEdge(),
        this->controller->cyclesToTicks(latency));
  }
}

void MLCDynamicDirectStream::receiveStreamData(
    const DynamicStreamSliceId &sliceId, const DataBlock &dataBlock,
    Addr paddrLine) {
  assert(sliceId.isValid() && "Invalid stream slice id for stream data.");

  auto numElements = sliceId.getNumElements();
  assert(this->dynamicStreamId == sliceId.getDynStreamId() &&
         "Unmatched dynamic stream id.");
  MLC_SLICE_DPRINTF(sliceId, "Receive data %#x.\n", sliceId.vaddr);

  /**
   * It is possible when the core stream engine runs ahead than
   * the LLC stream engine, and the stream data is delivered after
   * the slice is released. In such case we will ignore the
   * stream data.
   *
   * TODO: Properly handle this with sliceIdx.
   */
  if (this->slices.empty()) {
    assert(this->hasOverflowed() && "No slices when not overflowed yet.");
    // Simply ignore it.
    return;
  } else {
    // TODO: Properly detect that the slice is lagging behind.
    const auto &firstSlice = this->slices.front();
    bool laggingBehind = false;
    if (sliceId.getStartIdx() < firstSlice.sliceId.getStartIdx()) {
      laggingBehind = true;
    }
    if (sliceId.getStartIdx() == firstSlice.sliceId.getStartIdx() &&
        sliceId.vaddr < firstSlice.sliceId.vaddr) {
      // Due to multi-line elements, we have to also check vaddr.
      laggingBehind = true;
    }
    if (laggingBehind) {
      // The stream data is lagging behind. The slice is already
      // released.
      MLC_SLICE_DPRINTF(sliceId, "Discard as lagging behind %s.\n",
                        firstSlice.sliceId);
      return;
    }
  }

  /**
   * Find the correct stream slice and insert the data there.
   * Here we reversely search for it to save time.
   */
  for (auto slice = this->slices.rbegin(), end = this->slices.rend();
       slice != end; ++slice) {
    if (this->matchLLCSliceId(slice->sliceId, sliceId)) {
      // Found the slice.
      if (slice->sliceId.getNumElements() != numElements) {
        // Also consider llc stream being cut.
        if (this->llcCutLineVAddr > 0 &&
            slice->sliceId.vaddr < this->llcCutLineVAddr) {
          MLC_S_PANIC(this->dynamicStreamId,
                      "Mismatch numElements, incoming %d, slice %d.\n",
                      numElements, slice->sliceId.getNumElements());
        }
      }
      if (slice->dataReady) {
        // Must be from reuse.
      } else {
        slice->setData(dataBlock, this->controller->curCycle());
      }

      MLC_SLICE_DPRINTF(
          sliceId, "Found matched slice %s, core status %s.\n", slice->sliceId,
          MLCStreamSlice::convertCoreStatusToString(slice->coreStatus));
      if (slice->coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA) {
        this->makeResponse(*slice);
      } else if (slice->coreStatus == MLCStreamSlice::CoreStatusE::WAIT_ACK) {
        // Ack the stream element.
        // TODO: Send the packet back via normal message buffer.
        // hack("Indirect slices acked element %llu size %llu header %llu.\n",
        //      sliceId.getStartIdx(), this->slices.size(),
        //      this->slices.front().sliceId.getStartIdx());
        this->makeAck(*slice);
      } else if (slice->coreStatus == MLCStreamSlice::CoreStatusE::ACK_READY) {
        MLC_SLICE_PANIC(sliceId, "Received multiple acks.");
      }
      this->advanceStream();
      return;
    }
  }

  MLC_SLICE_PANIC(sliceId, "Fail to find the slice. Tail %lu.\n",
                  this->tailSliceIdx);
}

void MLCDynamicDirectStream::notifyIndirectStream(const MLCStreamSlice &slice) {
  if (this->indirectStreams.empty()) {
    return;
  }

  bool hasIndirectAddrStreams = false;
  auto S = this->getStaticStream();
  for (auto dynIS : this->indirectStreams) {
    auto IS = dynIS->getStaticStream();
    if (S->addrDepStreams.count(IS)) {
      hasIndirectAddrStreams = true;
      break;
    }
  }
  if (!hasIndirectAddrStreams) {
    // The indirect stream is not really dependent on me to compute the address.
    // We do not bother to notify indirect streams.
    // TODO: Too hacky?
    return;
  }

  const auto &sliceId = slice.sliceId;
  MLC_SLICE_DPRINTF(sliceId, "Notify IndirectSream.\n");
  for (auto elementIdx = sliceId.getStartIdx();
       elementIdx < sliceId.getEndIdx(); ++elementIdx) {

    // Try to extract the stream data.
    auto elementVAddr = this->slicedStream.getElementVAddr(elementIdx);
    auto elementSize = this->slicedStream.getMemElementSize();
    auto elementLineOffset = elementVAddr % RubySystem::getBlockSizeBytes();

    /**
     * For multi-line base element, we make sure that we only notify the
     * indirect stream once.
     * TODO: Really handle this case.
     */
    if (makeLineAddress(elementVAddr) != makeLineAddress(sliceId.vaddr)) {
      continue;
    }

    std::vector<uint8_t> elementData(elementSize, 0);
    auto rubySystem = this->controller->params()->ruby_system;
    if (rubySystem->getAccessBackingStore()) {
      // Get the data from backing store.
      // Using this API to handle page crossing.
      this->readBlob(elementVAddr, elementData.data(), elementData.size());
      // Addr elementPAddr = this->translateVAddr(elementVAddr);
      // RequestPtr req = std::make_shared<Request>(elementPAddr, elementSize,
      // 0,
      //                                            0 /* MasterId */);
      // PacketPtr pkt = Packet::createRead(req);
      // pkt->dataStatic(elementData.data());
      // rubySystem->getPhysMem()->functionalAccess(pkt);
      // delete pkt;
    } else {
      // Get the data from the cache line.
      assert(elementLineOffset + elementSize <=
                 RubySystem::getBlockSizeBytes() &&
             "Cannot support multi-line element with indirect streams without "
             "backing store.");
      for (auto byteOffset = 0; byteOffset < elementSize; ++byteOffset) {
        elementData[byteOffset] =
            slice.dataBlock.getByte(byteOffset + elementLineOffset);
      }
    }
    MLC_SLICE_DPRINTF(
        sliceId, "Extract element %lu data %s.\n", elementIdx,
        GemForgeUtils::dataToString(elementData.data(), elementData.size()));
    for (auto indirectStream : this->indirectStreams) {
      auto IS = indirectStream->getStaticStream();
      if (S->addrDepStreams.count(IS) == 0) {
        // This indirect stream does not use my data to generate address.
        continue;
      }
      uint64_t baseData = 0;
      int32_t subOffset = 0;
      int32_t subSize = elementSize;
      /**
       * In case the base stream is coalesced, we have to translate the offset
       * for the indirect streams.
       */
      const auto &baseEdges = IS->addrBaseEdges;
      if (baseEdges.empty()) {
        MLC_SLICE_PANIC(sliceId, "IS has no base edges.", baseEdges.size());
      }
      auto baseId = baseEdges.front().toStaticId;
      for (const auto &baseEdge : baseEdges) {
        if (baseEdge.toStaticId != baseId) {
          MLC_SLICE_PANIC(sliceId, "IS has multiple base streams.",
                          baseEdges.size());
        }
      }
      S->getCoalescedOffsetAndSize(baseId, subOffset, subSize);
      assert(subOffset + subSize <= elementSize &&
             "Overflow of coalesced base element.");
      baseData =
          GemForgeUtils::rebuildData(elementData.data() + subOffset, subSize);
      MLC_SLICE_DPRINTF(sliceId,
                        "Notify IndS base %lu offset %d size %d data %llu.\n",
                        elementIdx, subOffset, subSize, baseData);
      indirectStream->receiveBaseStreamData(elementIdx, baseData);
    }
  }
}

MLCDynamicDirectStream::SliceIter
MLCDynamicDirectStream::findSliceForCoreRequest(
    const DynamicStreamSliceId &sliceId) {
  if (this->slices.empty()) {
    MLC_SLICE_PANIC(
        sliceId, "No slices for request, overflowed %d, totalTripCount %lu.\n",
        this->hasOverflowed(), this->getTotalTripCount());
  }
  // Try to allocate more slices.
  while (!this->hasOverflowed() &&
         this->slices.back().sliceId.getStartIdx() <= sliceId.getStartIdx()) {
    this->allocateSlice();
  }
  for (auto iter = this->slices.begin(), end = this->slices.end(); iter != end;
       ++iter) {
    /**
     * So far we match them on vaddr.
     * TODO: Really assign the sliceIdx and match that.
     */
    if (this->matchCoreSliceId(iter->sliceId, sliceId)) {
      return iter;
    }
  }

  MLC_S_PANIC(this->dynamicStreamId, "Failed to find slice for core %s.\n",
              sliceId);
}

void MLCDynamicDirectStream::receiveReuseStreamData(
    Addr vaddr, const DataBlock &dataBlock) {
  MLC_S_DPRINTF(this->dynamicStreamId, "Received reuse block %#x.\n", vaddr);
  /**
   * Somehow it's possible that the slice is already allocated.
   * Search for it.
   */
  bool reused = false;
  for (auto &slice : this->slices) {
    if (slice.sliceId.vaddr == vaddr) {
      reused = true;
      if (!slice.dataReady) {
        slice.setData(dataBlock, this->controller->curCycle());
        if (slice.coreStatus == MLCStreamSlice::CoreStatusE::WAIT_DATA) {
          this->makeResponse(slice);
        }
        this->advanceStream();
      }
      break;
    }
  }
  if (!reused) {
    this->reuseBlockMap.emplace(vaddr, dataBlock).second;
  }
  /**
   * TODO: The current implementation may have multiple reuses, in
   * TODO: the boundary cases when streams overlapped.
   */
}

std::string
MLCDynamicDirectStream::LLCSegmentPosition::stateToString(const State state) {
#define Case(x)                                                                \
  case x:                                                                      \
    return #x
  switch (state) {
  default:
    panic("Unknown MLCDynamicDirectStream::LLCSegmentPosition::State %d.",
          state);
    Case(ALLOCATED);
    Case(COMMITTING);
    Case(COMMITTED);
  }
#undef Case
}

void MLCDynamicDirectStream::allocateLLCSegment() {
  /**
   * Corner case when initializing the stream.
   * The initial credits are directly sent out with the configuration,
   * and thus there is no previous segment for us to built on.
   * See the pushNewLLCSegment() in the constructor.
   */
  if (this->llcSegments.empty()) {
    return;
  }

  /**
   * There are three cases we need to create a new segment.
   * 1. We have reached the maximum number of slices per segment.
   * 2. The stream has overflowed.
   * 3. The llc stream is cutted.
   */
  const auto &lastSegment = this->getLastLLCSegment();
  auto lastSegmentSliceIdx = lastSegment.endSliceIdx;
  bool shouldAllocateSegment = false;
  if (!this->llcCutted) {
    if (!this->slicedStream.hasOverflowed()) {
      if (this->tailSliceIdx - lastSegmentSliceIdx >=
          this->maxNumSlicesPerSegment) {
        shouldAllocateSegment = true;
      }
    } else {
      if (this->tailSliceIdx > lastSegmentSliceIdx) {
        shouldAllocateSegment = true;
      }
    }
  } else {
    if (this->llcCutSliceIdx > lastSegmentSliceIdx) {
      shouldAllocateSegment = true;
    }
  }
  if (!shouldAllocateSegment) {
    return;
  }
  this->pushNewLLCSegment(lastSegment.endPAddr, lastSegment.endSliceIdx);
}

void MLCDynamicDirectStream::pushNewLLCSegment(Addr startPAddr,
                                               uint64_t startSliceIdx) {
  this->llcSegments.emplace_back();
  auto &segment = this->llcSegments.back();
  segment.startPAddr = startPAddr;
  segment.startSliceIdx = startSliceIdx;
  segment.sliceIds = this->nextSegmentSliceIds;
  segment.endPAddr = this->tailPAddr;
  segment.endSliceId = this->tailSliceId;
  segment.endSliceIdx = this->tailSliceIdx;
  segment.state = LLCSegmentPosition::State::ALLOCATED;

  this->nextSegmentSliceIds.clear();
  assert(!segment.sliceIds.sliceIds.empty() && "Empty segment.");
  MLC_S_DPRINTF_(StreamRangeSync, this->getDynamicStreamId(),
                 "[Commit] Push new LLC segment SliceIdx [%llu, %llu) "
                 "ElementIdx [%llu, %llu).\n",
                 segment.startSliceIdx, segment.endSliceIdx,
                 segment.sliceIds.firstSliceId().getStartIdx(),
                 segment.endSliceId.getStartIdx());
}

MLCDynamicDirectStream::LLCSegmentPosition &
MLCDynamicDirectStream::getLastLLCSegment() {
  if (this->llcSegments.empty()) {
    MLC_S_PANIC(this->getDynamicStreamId(), "Missing Last LLCSegment.");
  }
  return this->llcSegments.back();
}

const MLCDynamicDirectStream::LLCSegmentPosition &
MLCDynamicDirectStream::getLastLLCSegment() const {
  if (this->llcSegments.empty()) {
    MLC_S_PANIC(this->getDynamicStreamId(), "Missing Last LLCSegment.");
  }
  return this->llcSegments.back();
}

void MLCDynamicDirectStream::checkCoreCommitProgress() {
  auto dynS =
      this->getStaticStream()->getDynamicStream(this->getDynamicStreamId());
  if (!dynS) {
    return;
  }
  auto firstCoreElement = dynS->getFirstElement();
  uint64_t firstCoreElementIdx = 0;
  if (firstCoreElement) {
    firstCoreElementIdx = firstCoreElement->FIFOIdx.entryIdx;
  } else {
    // The core may not yet allocate new elements, we just take the next
    // FIFOIdx.
    firstCoreElementIdx = dynS->FIFOIdx.entryIdx;
    MLC_S_DPRINTF_(
        StreamRangeSync, this->getDynamicStreamId(),
        "Failed to find first core element, use DynS->FIFOIdx.entryIdx = %llu.",
        firstCoreElementIdx);
  }
  for (auto &seg : this->llcSegments) {
    if (seg.state == LLCSegmentPosition::State::ALLOCATED) {
      // We haven't even sent out the credit for this segment.
      break;
    }
    if (seg.state == LLCSegmentPosition::State::COMMITTING ||
        seg.state == LLCSegmentPosition::State::COMMITTED) {
      // We already commit this segment.
      continue;
    }
    bool isLastElement = firstCoreElementIdx == dynS->getTotalTripCount();
    if (isLastElement) {
      /**
       * Due to StreamLoopBound, we may not know the TotalTripCount and
       * allocated more elements. This cause the LastSegement never committing
       * as it keeps waiting for the core to commit those elements beyond the
       * TotalTripCount.
       *
       * We check if this is the LastSegment and the LastElement, and override
       * the EndSliceId.startIdx so that the LastSegment can correctly start to
       * commit in the following logic.
       * TODO: Handle this case in a more elegant and systematic way.
       */
      auto segStartElementIdx = seg.getStartSliceId().getStartIdx();
      auto segEndElementIdx = seg.endSliceId.getStartIdx();
      bool isLastSegment = firstCoreElementIdx > segStartElementIdx &&
                           firstCoreElementIdx < segEndElementIdx;
      if (isLastSegment) {
        MLC_S_DPRINTF_(StreamRangeSync, this->getDynamicStreamId(),
                       "[RangeCommit] Override the LastSegment ElementRange "
                       "[%llu, %llu) -> [%llu, %llu).\n",
                       segStartElementIdx, segEndElementIdx, segStartElementIdx,
                       firstCoreElementIdx);
        seg.endSliceId.getStartIdx() = firstCoreElementIdx;
      }
    }
    if (seg.endSliceId.getStartIdx() > firstCoreElementIdx) {
      /**
       * This segment contains an element that has not been committed yet.
       * With one exception:
       * If the first CoreElementIdx == TotalTripCount, and
       * falls within this segment, then we have to commit.
       */
      break;
    }
    // This segment has been committed in core.
    // Send out StreamCommit message to LLC bank.
    this->sendCommitToLLC(seg);
    seg.state = LLCSegmentPosition::State::COMMITTING;
  }
  // As a final touch, we release segment if we are done.
  while (this->llcSegments.size() > 1 &&
         this->llcSegments.front().state ==
             LLCSegmentPosition::State::COMMITTED) {
    this->llcSegments.pop_front();
  }
}

void MLCDynamicDirectStream::sendCommitToLLC(
    const LLCSegmentPosition &segment) {
  auto llcPAddrLine = makeLineAddress(segment.startPAddr);
  auto llcBank = this->mapPAddrToLLCBank(llcPAddrLine);

  // Send the commit control.
  auto startElementIdx = segment.sliceIds.firstSliceId().getStartIdx();
  auto endElementIdx = segment.endSliceId.getStartIdx();
  MLC_S_DPRINTF_(StreamRangeSync, this->dynamicStreamId,
                 "[Range] Commit [%llu, %lu), to LLC%d.\n", startElementIdx,
                 endElementIdx, llcBank.num);
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = llcPAddrLine;
  msg->m_Type = CoherenceRequestType_STREAM_COMMIT;
  msg->m_XXNewRewquestor.add(this->controller->getMachineID());
  msg->m_Destination.add(llcBank);
  msg->m_MessageSize = MessageSizeType_Control;
  DynamicStreamSliceId sliceId;
  sliceId.getDynStreamId() = this->dynamicStreamId;
  sliceId.getStartIdx() = startElementIdx;
  sliceId.getEndIdx() = endElementIdx;
  msg->m_sliceIds.add(sliceId);

  Cycles latency(1); // Just use 1 cycle latency here.

  bool ideaStreamCommit = false;
  if (this->controller->isStreamIdeaFlowEnabled()) {
    ideaStreamCommit = true;
  } else if (this->stream->isDirectLoadStream() &&
             this->indirectStreams.empty()) {
    ideaStreamCommit = true;
  }

  if (ideaStreamCommit) {
    auto llcController = this->controller->getController(llcBank);
    auto llcSE = llcController->getLLCStreamEngine();
    llcSE->receiveStreamCommit(sliceId);
  } else {
    this->requestToLLCMsgBuffer->enqueue(
        msg, this->controller->clockEdge(),
        this->controller->cyclesToTicks(latency));
  }
}

void MLCDynamicDirectStream::receiveStreamDone(
    const DynamicStreamSliceId &sliceId) {
  // Search for the segment.
  bool foundSegement = false;
  for (auto &segment : this->llcSegments) {
    if (segment.getStartSliceId().getStartIdx() == sliceId.getStartIdx() &&
        segment.endSliceId.getStartIdx() == sliceId.getEndIdx()) {
      // Found the segment.
      if (segment.state != LLCSegmentPosition::State::COMMITTING) {
        MLC_S_PANIC(this->getDynamicStreamId(),
                    "Receive StreamDone when we are not committing.");
      }
      segment.state = LLCSegmentPosition::State::COMMITTED;
      this->scheduleAdvanceStream();
      for (auto dynIS : this->indirectStreams) {
        dynIS->scheduleAdvanceStream();
      }
      foundSegement = true;
      break;
    }
  }
  if (!foundSegement) {
    MLC_S_PANIC(this->getDynamicStreamId(),
                "Failed to find the LLCSegment for StreamDone [%llu, %llu).",
                sliceId.getStartIdx(), sliceId.getEndIdx());
  }
  // Notify the Core about the StreamDone.
  // We use extra loop here to make sure the core is get notified in-order.
  if (auto dynS = this->getStaticStream()->getDynamicStream(
          this->getDynamicStreamId())) {
    for (auto &segment : this->llcSegments) {
      if (segment.state != LLCSegmentPosition::State::COMMITTED) {
        break;
      }
      if (dynS->getNextCacheDoneElemIdx() < segment.endSliceId.getStartIdx()) {
        MLC_S_DPRINTF_(StreamRangeSync, this->getDynamicStreamId(),
                       "[Commit] Notify the Core StreamDone until %llu.\n",
                       segment.endSliceId.getStartIdx());
        dynS->setNextCacheDoneElemIdx(segment.endSliceId.getStartIdx());
      }
    }
  }
}

void MLCDynamicDirectStream::setTotalTripCount(int64_t totalTripCount,
                                               Addr brokenPAddr) {
  MLC_S_DPRINTF_(MLCStreamLoopBound, this->getDynamicStreamId(),
                 "[LoopBound] Set TotalTripCount %lld. BrokenPAddr %#x.\n",
                 totalTripCount, brokenPAddr);
  this->slicedStream.setTotalTripCount(totalTripCount);
  this->llcStreamLoopBoundCutted = true;
  this->llcStreamLoopBoundBrokenPAddr = brokenPAddr;
}

Addr MLCDynamicDirectStream::getLLCTailPAddr() const {
  /**
   * Normally we just get LastLLCSegment.endPAddr, however things get
   * complicated with StreamLoopBound, as we may have allocated more
   * credits and the LLCStream may stop iterating before consuming all
   * of our credits.
   * For such cutted streams, we directly query the LLCStream's location.
   */
  if (this->llcStreamLoopBoundCutted) {
    return this->llcStreamLoopBoundBrokenPAddr;
  } else {
    return this->getLastLLCSegment().endPAddr;
  }
}