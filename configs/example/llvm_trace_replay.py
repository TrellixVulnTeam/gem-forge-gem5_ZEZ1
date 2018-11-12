import optparse
import os

from m5.util import addToPath, fatal

addToPath('../')

from common import Options
from common import Simulation
from common import CacheConfig
from common import MemConfig
from common.Caches import *

parser = optparse.OptionParser()
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

parser.add_option("--llvm-trace-file", action="store", type="string",
                  help="""llvm trace file input LLVMTraceCPU""", default="")
parser.add_option("--llvm-issue-width", action="store", type="int",
                  help="""llvm issue width""", default="8")
parser.add_option("--llvm-store-queue-size", action="store",
                  type="int", help="""store queue size""", default="32")
parser.add_option("--llvm-standalone", action="store",
                  type="int", help="""replay in stand alone mode""", default="0")
parser.add_option("--llvm-prefetch", action="store", type="int",
                  help="""whether to use a prefetcher""", default="0")
parser.add_option("--llvm-mcpat", action="store", type="int", 
                  help="""whether to use mcpat to estimate power""", default="0")
parser.add_option("--gem-forge-stream-engine-max-run-ahead-length", action="store", type="int",
                  help="""How many element can a stream run ahead""", default="10")
parser.add_option("--gem-forge-stream-engine-is-oracle", action="store", type="int",
                  help="""whether make the stream engine oracle""", default="0")
parser.add_option("--gem-forge-stream-engine-throttling", action="store", type="string",
                  help="""Throttling tenchique used by stream engine.""", default="static")

(options, args) = parser.parse_args()

if args:
    fatal("Error: script doesn't take any positional arguments")


def get_processes(options):
    """Interprets provided options and returns a list of processes"""

    multiprocesses = []
    inputs = []
    outputs = []
    errouts = []
    pargs = []

    workloads = options.cmd.split(';')
    if options.input != "":
        inputs = options.input.split(';')
    if options.output != "":
        outputs = options.output.split(';')
    if options.errout != "":
        errouts = options.errout.split(';')
    if options.options != "":
        pargs = options.options.split(';')

    idx = 0
    for wrkld in workloads:
        process = Process()
        process.executable = wrkld
        process.cwd = os.getcwd()

        if len(pargs) > idx:
            process.cmd = [wrkld] + pargs[idx].split()
        else:
            process.cmd = [wrkld]

        if len(inputs) > idx:
            process.input = inputs[idx]
        if len(outputs) > idx:
            process.output = outputs[idx]
        if len(errouts) > idx:
            process.errout = errouts[idx]

        multiprocesses.append(process)
        idx += 1

    if options.smt:
        assert(options.cpu_type == "detailed" or options.cpu_type == "inorder")
        return multiprocesses, idx
    else:
        return multiprocesses, 1


if options.ruby:
    fatal("This script does not support Ruby configuration, mainly"
          " because Trace CPU has been tested only with classic memory system")

if options.cpu_type == "LLVMTraceCPU":
    fatal("The host CPU should be a normal CPU other than LLVMTraceCPU\n")

if options.num_cpus > 1:
    fatal("This script does not support multi-processor trace replay.\n")

multiprocesses, numThreads = get_processes(options)
(CPUClass, test_mem_mode, FutureClass) = Simulation.setCPUClass(options)
CPUClass.numThreads = numThreads

