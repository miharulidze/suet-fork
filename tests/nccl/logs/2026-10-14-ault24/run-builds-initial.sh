#!/usr/bin/env bash
set -uo pipefail
ROOT=/tmp/suet-nccl-mkhalilo-20261014
cd "$ROOT" || exit 1
mkdir -p logs
exec >logs/orchestration.log 2>&1
set -x
date -Is
hostname
nvidia-smi
gcc --version
g++ --version
autoconf --version
automake --version
libtoolize --version
command -v nvcc || true
for repo in suet nccl aws-ofi-nccl nccl-tests; do
    git -C "$repo" rev-parse HEAD
    git -C "$repo" status --short
done
CUDA_ROOT=/apps/ault/spack/opt/spack/linux-centos8-zen2/gcc-10.2.0/cuda-11.4.0-udyaakpt7oztg7gnj764dhkhdf5ory5d
HWLOC_ROOT=/apps/ault/spack/opt/spack/linux-centos8-zen2/gcc-10.2.0/hwloc-2.5.0-gwltaenwfu2fe6elj2k2img2ospfhwvm
(
    set -ex
    cd "$ROOT/nccl"
    make -j4 src.build CUDA_HOME="$CUDA_ROOT" NVCC_GENCODE='-gencode=arch=compute_70,code=sm_70'+) >logs/nccl-build.log 2>&1
printf 'nccl_build=%s\n' "$?" >>logs/status.txt
(
    set -ex
    cd "$ROOT/suet"
    export GIT_COMMITTER_NAME='SUET baseline build'
    export GIT_COMMITTER_EMAIL='mkhalilo@ault24'
    export JOBS=8 WORKDIR="$ROOT/suet-workspace"
    export LIBFABRIC_REF=0d52314b7e1d9329f9843e46040fa4f8a74ed121
    bash ./build.sh
) >logs/suet-build.log 2>&1
printf 'suet_build=%s\n' "$?" >>logs/status.txt
(
    set -ex
    cd "$ROOT/aws-ofi-nccl"
    ./autogen.sh
    ./configure --prefix="$ROOT/plugin-install" --with-libfabric="$ROOT/suet-workspace/build" --with-cuda="$CUDA_ROOT" --with-hwloc="$HWLOC_ROOT" --disable-tests
    make -j4
    make install
) >logs/plugin-build.log 2>&1
printf 'plugin_build=%s\n' "$?" >>logs/status.txt
if test -x "$ROOT/suet-workspace/build/bin/fi_info"; then
    "$ROOT/suet-workspace/build/bin/fi_info" -p 'udp;ofi_suet' >logs/fi-info-suet.log 2>&1
    printf 'fi_info_suet=%s\n' "$?" >>logs/status.txt
fi
if test -f "$ROOT/nccl/build/lib/libnccl.so" && test -f "$ROOT/plugin-install/lib/libnccl-net.so"; then
    (
        set -ex
        cd "$ROOT/nccl-tests"
        make -j4 CUDA_HOME="$CUDA_ROOT" NCCL_HOME="$ROOT/nccl/build"
    ) >logs/nccl-tests-build.log 2>&1
    printf 'nccl_tests_build=%s\n' "$?" >>logs/status.txt
else
    printf '%s\n' 'gpu_tests=BLOCKED: NCCL and/or OFI plugin did not build; no fallback transport tested.' >>logs/status.txt
fi
date -Is
touch logs/builds-finished
