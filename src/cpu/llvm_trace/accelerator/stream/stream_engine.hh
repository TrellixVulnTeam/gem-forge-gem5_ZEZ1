#ifndef __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__
#define __CPU_TDG_ACCELERATOR_STREAM_ENGINE_H__

#include "insts.hh"
#include "stream.hh"

#include "base/statistics.hh"
#include "cpu/llvm_trace/accelerator/tdg_accelerator.hh"

#include <unordered_map>

class StreamEngine : public TDGAccelerator {
public:
  StreamEngine();
  ~StreamEngine() override;

  bool handle(LLVMDynamicInst *inst) override;
  void tick() override;
  void regStats() override;

  void useStream(uint64_t streamId, const LLVMDynamicInst *user);
  bool isStreamReady(uint64_t streamId, const LLVMDynamicInst *user) const;
  bool canStreamStep(uint64_t streamId) const;
  void commitStreamConfigure(uint64_t streamId, uint64_t configSeqNum);
  void commitStreamStep(uint64_t streamId, uint64_t stepSeqNum);
  void commitStreamStore(uint64_t streamId, uint64_t storeSeqNum);

  const Stream *getStreamNullable(uint64_t streamId) const;
  Stream *getStreamNullable(uint64_t streamId);
  /**
   * Stats
   */
  Stats::Scalar numConfigured;
  Stats::Scalar numStepped;
  Stats::Scalar numElements;
  Stats::Scalar numElementsUsed;
  Stats::Scalar entryWaitCycles;
  Stats::Scalar numMemElements;
  Stats::Scalar numMemElementsFetched;
  Stats::Scalar numMemElementsUsed;
  Stats::Scalar memEntryWaitCycles;

private:
  std::unordered_map<uint64_t, Stream> streamMap;

  Stream *getOrInitializeStream(
      const LLVM::TDG::TDGInstruction_StreamConfigExtra &configInst);
};

#endif