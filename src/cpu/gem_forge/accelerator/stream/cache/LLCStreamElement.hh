#ifndef __CPU_TDG_ACCELERATOR_LLC_STREAM_ELEMENT_H__
#define __CPU_TDG_ACCELERATOR_LLC_STREAM_ELEMENT_H__

#include "LLCStreamSlice.hh"

#include "cpu/gem_forge/accelerator/stream/stream.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/slicc_interface/AbstractStreamAwareController.hh"

#include <memory>

struct LLCStreamElement;
using LLCStreamElementPtr = std::shared_ptr<LLCStreamElement>;
using ConstLLCStreamElementPtr = std::shared_ptr<const LLCStreamElement>;

class LLCStreamElement {
public:
  /**
   * This represents the basic unit of LLCStreamElement.
   * It remembers the base elements it depends on. Since this can be a
   * remote LLCDynamicStream sending here, we do not remember LLCDynamicStream
   * in the element, but just the DynamicStreamId and the StaticStream.
   */
  LLCStreamElement(Stream *_S, AbstractStreamAwareController *_mlcController,
                   const DynamicStreamId &_dynStreamId, uint64_t _idx,
                   Addr _vaddr, int _size);

  Stream *S;
  AbstractStreamAwareController *mlcController;
  const DynamicStreamId &dynStreamId;
  const uint64_t idx;
  const int size;
  Addr vaddr = 0;

  int curLLCBank() const;

  std::vector<LLCStreamElementPtr> baseElements;
  bool areBaseElementsReady() const {
    for (const auto &baseElement : this->baseElements) {
      if (!baseElement->isReady()) {
        return false;
      }
    }
    return true;
  }
  StreamValue getBaseStreamValue(uint64_t baseStreamId) {
    for (const auto &baseE : this->baseElements) {
      int32_t offset;
      int32_t size;
      if (baseE->S->tryGetCoalescedOffsetAndSize(baseStreamId, offset, size)) {
        // Found it.
        return baseE->getValue(offset, size);
      }
    }
    assert(false && "Invalid baseStreamId.");
    return StreamValue();
  };

  /*************************************************
   * Accessors to the data.
   *************************************************/
  bool isReady() const { return this->readyBytes == this->size; }
  bool isComputationScheduled() const { return this->computationScheduled; }
  void scheduledComputation() {
    assert(!this->computationScheduled && "Computation already scheduled.");
    this->computationScheduled = true;
  }

  StreamValue getValue(int offset = 0, int size = sizeof(StreamValue)) const;
  uint8_t *getUInt8Ptr(int offset = 0);
  const uint8_t *getUInt8Ptr(int offset = 0) const;
  uint64_t getUInt64() const {
    assert(this->isReady());
    assert(this->size <= sizeof(uint64_t));
    return this->value.front();
  }
  uint64_t getUInt64ByStreamId(uint64_t streamId) const;

  void setValue(const StreamValue &value);

  void extractElementDataFromSlice(GemForgeCPUDelegator *cpuDelegator,
                                   const DynamicStreamSliceId &sliceId,
                                   const DataBlock &dataBlock);

  /**
   * Helper function to compute the overlap between the a range and the element.
   * @return: the size of the overlap.
   */
  int computeOverlap(Addr rangeVAddr, int rangeSize, int &rangeOffset,
                     int &elementOffset) const;

  void addSlice(LLCStreamSlicePtr &slice);
  int getNumSlices() const { return this->numSlices; }

private:
  int readyBytes;
  bool computationScheduled = false;
  static constexpr int MAX_SIZE = 128;
  std::array<uint64_t, MAX_SIZE> value;

  static constexpr int MAX_SLICES_PER_ELEMENT = 2;
  std::array<LLCStreamSlicePtr, MAX_SLICES_PER_ELEMENT> slices;
  int numSlices = 0;
};

#endif