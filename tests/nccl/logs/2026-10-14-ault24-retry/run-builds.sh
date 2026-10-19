#!/usr/bin/env bash
set -uo pipefail
ROOT=/tmp/suet-nccl-retry-mkhalilo-20261014
source "$ROOT/env.sh"
mkdir -p "$ROOT/logs"
exec >"$ROOT/logs/orchestration.log" 2>&1
set -x
date -Is
hostname
"$CC" --version
"$CXX" --version
nvcc --version
nvidia-smi
pkg-config --modversion hwloc
for repo in suet nccl aws-ofi-nccl nccl-tests; do
    git -C "$ROOT/src/$repo" rev-parse HEAD
    git -C "$ROOT/src/$repo" status --short
done
build_nccl() (
    set -ex
    cd "$ROOT/src/nccl"
    make -j12 src.build CUDA_HOME="$CUDA_HOME" CUDA_INC="$CUDA_INC" CUDA_LIB="$CUDA_LIB" NVCC_GENCODE="$NVCC_GENCODE"
)
build_suet() (
    set -ex
    cd "$ROOT/src/suet"
    JOBS=12 WORKDIR="$ROOT/suet-workspace" LIBFABRIC_REF=94f3b1ac6c3e08ca47804bd327f7c328b60f67d2 bash ./build.sh
)
build_nccl >"$ROOT/logs/nccl-build.log" 2>&1 &
nccl_pid=$!
build_suet >"$ROOT/logs/suet-build.log" 2>&1 &
suet_pid=$!
wait "$nccl_pid"
nccl_rc=$?
printf 'nccl_build=%s\n' "$nccl_rc" >>"$ROOT/logs/status.txt"
wait "$suet_pid"
suet_rc=$?
printf 'suet_build=%s\n' "$suet_rc" >>"$ROOT/logs/status.txt"
plugin_rc=99
tests_rc=99
if test "$suet_rc" -eq 0; then
    export LD_LIBRARY_PATH="$ROOT/suet-workspace/build/lib:$LD_LIBRARY_PATH"
    (
        set -ex
        cd "$ROOT/src/aws-ofi-nccl"
        ./autogen.sh
        ./configure --prefix="$ROOT/plugin-install" --with-libfabric="$ROOT/suet-workspace/build" --with-cuda="$CUDA_HOME" --with-nvcc="$TC/bin/nvcc" --with-hwloc="$TC" --disable-tests
        make -j12 NVCC_GENCODE="$NVCC_GENCODE"
        make install
    ) >"$ROOT/logs/plugin-build.log" 2>&1
    plugin_rc=$?
    printf 'plugin_build=%s\n' "$plugin_rc" >>"$ROOT/logs/status.txt"
    cp "$ROOT/src/aws-ofi-nccl/config.log" "$ROOT/logs/plugin-config.log"
    "$ROOT/suet-workspace/build/bin/fi_info" -p 'udp;ofi_suet' -t FI_EP_RDM >"$ROOT/logs/fi-info-suet.log" 2>&1
    printf 'fi_info_suet=%s\n' "$?" >>"$ROOT/logs/status.txt"
else
    printf 'plugin_build=BLOCKED: SUET/libfabric build failed\n' >>"$ROOT/logs/status.txt"
fi
if test "$nccl_rc" -eq 0; then
    (
        set -ex
        cd "$ROOT/src/nccl-tests"
        make -j8 CUDA_HOME="$CUDA_HOME" CUDA_INC="$CUDA_INC" CUDA_LIB="$CUDA_LIB" NCCL_HOME="$ROOT/src/nccl/build" NVCC_GENCODE="$NVCC_GENCODE"
    ) >"$ROOT/logs/nccl-tests-build.log" 2>&1
    tests_rc=$?
    printf 'nccl_tests_build=%s\n' "$tests_rc" >>"$ROOT/logs/status.txt"
else
    printf 'nccl_tests_build=BLOCKED: NCCL build failed\n' >>"$ROOT/logs/status.txt"
fi
if test "$plugin_rc" -eq 0 && test "$tests_rc" -eq 0; then
    export LD_LIBRARY_PATH="$ROOT/plugin-install/lib:$ROOT/src/nccl/build/lib:$ROOT/suet-workspace/build/lib:$LD_LIBRARY_PATH"
    export FI_PROVIDER='udp;ofi_suet'
    export OFI_NCCL_PROTOCOL=SENDRECV
    export NCCL_NET=Libfabric
    export NCCL_P2P_DISABLE=1 NCCL_SHM_DISABLE=1
    export NCCL_NET_GDR_LEVEL=LOC NCCL_PROTO=Simple
    export NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH
    for gpus in 2 4; do
        (
            set -x
            timeout 180s "$ROOT/src/nccl-tests/build/all_reduce_perf" -b 8 -e 1M -f 2 -g "$gpus" -w 2 -n 5 -c 1
        ) >"$ROOT/logs/all-reduce-${gpus}gpu.log" 2>&1
        printf 'all_reduce_%sgpu=%s\n' "$gpus" "$?" >>"$ROOT/logs/status.txt"
    done
else
    printf 'gpu_tests=BLOCKED: one or more builds failed\n' >>"$ROOT/logs/status.txt"
fi
for repo in suet nccl aws-ofi-nccl nccl-tests; do
    git -C "$ROOT/src/$repo" diff --stat
    git -C "$ROOT/src/$repo" diff --cached --stat
done
date -Is
touch "$ROOT/logs/builds-finished"