if options.llvm_standalone == 0:
    # Non-standalone mode, intialize the driver and normal cpu.

    # In this case FutureClass will be None as there is not fast forwarding or
    # switching
    cpus = [CPUClass(cpu_id=i) for i in xrange(options.num_cpus)]

    # Set the workload for normal CPUs.
    for i in xrange(options.num_cpus):
        if options.smt:
            cpus[i].workload = multiprocesses
        elif len(multiprocesses) == 1:
            cpus[i].workload = multiprocesses[0]
        else:
            cpus[i].workload = multiprocesses[i]
        cpus[i].createThreads()

    # Add the emulated LLVM tracer driver for the process
    if options.llvm_trace_file != '':
        for process in multiprocesses:
            driver = LLVMTraceCPUDriver()
            driver.filename = 'llvm_trace_cpu'
            process.drivers = [driver]

            # For each process, add a LLVMTraceCPU for simulation.
            llvm_trace_cpu = LLVMTraceCPU(cpu_id=len(cpus))
            llvm_trace_cpu.issueWidth = options.llvm_issue_width
            llvm_trace_cpu.storeQueueSize = options.llvm_store_queue_size
            llvm_trace_cpu.cpu_id = len(cpus)
            llvm_trace_cpu.traceFile = options.llvm_trace_file
            llvm_trace_cpu.driver = driver

            cpus.append(llvm_trace_cpu)
            options.num_cpus = len(cpus)
else:
    # Standalone mode, just the replay cpu.
    # There should be a trace file for replay.
    assert options.llvm_trace_file != ''
    cpus = list()
    # No need to worry about the process.
    for process in multiprocesses:
        driver = LLVMTraceCPUDriver()
        driver.filename = 'llvm_trace_cpu'
        process.drivers = [driver]

        # For each process, add a LLVMTraceCPU for simulation.
        llvm_trace_cpu = LLVMTraceCPU(cpu_id=len(cpus))
        llvm_trace_cpu.issueWidth = options.llvm_issue_width
        llvm_trace_cpu.storeQueueSize = options.llvm_store_queue_size
        llvm_trace_cpu.cpu_id = len(cpus)
        llvm_trace_cpu.traceFile = options.llvm_trace_file
        llvm_trace_cpu.streamEngineIsOracle = (
            options.gem_forge_stream_engine_is_oracle != 0)
        llvm_trace_cpu.streamEngineMaxRunAHeadLength = (
            options.gem_forge_stream_engine_max_run_ahead_length
        )
        llvm_trace_cpu.streamEngineThrottling = options.gem_forge_stream_engine_throttling
        # A dummy driver to make the python script happy.
        llvm_trace_cpu.driver = NULL
        cpus.append(llvm_trace_cpu)

system = System(cpu=cpus,
                mem_mode=test_mem_mode,
                mem_ranges=[AddrRange(options.mem_size)],
                cache_line_size=options.cacheline_size)

Simulation.setWorkCountOptions(system, options)

# Create a top-level voltage domain
system.voltage_domain = VoltageDomain(voltage=options.sys_voltage)

# Create a source clock for the system. This is used as the clock period for
# xbar and memory
system.clk_domain = SrcClockDomain(clock=options.sys_clock,
                                   voltage_domain=system.voltage_domain)

# Create a CPU voltage domain
system.cpu_voltage_domain = VoltageDomain()

# Create a separate clock domain for the CPUs. In case of Trace CPUs this clock
# is actually used only by the caches connected to the CPU.
system.cpu_clk_domain = SrcClockDomain(clock=options.cpu_clock,
                                       voltage_domain=system.cpu_voltage_domain)

# All cpus belong to a common cpu_clk_domain, therefore running at a common
# frequency.
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain
    if isinstance(cpu, DerivO3CPU):
        cpu.issueWidth = options.llvm_issue_width
        cpu.SQEntries = options.llvm_store_queue_size
        cpu.branchPred = LocalBP(numThreads=1)

# Assign input trace files to the Trace CPU
# system.cpu.traceFile = options.llvm_trace_file

# Configure the classic memory system options
MemClass = Simulation.setMemClass(options)
system.membus = SystemXBar()
system.system_port = system.membus.slave
CacheConfig.config_cache(options, system)
MemConfig.config_mem(options, system)

if options.llvm_prefetch == 1:
    for cpu in system.cpu:
        cpu.dcache.prefetcher = StridePrefetcher(degree=8, latency=1)
    system.l2.prefetcher = StridePrefetcher(degree=8, latency=1)

if options.llvm_mcpat == 1:
    system.mcpat_manager = McPATManager()

root = Root(full_system=False, system=system)
Simulation.run(options, root, system, FutureClass)
