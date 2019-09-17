#ifndef __RISCV_GEM_FORGE_ISA_HANDLER_HH__
#define __RISCV_GEM_FORGE_ISA_HANDLER_HH__

/**
 * A place to implement the actual instruction functionality.
 */

#include "stream/riscv_stream_engine.hh"

#include <unordered_map>

namespace RiscvISA {
class GemForgeISAHandler {
public:
  bool canDispatch(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);
  void dispatch(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);
  void execute(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);
  void commit(const GemForgeDynInstInfo &dynInfo, ExecContext &xc);

private:
  enum GemForgeStaticInstOpE {
    NORMAL, // Normal instructions.
    STREAM_CONFIG,
    STREAM_END,
    STREAM_INPUT,
    STREAM_READY,
    STREAM_LOAD,
    STREAM_STEP,
  };
  struct GemForgeStaticInstInfo {
    GemForgeStaticInstOpE op;
  };
  mutable std::unordered_map<Addr, GemForgeStaticInstInfo> cachedStaticInstInfo;

  GemForgeStaticInstInfo &getStaticInstInfo(const TheISA::PCState &pcState,
                                            const GemForgeDynInstInfo &dynInfo);

  RISCVStreamEngine se;
};

} // namespace RiscvISA

#endif