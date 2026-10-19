# Test NCCL over SUET/UDP

This procedure builds NCCL, the AWS OFI NCCL plugin and `nccl-tests`, then
checks all-reduce on 2 and 4 GPUs on one Linux node. It uses SUET's tagged
SENDRECV path with CPU staging and ROD. GPU P2P, shared memory and GPU Direct
RDMA are disabled so that inter-GPU traffic exercises `udp;ofi_suet`.

The [2026-10-14 validation](endpoint-fix-2026-10-14.md) passed on four
V100 PCIe GPUs, from 8 bytes through 1 MiB, with zero validation errors.
It used the RI/AV endpoint-identity fix, GCC 13.4.0, CUDA 12.8.93 and hwloc
2.15.0. The NCCL and plugin sources were unmodified. This does not establish
GPU-direct, multinode, RDMA-protocol or NCCL/RUD support.

The RI/AV fix and its regressions are in commit `69100a2`; commit `91c7893`
pins the matching libfabric dependency. Build from a revision containing both.

## Prerequisites and paths

Run inside an allocation with at least four visible GPUs, a compatible NVIDIA
driver/toolkit, a C++20 compiler, hwloc development files, Git, Autoconf,
Automake, Libtool, pkg-config, rsync, Python 3, Make and GNU `timeout`.
Start at the SUET repository root and edit the CUDA/hwloc paths below:

```bash
set -euo pipefail
export SUET_REPO="$PWD"
export WORKDIR="${WORKDIR:-$SUET_REPO/suet-workspace}"
export NCCL_WORKDIR="${NCCL_WORKDIR:-/tmp/suet-nccl-validation}"
export LIBFABRIC_PREFIX="$WORKDIR/build"
export PLUGIN_PREFIX="$NCCL_WORKDIR/plugin-install"
export CUDA_HOME=/path/to/cuda
export HWLOC_PREFIX=/path/to/hwloc
export CC=gcc CXX=g++ JOBS=8
# V100; change this for another GPU architecture.
export NVCC_GENCODE='-gencode=arch=compute_70,code=sm_70'
export CUDA_INC="${CUDA_INC:-$CUDA_HOME/include}"
export CUDA_LIB="${CUDA_LIB:-$CUDA_HOME/lib64}"
export PATH="$CUDA_HOME/bin:$PATH"
export PKG_CONFIG_PATH="$HWLOC_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$CUDA_LIB:$HWLOC_PREFIX/lib:${LD_LIBRARY_PATH:-}"
mkdir -p "$NCCL_WORKDIR/src" "$NCCL_WORKDIR/logs"
"$CC" --version
"$CXX" --version
"$CUDA_HOME/bin/nvcc" --version
pkg-config --modversion hwloc
nvidia-smi
```

Use absolute workspace paths. For a split Conda CUDA installation, set
`CUDA_INC` and `CUDA_LIB` to its target include/library directories before
running this block, and make those directories available to the plugin's
compiler/linker as well. The [Ault retry report](retry-2026-10-14.md) records
the tested Conda setup. Check these paths on the compute node: in the Ault
experiment `/scratch`, `/apps` and `/users` were unavailable there, so the
toolchain and builds were staged into node-local `/tmp`.

## Build SUET and the pinned dependencies

```bash
cd "$SUET_REPO"
export LIBFABRIC_REF=94f3b1ac6c3e08ca47804bd327f7c328b60f67d2
./build.sh > "$NCCL_WORKDIR/logs/suet-build.log" 2>&1
export LD_LIBRARY_PATH="$LIBFABRIC_PREFIX/lib:$LD_LIBRARY_PATH"
"$LIBFABRIC_PREFIX/bin/fi_info" -p 'udp;ofi_suet' -t FI_EP_RDM \
  > "$NCCL_WORKDIR/logs/fi-info.log"
./build.sh --test > "$NCCL_WORKDIR/logs/fabtests.log" 2>&1
"$CC" -g -Wall tests/test_endpoint_identity.c \
  -I"$LIBFABRIC_PREFIX/include" -L"$LIBFABRIC_PREFIX/lib" \
  -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" -lfabric \
  -o "$NCCL_WORKDIR/test_endpoint_identity"
"$NCCL_WORKDIR/test_endpoint_identity" \
  > "$NCCL_WORKDIR/logs/endpoint-identity.log" 2>&1
```

The default build pins libfabric to `94f3b1ac6c3e08ca47804bd327f7c328b60f67d2`
and applies the existing source-error, provider-registration and entropy
extensions. See [INSTALL.md](../../INSTALL.md). The endpoint test must report
distinct AV entries and successful directed/unexpected transfers under both
ROD and RUD before running NCCL.

## Build NCCL, its OFI plugin and the benchmarks

Use fresh component directories so an old configure cache or plugin RPATH
cannot select an earlier SUET build. These are the revisions tested on Ault:

