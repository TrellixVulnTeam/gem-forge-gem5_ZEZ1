from m5.params import *

from BaseCPU import BaseCPU
from FUPool import DefaultFUPool
from Process import EmulatedDriver


class LLVMTraceCPU(BaseCPU):
    type = 'LLVMTraceCPU'
    cxx_header = 'cpu/llvm_trace/llvm_trace_cpu.hh'

    traceFile = Param.String('', 'The input llvm trace file.')
    driver = Param.LLVMTraceCPUDriver('The driver to control this cpu.')
    maxFetchQueueSize = Param.UInt64(32, 'Maximum size of the fetch queue.')
    maxReorderBufferSize = Param.UInt64(8, 'Maximum size of the rob.')
    maxInstructionQueueSize = Param.UInt64(
        8, 'Maximum size of the instruction queue.')

    fetchToDecodeDelay = Param.Cycles(1, "Fetch to decode delay")
    fetchWidth = Param.Unsigned(8, "Fetch width")
    decodeToRenameDelay = Param.Cycles(1, "Decode to rename delay")
    decodeWidth = Param.Unsigned(8, "Decode width")
    decodeQueueSize = Param.Unsigned(32, "Decode queue size")
    renameToIEWDelay = Param.Cycles(
        2, "Rename to Issue/Execute/Writeback delay")
    renameWidth = Param.Unsigned(8, "Rename width")
    robSize = Param.Unsigned(32, "ROB size")
    iewToCommitDelay = Param.Cycles(
        1, "Issue/Execute/Writeback to commit delay")
    issueWidth = Param.Unsigned(8, "Issue width")
    instQueueSize = Param.Unsigned(64, "Inst queue size")
    commitWidth = Param.Unsigned(8, "Commit width")

    fuPool = Param.FUPool(DefaultFUPool(), "Functional Unit pool")

    @classmethod
    def memory_mode(cls):
        return 'timing'

    @classmethod
    def require_caches(cls):
        return False

    @classmethod
    def support_take_over(cls):
        return False


class LLVMTraceCPUDriver(EmulatedDriver):
    type = 'LLVMTraceCPUDriver'
    cxx_header = 'cpu/llvm_trace/llvm_trace_cpu_driver.hh'
