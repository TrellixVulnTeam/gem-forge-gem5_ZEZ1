#ifndef __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_STREAM_H__
#define __CPU_TDG_ACCELERATOR_STREAM_MLC_DYNAMIC_STREAM_H__

#include "DynamicStreamAddressRange.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"

#include "mem/ruby/common/DataBlock.hh"

// Generated by slicc.
#include "mem/ruby/protocol/ResponseMsg.hh"

#include <list>

class AbstractStreamAwareController;
class MessageBuffer;

class MLCDynamicStream {
public:
  MLCDynamicStream(CacheStreamConfigureDataPtr _configData,
                   AbstractStreamAwareController *_controller,
                   MessageBuffer *_responseMsgBuffer,
                   MessageBuffer *_requestToLLCMsgBuffer);

  virtual ~MLCDynamicStream();

  Stream *getStaticStream() const { return this->stream; }

  const DynamicStreamId &getDynamicStreamId() const {
    return this->dynamicStreamId;
  }

  bool getIsPseudoOffload() const { return this->isPseudoOffload; }

  virtual const DynamicStreamId &getRootDynamicStreamId() const {
    // By default this we are the root stream.
    return this->getDynamicStreamId();
  }

  /**
   * Helper function to check if a slice is valid within this stream context.
   * So far always valid, except the first element of indirect stream that is
   * behind by one iteration.
   */
  virtual bool isSliceValid(const DynamicStreamSliceId &sliceId) const {
    return true;
  }

  /**
   * Get where is the LLC stream is at the end of current allocated credits.
   */
  virtual Addr getLLCTailPAddr() const {
    panic("Should only call this on direct stream.");
  }

  virtual void receiveStreamData(const DynamicStreamSliceId &sliceId,
                                 const DataBlock &dataBlock,
                                 Addr paddrLine) = 0;
  void receiveStreamRequest(const DynamicStreamSliceId &sliceId);
  void receiveStreamRequestHit(const DynamicStreamSliceId &sliceId);

  /**
   * Before end the stream, we have make dummy response to the request
   * we have seen to make the ruby system happy.
   */
  void endStream();

  uint64_t getHeadSliceIdx() const { return this->headSliceIdx; }
  uint64_t getTailSliceIdx() const { return this->tailSliceIdx; }

  void receiveStreamRange(const DynamicStreamAddressRangePtr &range);
  virtual void receiveStreamDone(const DynamicStreamSliceId &sliceId);

  void scheduleAdvanceStream();

  /**
   * Whether this stream requires range-based syncrhonization.
   */
  bool shouldRangeSync() const { return this->config->rangeSync; }

  const std::vector<CacheStreamConfigureDataPtr> &getSendToConfigs() const {
    return this->sendToConfigs;
  }

protected:
  Stream *stream;
  DynamicStreamId dynamicStreamId;
  CacheStreamConfigureDataPtr config;
  bool isPointerChase;
  bool isPseudoOffload;

  std::vector<CacheStreamConfigureDataPtr> sendToConfigs;

  AbstractStreamAwareController *controller;
  MessageBuffer *responseMsgBuffer;
  MessageBuffer *requestToLLCMsgBuffer;
  uint64_t maxNumSlices;

  /**
   * Represent an allocated stream slice at MLC.
   * Used as a meeting point for the request from core
   * and data from LLC stream engine.
   */
  struct MLCStreamSlice {
    DynamicStreamSliceId sliceId;
    DataBlock dataBlock;
    // Whether the core's request is already here.
    bool dataReady;
    enum CoreStatusE {
      NONE,
      WAIT_DATA, // The core is waiting the data.
      WAIT_ACK,  // The core is waiting the ack.
      ACK_READY, // The ack is ready, waiting to be reported to core in order.
      DONE,
      FAULTED
    };
    CoreStatusE coreStatus;
    // For debug purpose, we also remember core's request sliceId.
    DynamicStreamSliceId coreSliceId;
    // Statistics.
    Cycles dataReadyCycle;
    Cycles coreWaitCycle;