```bash
cd "$NCCL_WORKDIR/src"
git clone https://github.com/NVIDIA/nccl.git
git -C nccl checkout --detach 3e46c089f4ae4732722a419eb6096aead7e47a30
git clone https://github.com/miharulidze/aws-ofi-nccl.git
git -C aws-ofi-nccl checkout --detach 745b951f443511bbc79beb0868cd8a967907eb42
git clone https://github.com/NVIDIA/nccl-tests.git
git -C nccl-tests checkout --detach afd59abab774a6a4b492eb344bd0c845a0b7bb4d

cd "$NCCL_WORKDIR/src/nccl"
make -j"$JOBS" src.build CUDA_HOME="$CUDA_HOME" CUDA_INC="$CUDA_INC" \
  CUDA_LIB="$CUDA_LIB" NVCC_GENCODE="$NVCC_GENCODE" \
  > "$NCCL_WORKDIR/logs/nccl-build.log" 2>&1

cd "$NCCL_WORKDIR/src/aws-ofi-nccl"
./autogen.sh > "$NCCL_WORKDIR/logs/plugin-autogen.log" 2>&1
./configure --prefix="$PLUGIN_PREFIX" --with-libfabric="$LIBFABRIC_PREFIX" \
  --with-cuda="$CUDA_HOME" --with-nvcc="$CUDA_HOME/bin/nvcc" \
  --with-hwloc="$HWLOC_PREFIX" --disable-tests \
  > "$NCCL_WORKDIR/logs/plugin-configure.log" 2>&1
make -j"$JOBS" NVCC_GENCODE="$NVCC_GENCODE" \
  > "$NCCL_WORKDIR/logs/plugin-build.log" 2>&1
make install > "$NCCL_WORKDIR/logs/plugin-install.log" 2>&1

cd "$NCCL_WORKDIR/src/nccl-tests"
make -j"$JOBS" MPI=0 CUDA_HOME="$CUDA_HOME" CUDA_INC="$CUDA_INC" \
  CUDA_LIB="$CUDA_LIB" NCCL_HOME="$NCCL_WORKDIR/src/nccl/build" \
  NVCC_GENCODE="$NVCC_GENCODE" \
  > "$NCCL_WORKDIR/logs/nccl-tests-build.log" 2>&1
```

`MPI=0` uses one process controlling multiple GPUs; an MPI installation is
not needed for this test. Rebuild the plugin with `--with-libfabric` pointing
to the current SUET installation whenever that installation moves. In the
initial rerun, an embedded `DT_RPATH` loaded the old provider despite a new
`LD_LIBRARY_PATH`; that run still failed until the plugin was relinked.

## Run the checked 2-/4-GPU matrix

```bash
export LD_LIBRARY_PATH="$PLUGIN_PREFIX/lib:$LIBFABRIC_PREFIX/lib:$NCCL_WORKDIR/src/nccl/build/lib:$LD_LIBRARY_PATH"
export FI_PROVIDER='udp;ofi_suet'
export OFI_NCCL_PROTOCOL=SENDRECV NCCL_NET=Libfabric
export NCCL_P2P_DISABLE=1 NCCL_SHM_DISABLE=1
export NCCL_NET_GDR_LEVEL=LOC NCCL_PROTO=Simple
export NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH
for gpus in 2 4; do
  LD_DEBUG=libs timeout 180s \
    "$NCCL_WORKDIR/src/nccl-tests/build/all_reduce_perf" \
    -b 8 -e 1M -f 2 -g "$gpus" -w 2 -n 5 -c 1 \
    > "$NCCL_WORKDIR/logs/all-reduce-${gpus}gpu.log" 2>&1
done
```

The shell stops on a nonzero exit, including timeout. `-c 1` enables payload
checking. Preserve the complete logs, including `LD_DEBUG=libs` output.
Accept the run only when **both** GPU counts satisfy all of these conditions:

- The command exits zero, covers 8 bytes through 1 MiB, and ends with
  `Out of bounds values : 0 OK`.
- NCCL reports `Using network Libfabric` and channel traffic via
  `NET/Libfabric`, rather than another network transport.
- The OFI plugin reports the selected `udp;ofi_suet` provider and SENDRECV.
  The loader trace identifies the intended `libfabric.so.1`, `libnccl-net.so`
  and NCCL build; discovery with `fi_info` alone does not prove their use.
- GPU Direct RDMA is disabled and the P2P/SHM disables remain in effect.

The unmodified plugin requests `FI_ORDER_SAS`, so these commands select ROD
even though SUET also supports RUD. Do not remove the ordering requirement
merely to force the test onto RUD.

## Recorded evidence

The [endpoint-fix report](endpoint-fix-2026-10-14.md) contains the passing
2-/4-GPU logs, source checksums, loader evidence and archive locations.
The [experiment index](README.md) and [retry report](retry-2026-10-14.md)
retain the earlier build and runtime failures for comparison. The MPI and
reference-provider experiments are separate tests; their results do not
extend this NCCL test's coverage.
