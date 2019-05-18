#include "thread_context.hh"

LLVMTraceThreadContext::LLVMTraceThreadContext(
    ContextID _contextId, const std::string &_traceFileName, bool _isIdeal)
    : contextId(_contextId), traceFileName(_traceFileName),
      dynInstStream(new DynamicInstructionStream(_traceFileName)),
      isIdeal(_isIdeal), inflyInsts(0), cpu(nullptr),
      threadId(InvalidThreadID) {}

LLVMTraceThreadContext::~LLVMTraceThreadContext() {
  delete this->dynInstStream;
  this->dynInstStream = nullptr;
}

bool LLVMTraceThreadContext::canFetch() const {
  return !this->dynInstStream->fetchEmpty();
}

LLVMDynamicInst *LLVMTraceThreadContext::fetch() {
  assert(this->canFetch() && "Illega fetch.");
  this->inflyInsts++;
  return this->dynInstStream->fetch();
}

void LLVMTraceThreadContext::commit(LLVMDynamicInst *inst) {
  this->dynInstStream->commit(inst);
  this->inflyInsts--;
}

bool LLVMTraceThreadContext::isDone() const {
  return this->dynInstStream->fetchEmpty() && this->inflyInsts == 0;
}

void LLVMTraceThreadContext::activate(LLVMTraceCPU *cpu, ThreadID threadId) {
  this->cpu = cpu;
  this->threadId = threadId;
}

void LLVMTraceThreadContext::deactivate() {
  this->cpu = nullptr;
  this->threadId = InvalidThreadID;
}