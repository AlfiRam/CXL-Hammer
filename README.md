# CXL-Hammer

CXL-Hammer is a gem5-based proof-of-concept work of Rowhammer attacks on CXL Type 3 disaggregated memory. It ports a behavioral Rowhammer disturbance model onto the CXL memory path modeled by CXL-DMSim, and adds an optional SECDED ECC recovery layer at the CXL DRAM interface.

## Based on

- **Base simulator**: [CXL-DMSim](https://github.com/ferry-hhh/CXL-DMSim), forked at commit `6d2a3f76`.
- **Rowhammer model**: [HammerSim](https://arch.cs.ucdavis.edu/memory/simulation/security/2023/03/20/yarch-hammersim.html), source at [darchr/gem5-rowhammer](https://github.com/darchr/gem5-rowhammer).
- **JSON library**: [nlohmann/json](https://github.com/nlohmann/json) (MIT licensed, vendored at `ext/json/json/single_include/`).

## Requirements

Same as gem5 v23.10. Recommended on Ubuntu 20.04 or 22.04:

```bash
sudo apt install build-essential git m4 scons zlib1g zlib1g-dev \
    libprotobuf-dev protobuf-compiler libprotoc-dev libgoogle-perftools-dev \
    python3-dev python-is-python3 libboost-all-dev pkg-config
```

KVM must be accessible on the host (`/dev/kvm`).

## Clone

```bash
git clone https://github.com/AlfiRam/CXL-Hammer.git
cd CXL-Hammer
```

## Build

```bash
# build the simulator
scons build/X86/gem5.opt -j$(nproc)

# build the hammer workload
cd benchmarks
make
cd ..
```

## Kernel and disk image

This simulation requires a gem5-compatible x86 Linux kernel and a disk image with a Linux root filesystem. These are not included in this repository due to size.

Download them from the upstream CXL-DMSim project's Google Drive:

https://drive.google.com/drive/folders/1sxZBsedT19ntJdzN8MTkcbczMGXNXgrM

Place the files at:

```
fs_files/vmlinux_20240920
fs_files/parsec.img
```

The `fs_files/` directory is gitignored and must be created by the user. If your downloaded kernel has a different filename, either rename it to `vmlinux_20240920` or edit `configs/example/gem5_library/x86-cxl-hammer.py` to match.

## Install the hammer binary into the disk image

The workload binary must live inside the guest filesystem at `/home/cxl_benchmark/rowhammer_cxl_test`. The disk image must also have `numactl` installed; install it via `apt` in the mounted image if it isn't already present.

```bash
sudo mkdir -p /tmp/parsec_mount
sudo mount -o loop,offset=$((2048*512)) fs_files/parsec.img /tmp/parsec_mount

cd benchmarks
make install
cd ..

sudo umount /tmp/parsec_mount
```

## Run the simulation

```bash
./build/X86/gem5.opt -d m5out/cxl_hammer \
    configs/example/gem5_library/x86-cxl-hammer.py
```

The config ships with `enable_ecc=True`. To run the ECC-off condition, edit `configs/example/gem5_library/x86-cxl-hammer.py` and change `enable_ecc=True` to `enable_ecc=False`.

## Results

After the simulation finishes, gem5 writes all output to `m5out/cxl_hammer/`, especially the stats.txt file:

- **`m5out/cxl_hammer/stats.txt`** — gem5 statistics dump. Rowhammer and ECC counters are reported per CXL DRAM channel under `board.cxl_memory.mem_ctrl.dram.*`. Key counters to grep for:

```bash
  grep "board.cxl_memory.mem_ctrl.dram.rowHammer" m5out/cxl_hammer/stats.txt
```
