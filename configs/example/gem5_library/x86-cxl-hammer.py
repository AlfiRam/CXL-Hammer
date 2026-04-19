# Copyright (c) 2026 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import m5

from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic import (
    private_l1_private_l2_shared_l3_cache_hierarchy as _phier,
)
from gem5.components.memory.dram_interfaces.ddr4 import DDR4_2400_8x8
from gem5.components.memory.memory import ChanneledMemory
from gem5.components.memory.single_channel import DIMM_DDR5_4400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import (
    DiskImageResource,
    KernelResource,
)
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(
    isa_required=ISA.X86,
    kvm_required=True,
)

import os

# resolve kernel and disk image paths relative to the repository root.
# this script lives at <repo>/configs/example/gem5_library/, so three
# dirname() calls take us back to <repo>/.
_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..")
)
_FS_FILES = os.path.join(_REPO_ROOT, "fs_files")

# MESI three-level classic hierarchy
cache_hierarchy = _phier.PrivateL1PrivateL2SharedL3CacheHierarchy(
    l1d_size="48kB",
    l1d_assoc=6,
    l1i_size="32kB",
    l1i_assoc=8,
    l2_size="2MB",
    l2_assoc=16,
    l3_size="96MB",
    l3_assoc=48,
)

# local (NUMA node 0) memory: leave as the repo default so the
# non-CXL side is unchanged versus other CXL-DMSim configs.
memory = DIMM_DDR5_4400(size="3GB")

# CXL memory: force single-channel DDR4 to keep the address mapping
# simple.
cxl_memory = ChanneledMemory(
    DDR4_2400_8x8,
    num_channels=1,
    interleaving_size=64,
    size="2GB",
)


#   "stride64"   - weak cells 64 bytes apart (each in its own SECDED
#                  word, so every flip is single-bit-correctable)
DEVICE_MAP = "stride64"

# --- rowhammer param overrides on every CXL DRAM channel -------------
# ChanneledMemory exposes its MemCtrls via get_memory_controllers();
# ctrl.dram is the DRAMInterface SimObject carrying our new params.
_RH_OVERRIDES = dict(
    enable_memory_corruption=True,
    enable_ecc=False,
    rowhammer_threshold=100,
    single_sided_prob=10,
    half_double_prob=10,
    double_sided_prob=10,
    device_file=f"util/hammersim/prob-test-{DEVICE_MAP}.json",
    rh_stat_dump=False,
    rh_stat_file="m5out/rowhammer.trace",
    trr_variant=0,
)
for ctrl in cxl_memory.get_memory_controllers():
    for _k, _v in _RH_OVERRIDES.items():
        setattr(ctrl.dram, _k, _v)
# ---------------------------------------------------------------------

# single-core switchable processor: boot on KVM, hammer on TIMING.
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)

# KVM without perf counters -- matches the no-perf reference config.
# required on WSL and other shared hosts where /proc/sys/kernel/
# perf_event_paranoid blocks perf access from a KVM guest.
for proc in processor.start:
    proc.core.usePerf = False

# CXL-capable board. is_asic=True keeps the default CXL-DMSim ASIC
# timings
board = X86Board(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_memory,
    is_asic=True,
)

# workload: the rowhammer_cxl_test binary. the first `m5
# exit` below drives the KVM -> TIMING switch before the guest
# launches the binary; the binary itself emits m5_exit(0) right
# before its hammer loop to terminate the sim. numactl --membind=1
# is required so the 256 MB mmap lands in CXL-backed memory
# (NUMA node 1) rather than local DDR5.
command = (
    "m5 exit;"
    + "echo '=== CXL-DMSim Rowhammer test ===';"
    + "echo '[workload] rowhammer_cxl_test';"
    + "numactl --membind=1 /home/cxl_benchmark/rowhammer_cxl_test;"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(
        local_path=os.path.join(_FS_FILES, "vmlinux_20240920")
    ),
    disk_image=DiskImageResource(
        local_path=os.path.join(_FS_FILES, "parsec.img")
    ),
    readfile_contents=command,
)

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: (func() for func in [lambda: processor.switch()])
    },
)

simulator.run()
