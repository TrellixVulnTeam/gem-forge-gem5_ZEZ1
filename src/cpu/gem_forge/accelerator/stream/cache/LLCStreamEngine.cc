
#include "LLCStreamEngine.hh"

#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

// Generated by slicc.
#include "mem/ruby/protocol/StreamMigrateRequestMsg.hh"

#include "cpu/gem_forge/llvm_trace_cpu.hh"

#include "base/trace.hh"
#include "debug/LLCRubyStream.hh"

#define LLCSE_DPRINTF(format, args...)                                         \
  DPRINTF(LLCRubyStream, "[LLC_SE%d]: " format,                                \
          this->controller->getMachineID().num, ##args)

#define LLC_STREAM_DPRINTF(streamId, format, args...)                          \
  DPRINTF(LLCRubyStream, "[LLC_SE%d][%lu-%d]: " format,                        \
          this->controller->getMachineID().num, (streamId).staticId,           \
          (streamId).streamInstance, ##args)

#define LLC_ELEMENT_DPRINTF(streamId, startIdx, numElements, format, args...)  \
  DPRINTF(LLCRubyStream, "[LLC_SE%d][%lu-%d][%lu, +%d): " format,              \
          this->controller->getMachineID().num, (streamId).staticId,           \
          (streamId).streamInstance, startIdx, numElements, ##args)

LLCStreamEngine::LLCStreamEngine(AbstractStreamAwareController *_controller,
                                 MessageBuffer *_streamMigrateMsgBuffer,
                                 MessageBuffer *_streamIssueMsgBuffer,
                                 MessageBuffer *_streamIndirectIssueMsgBuffer)
    : Consumer(_controller), controller(_controller),
      streamMigrateMsgBuffer(_streamMigrateMsgBuffer),
      streamIssueMsgBuffer(_streamIssueMsgBuffer),
      streamIndirectIssueMsgBuffer(_streamIndirectIssueMsgBuffer),
      issueWidth(1), migrateWidth(1), maxInflyRequests(8),
      maxInqueueRequests(2) {}

LLCStreamEngine::~LLCStreamEngine() {
  for (auto &s : this->streams) {
    delete s;
    s = nullptr;
  }
  this->streams.clear();
}

void LLCStreamEngine::receiveStreamConfigure(PacketPtr pkt) {
  auto streamConfigureData = *(pkt->getPtr<CacheStreamConfigureData *>());
  LLCSE_DPRINTF("Received Pkt %#x, StreamConfigure %#x, initVAddr "
                "%#x, "
                "initPAddr %#x.\n",
                pkt, streamConfigureData, streamConfigureData->initVAddr,
                streamConfigureData->initPAddr);
  // Create the stream.
  auto stream = new LLCDynamicStream(streamConfigureData);

  // Check if we have indirect streams.
  if (streamConfigureData->indirectStreamConfigure != nullptr) {
    // Let's create an indirect stream.
    streamConfigureData->indirectStreamConfigure->initAllocatedIdx =
        streamConfigureData->initAllocatedIdx;
    auto indirectStream = new LLCDynamicStream(
        streamConfigureData->indirectStreamConfigure.get());
    stream->indirectStreams.push_back(indirectStream);
  }

  this->streams.emplace_back(stream);
  // Release memory.
  delete streamConfigureData;
  delete pkt;

  // Let's schedule a wakeup event.
  this->scheduleEvent(Cycles(1));
}

void LLCStreamEngine::receiveStreamEnd(PacketPtr pkt) {
  auto endStreamDynamicId = *(pkt->getPtr<DynamicStreamId *>());
  LLC_STREAM_DPRINTF(*endStreamDynamicId, "Received StreamEnd.\n");
  // Search for this stream.
  for (auto streamIter = this->streams.begin(), streamEnd = this->streams.end();
       streamIter != streamEnd; ++streamIter) {
    auto &stream = *streamIter;
    if (stream->getDynamicStreamId() == (*endStreamDynamicId)) {
      // Found it.
      // ? Can we just sliently release it?
      LLC_STREAM_DPRINTF(*endStreamDynamicId, "Ended.\n");
      delete stream;
      stream = nullptr;
      this->streams.erase(streamIter);
      return;
    }
  }
  /**
   * ? No need to search in migratingStreams?
   * For migrating streams, the end message should be sent to the destination
   * llcBank.
   */

  /**
   * If not found, it is similar case as stream flow control message.
   * We are waiting for the stream to migrate here.
   * Add the message to the pending
   */
  this->pendingStreamEndMsgs.insert(*endStreamDynamicId);

  // Don't forgot to release the memory.
  delete endStreamDynamicId;
  delete pkt;
}

void LLCStreamEngine::receiveStreamMigrate(LLCDynamicStreamPtr stream) {

  // Sanity check.
  Addr vaddr = stream->peekVAddr();
  Addr paddr = stream->translateToPAddr(vaddr);
  Addr paddrLine = makeLineAddress(paddr);
  assert(this->isPAddrHandledByMe(paddrLine) &&
         "Stream migrated to wrong LLC bank.\n");

  assert(stream->waitingDataBaseRequests == 0 &&
         "Stream migrated with waitingDataBaseRequests.");
  assert(stream->waitingIndirectElements.empty() &&
         "Stream migrated with waitingIndirectElements.");
  assert(stream->readyIndirectElements.empty() &&
         "Stream migrated with readyIndirectElements.");

  LLC_STREAM_DPRINTF(stream->getDynamicStreamId(), "Received migrate.\n");

  // Check for if the stream is already ended.
  if (this->pendingStreamEndMsgs.count(stream->getDynamicStreamId())) {
    LLC_STREAM_DPRINTF(stream->getDynamicStreamId(), "Ended.\n");
    delete stream;
    return;
  }

  this->streams.push_back(stream);
  this->scheduleEvent(Cycles(1));
}

void LLCStreamEngine::receiveStreamFlow(const DynamicStreamSliceId &sliceId) {
  // Simply append it to the list.
  LLCSE_DPRINTF("Received stream flow [%lu, +%lu).\n", sliceId.startIdx,
                sliceId.getNumElements());
  this->pendingStreamFlowControlMsgs.push_back(sliceId);
  this->scheduleEvent(Cycles(1));
}

void LLCStreamEngine::receiveStreamElementData(
    const DynamicStreamSliceId &sliceId) {
  // Search through the direct streams.
  LLCDynamicStream *stream = nullptr;
  for (auto S : this->streams) {
    if (S->configData.dynamicId == sliceId.streamId) {
      stream = S;
      break;
    }
  }
  /**
   * Since we notify the stream engine for all stream data,
   * it is possible that we don't find the stream if it is indirect stream.
   * Ignore it in such case.
   */
  if (stream == nullptr) {
    return;
  }

  stream->waitingDataBaseRequests--;
  assert(stream->waitingDataBaseRequests >= 0 &&
         "Negative waitingDataBaseRequests.");

  LLC_ELEMENT_DPRINTF(stream->getDynamicStreamId(), sliceId.startIdx,
                      sliceId.getNumElements(), "Received element data.\n");
  /**
   * It is also possible that this stream doesn't have indirect streams.
   */
  if (stream->indirectStreams.empty()) {
    return;
  }
  for (auto idx = sliceId.startIdx, endIdx = sliceId.endIdx; idx < endIdx;
       ++idx) {
    assert(stream->waitingIndirectElements.count(idx) == 1 &&
           "There is no waiting indirect element for this index.");
    // Add them to the ready indirect list.
    for (auto indirectStream : stream->indirectStreams) {
      // If the indirect stream is behind one iteration, base element of
      // iteration 0 should trigger the indirect element of iteration 1.
      if (indirectStream->isOneIterationBehind()) {
        stream->readyIndirectElements.emplace(idx + 1, indirectStream);
      } else {
        stream->readyIndirectElements.emplace(idx, indirectStream);
      }
    }
    // Don't forget to erase it from the waiting list.
    stream->waitingIndirectElements.erase(idx);
  }
}

bool LLCStreamEngine::canMigrateStream(LLCDynamicStream *stream) const {
  /**
   * In this implementation, the LLC stream will aggressively
   * migrate to the next element bank, even the credit has only been allocated
   * to the previous element. Therefore, we do not need to check if the next
   * element is allocated.
   */
  auto nextVAddr = stream->peekVAddr();
  auto nextPAddr = stream->translateToPAddr(nextVAddr);
  // Check if it is still on this bank.
  if (this->isPAddrHandledByMe(nextPAddr)) {
    // Still here.
    return false;
  }
  if (!stream->waitingIndirectElements.empty()) {
    // We are still waiting data for indirect streams.
    return false;
  }
  if (!stream->readyIndirectElements.empty()) {
    // We are still waiting for some indirect streams to be issued.
    return false;
  }
  /**
   * Enforce that pointer chase stream can not migrate until the previous
   * base request comes back.
   */
  if (stream->isPointerChase() && stream->waitingDataBaseRequests > 0) {
    return false;
  }
  return true;
}

void LLCStreamEngine::wakeup() {

  // Sanity check.
  if (this->streams.size() >= 1000) {
    panic("Too many LLCStream.\n");
  }

  this->processStreamFlowControlMsg();
  this->issueStreams();
  this->migrateStreams();
  if (!this->streams.empty() || !this->migratingStreams.empty()) {
    this->scheduleEvent(Cycles(1));
  }
}

void LLCStreamEngine::processStreamFlowControlMsg() {
  auto iter = this->pendingStreamFlowControlMsgs.begin();
  auto end = this->pendingStreamFlowControlMsgs.end();
  while (iter != end) {
    const auto &msg = *iter;
    bool processed = false;
    for (auto stream : this->streams) {
      if (stream->getDynamicStreamId() == msg.streamId &&
          msg.startIdx == stream->allocatedIdx) {
        // We found it.
        // Update the idx.
        LLC_STREAM_DPRINTF(stream->getDynamicStreamId(),
                           "Add credit %lu -> %lu.\n", msg.startIdx,
                           msg.endIdx);
        stream->addCredit(msg.getNumElements());
        processed = true;
        break;
      }
    }
    if (processed) {
      iter = this->pendingStreamFlowControlMsgs.erase(iter);
    } else {
      // LLCSE_DPRINTF("Failed to process stream credit %s [%lu, %lu).\n",
      //               msg.streamId.name.c_str(), msg.startIdx, msg.endIdx);
      ++iter;
    }
  }
}

void LLCStreamEngine::issueStreams() {

  /**
   * Enforce thresholds for issue stream requests here.
   * 1. If there are many requests in the queue, there is no need to inject more
   * packets to block the queue.
   * 2. As a sanity check, we limit the total number of infly direct requests.
   */

  if (this->streamIssueMsgBuffer->getSize(this->controller->clockEdge()) >=
      this->maxInqueueRequests) {
    return;
  }

  auto streamIter = this->streams.begin();
  auto streamEnd = this->streams.end();
  StreamList issuedStreams;
  while (streamIter != streamEnd && issuedStreams.size() < this->issueWidth) {
    auto stream = *streamIter;
    bool issued = this->issueStream(stream);
    if (issued) {
      // Check if we want to migrate the stream.
      issuedStreams.emplace_back(stream);
      streamIter = this->streams.erase(streamIter);
    } else {
      // Move to the next one.
      ++streamIter;
    }
  }

  /**
   * Previously I only check issuedStreams for migration.
   * This assumes we only need to check migration possibility after issuing.
   * However, for pointer chase stream without indirect streams, this is not the
   * case. It maybe come migration target after receiving the previous stream
   * element data. Therefore, here I rescan all the streams to avoid deadlock.
   */

  // Insert the issued streams to the back of streams list.
  for (auto stream : issuedStreams) {
    this->streams.emplace_back(stream);
  }

  // Scan all streams for migration target.
  streamIter = this->streams.begin();
  streamEnd = this->streams.end();
  while (streamIter != streamEnd) {
    auto stream = *streamIter;
    if (this->canMigrateStream(stream)) {
      this->migratingStreams.emplace_back(stream);
      streamIter = this->streams.erase(streamIter);
    } else {
      ++streamIter;
    }
  }
}

bool LLCStreamEngine::issueStream(LLCDynamicStream *stream) {

  /**
   * Prioritize indirect elements.
   */
  if (this->issueStreamIndirect(stream)) {
    // We successfully issued an indirect element of this stream.
    // We are done.
    return true;
  }

  /**
   * After this point, try to issue base stream element.
   */

  // Enforce the per stream maxWaitingDataBaseRequests constraint.
  if (stream->waitingDataBaseRequests == stream->maxWaitingDataBaseRequests) {
    return false;
  }

  /**
   * Key point is to merge continuous stream elements within one cache line.
   * TODO: Really check if continuous. So far just consume until a different
   * TODO: cache line.
   */
  if (!stream->isNextElementAllcoated()) {
    return false;
  }

  // Get the first element.
  int numElements = 0;
  Addr vaddr = stream->peekVAddr();
  Addr paddr = stream->translateToPAddr(vaddr);
  Addr paddrLine = makeLineAddress(paddr);

  /**
   * Due to the waiting indirect element, a stream may not be migrated
   * immediately after the stream engine found the next element is not
   * handled here. In such case, we simply give up and return false.
   */
  if (!this->isPAddrHandledByMe(paddr)) {
    return false;
  }

  auto startIdx = stream->consumeNextElement();
  numElements = 1;

  // Try to get more elements if this is not pointer chase stream.
  if (!stream->isPointerChase()) {
    while (stream->isNextElementAllcoated() &&
           stream->isNextElementWithinHistory()) {
      Addr nextVAddr = stream->peekVAddr();
      Addr nextPAddr = stream->translateToPAddr(nextVAddr);
      Addr nextPAddrLine = makeLineAddress(nextPAddr);
      if (nextPAddrLine == paddrLine) {
        // We can merge the request.
        stream->consumeNextElement();
        numElements++;
      } else {
        break;
      }
    }
  }

  // Register the waiting indirect elements.
  if (!stream->indirectStreams.empty()) {
    for (auto idx = startIdx; idx < startIdx + numElements; ++idx) {
      stream->waitingIndirectElements.insert(idx);
    }
  }

  this->issueStreamRequestHere(stream, paddrLine, startIdx, numElements);

  stream->waitingDataBaseRequests++;

  return true;
}

bool LLCStreamEngine::issueStreamIndirect(LLCDynamicStream *stream) {
  if (stream->readyIndirectElements.empty()) {
    // There is no ready indirect element to be issued.
    return false;
  }

  // Try to issue one with lowest element index.
  auto firstIndirectIter = stream->readyIndirectElements.begin();
  auto idx = firstIndirectIter->first;
  auto indirectStream = firstIndirectIter->second;

  Addr vaddr = indirectStream->getVAddr(idx);
  Addr paddr = indirectStream->translateToPAddr(vaddr);
  Addr paddrLine = makeLineAddress(paddr);

  /**
   * It's possible that the element is not handled here.
   */
  if (!this->isPAddrHandledByMe(paddr)) {
    // Send to other bank.
    this->issueStreamRequestThere(indirectStream, paddrLine, idx,
                                  1 /* numElements */);
  } else {
    // Send to this bank;
    this->issueStreamRequestHere(indirectStream, paddrLine, idx,
                                 1 /* numElements */);
  }

  // Don't forget to release the element.
  stream->readyIndirectElements.erase(firstIndirectIter);
  return true;
}

void LLCStreamEngine::issueStreamRequestHere(LLCDynamicStream *stream,
                                             Addr paddrLine, uint64_t startIdx,
                                             int numElements) {
  LLC_ELEMENT_DPRINTF(stream->getDynamicStreamId(), startIdx, numElements,
                      "Issue [local] request.\n");

  auto selfMachineId = this->controller->getMachineID();
  auto streamCPUId = stream->getStaticStream()->getCPUDelegator()->cpuId();
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Type = CoherenceRequestType_GETU;
  msg->m_Requestor =
      MachineID(static_cast<MachineType>(selfMachineId.type - 1), streamCPUId);
  msg->m_Destination.add(selfMachineId);
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_sliceId.streamId = stream->getDynamicStreamId();
  msg->m_sliceId.startIdx = startIdx;
  msg->m_sliceId.endIdx = startIdx + numElements;
  msg->m_sliceId.sizeInByte = numElements * stream->getElementSize();

  Cycles latency(1); // Just use 1 cycle latency here.

  this->streamIssueMsgBuffer->enqueue(msg, this->controller->clockEdge(),
                                      this->controller->cyclesToTicks(latency));
}

void LLCStreamEngine::issueStreamRequestThere(LLCDynamicStream *stream,
                                              Addr paddrLine, uint64_t startIdx,
                                              int numElements) {
  auto addrMachineId = this->mapPaddrToLLCBank(paddrLine);
  LLC_ELEMENT_DPRINTF(stream->getDynamicStreamId(), startIdx, numElements,
                      "Issue [remote] request to LLC%d.\n", addrMachineId.num);

  auto selfMachineId = this->controller->getMachineID();
  auto streamCPUId = stream->getStaticStream()->getCPUDelegator()->cpuId();
  auto msg = std::make_shared<RequestMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Type = CoherenceRequestType_GETU;
  msg->m_Requestor =
      MachineID(static_cast<MachineType>(selfMachineId.type - 1), streamCPUId);
  msg->m_Destination.add(addrMachineId);
  msg->m_MessageSize = MessageSizeType_Control;
  msg->m_sliceId.streamId = stream->getDynamicStreamId();
  msg->m_sliceId.startIdx = startIdx;
  msg->m_sliceId.endIdx = startIdx + numElements;
  msg->m_sliceId.sizeInByte = numElements * stream->getElementSize();

  Cycles latency(1); // Just use 1 cycle latency here.

  this->streamIndirectIssueMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

void LLCStreamEngine::migrateStreams() {
  auto streamIter = this->migratingStreams.begin();
  auto streamEnd = this->migratingStreams.end();
  int migrated = 0;
  while (streamIter != streamEnd && migrated < this->migrateWidth) {
    auto stream = *streamIter;
    assert(this->canMigrateStream(stream) && "Can't migrate stream.");
    // We do not migrate the stream if it has unprocessed indirect elements.
    this->migrateStream(stream);
    streamIter = this->migratingStreams.erase(streamIter);
    migrated++;
  }
}

void LLCStreamEngine::migrateStream(LLCDynamicStream *stream) {

  // Remember to clear the waitingDataBaseRequests becase we may aggressively
  // migrate direct streams (not pointer chase).
  stream->waitingDataBaseRequests = 0;

  // Create the migrate request.
  Addr vaddr = stream->peekVAddr();
  Addr paddr = stream->translateToPAddr(vaddr);
  Addr paddrLine = makeLineAddress(paddr);
  auto selfMachineId = this->controller->getMachineID();
  auto addrMachineId =
      this->controller->mapAddressToLLC(paddrLine, selfMachineId.type);

  LLC_STREAM_DPRINTF(stream->getDynamicStreamId(), "Migrate to LLC%d.\n",
                     addrMachineId.num);

  auto msg =
      std::make_shared<StreamMigrateRequestMsg>(this->controller->clockEdge());
  msg->m_addr = paddrLine;
  msg->m_Type = CoherenceRequestType_GETS;
  msg->m_Requestor = selfMachineId;
  msg->m_Destination.add(addrMachineId);
  msg->m_MessageSize = MessageSizeType_Data;
  msg->m_Stream = stream;

  Cycles latency(1); // Just use 1 cycle latency here.

  this->streamMigrateMsgBuffer->enqueue(
      msg, this->controller->clockEdge(),
      this->controller->cyclesToTicks(latency));
}

MachineID LLCStreamEngine::mapPaddrToLLCBank(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto addrMachineId =
      this->controller->mapAddressToLLC(paddr, selfMachineId.type);
  return addrMachineId;
}

bool LLCStreamEngine::isPAddrHandledByMe(Addr paddr) const {
  auto selfMachineId = this->controller->getMachineID();
  auto addrMachineId =
      this->controller->mapAddressToLLC(paddr, selfMachineId.type);
  return addrMachineId == selfMachineId;
}

void LLCStreamEngine::print(std::ostream &out) const {}

void LLCStreamEngine::receiveStreamIndirectRequest(const RequestMsg &req) {
  // Simply copy and inject the msg to L1 request in.
  const auto &sliceId = req.m_sliceId;
  assert(sliceId.isValid() && "Invalid stream slice for indirect request.");

  auto startIdx = sliceId.startIdx;
  auto numElements = sliceId.getNumElements();
  LLC_ELEMENT_DPRINTF(sliceId.streamId, startIdx, numElements,
                      "Inject [remote] request.\n");

  auto msg = std::make_shared<RequestMsg>(req);
  Cycles latency(1); // Just use 1 cycle latency here.

  this->streamIssueMsgBuffer->enqueue(msg, this->controller->clockEdge(),
                                      this->controller->cyclesToTicks(latency));
}