    MLCStreamSlice(const DynamicStreamSliceId &_sliceId)
        : sliceId(_sliceId), dataBlock(), dataReady(false),
          coreStatus(CoreStatusE::NONE) {}

    void setData(const DataBlock &dataBlock, Cycles currentCycle) {
      assert(!this->dataReady && "Data already ready.");
      this->dataBlock = dataBlock;
      this->dataReady = true;
      this->dataReadyCycle = currentCycle;
    }

    static std::string convertCoreStatusToString(CoreStatusE status) {
      switch (status) {
      case CoreStatusE::NONE:
        return "NONE";
      case CoreStatusE::WAIT_DATA:
        return "WAIT_DATA";
      case CoreStatusE::WAIT_ACK:
        return "WAIT_ACK";
      case CoreStatusE::ACK_READY:
        return "ACK_READY";
      case CoreStatusE::DONE:
        return "DONE";
      case CoreStatusE::FAULTED:
        return "FAULTED";
      default:
        return "ILLEGAL";
      }
    }
  };

  std::list<MLCStreamSlice> slices;
  using SliceIter = std::list<MLCStreamSlice>::iterator;
  // Slice index of allocated [head, tail).
  uint64_t headSliceIdx;
  uint64_t tailSliceIdx;

  EventFunctionWrapper advanceStreamEvent;

  virtual void advanceStream() = 0;
  void makeResponse(MLCStreamSlice &slice);
  void makeAck(MLCStreamSlice &slice);

  /**
   * API for this to check if overflowed.
   */
  virtual bool hasOverflowed() const = 0;
  virtual int64_t getTotalTripCount() const = 0;
  virtual bool matchSliceId(const DynamicStreamSliceId &A,
                            const DynamicStreamSliceId &B) const {
    // By default match the vaddr.
    // TODO: This is really wrong.
    return A.vaddr == B.vaddr;
  }
  /**
   * Find the correct slice for a core request.
   * Used in receiveStreamRequest() and receiveStreamRequestHit().
   */
  virtual SliceIter
  findSliceForCoreRequest(const DynamicStreamSliceId &sliceId) = 0;

  /**
   * Helper function to translate the vaddr to paddr.
   */
  Addr translateVAddr(Addr vaddr) const;

  /**
   * Helper function to direct read data from memory.
   */
  void readBlob(Addr vaddr, uint8_t *data, int size) const;

  /**
   * Map paddr line to LLC bank.
   */
  MachineID mapPAddrToLLCBank(Addr paddr) const;

  /**
   * Pop slices.
   */
  void popStream();

  /**
   * These function checks if we are waiting for something.
   */
  bool isWaitingAck() const {
    // This is stream is waiting for Ack, not Data.
    // So far only for offloaded store and atomicrmw streams.
    if (this->isPseudoOffload) {
      return false;
    }
    if (this->stream->isStoreStream()) {
      return true;
    } else if (this->stream->isAtomicStream() ||
               this->stream->isUpdateStream()) {
      // We need to check if there the core is issuing.
      if (auto dynS = this->stream->getDynamicStream(this->dynamicStreamId)) {
        return !dynS->shouldCoreSEIssue();
      } else {
        // The dynamic stream is already released, we don't really care.
        return false;
      }
    } else {
      // Load stream has no ack for now.
      return false;
    }
  }

  bool isWaitingData() const {
    if (this->isPseudoOffload) {
      return false;
    }
    if (this->stream->isStoreStream()) {
      return false;
    }
    if (auto dynS = this->stream->getDynamicStream(this->dynamicStreamId)) {
      return dynS->shouldCoreSEIssue();
    } else {
      // The dynamic stream is already released, we don't really care.
      // Assume it waits for data in case the core issued a request.
      return true;
    }
  }

  bool isWaitingNothing() const {
    return !this->isWaitingData() && !this->isWaitingAck();
  }

  /**
   * This remember the received StreamRange.
   */
  std::list<DynamicStreamAddressRangePtr> receivedRanges;

  /**
   * A helper function to dump some basic status of the stream when panic.
   */
  void panicDump() const;
};

#endif
