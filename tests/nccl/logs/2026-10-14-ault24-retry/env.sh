#!/usr/bin/env bash
export ROOT=/tmp/suet-nccl-retry-mkhalilo-20261014
export TC="$ROOT/toolchain"
export PATH="$TC/bin:/usr/bin:/bin"
export CC="$TC/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$TC/bin/x86_64-conda-linux-gnu-g++"
export CUDA_HOME="$ROOT/cuda"
export CUDA_INC="$TC/targets/x86_64-linux/include"
export CUDA_LIB="$TC/targets/x86_64-linux/lib"
export LD_LIBRARY_PATH="$TC/lib:$CUDA_LIB:/usr/lib64"
export PKG_CONFIG_PATH="$TC/lib/pkgconfig"
export CPPFLAGS="-isystem $CUDA_INC -isystem $TC/include"
export LDFLAGS="-L$CUDA_LIB -L$TC/lib -Wl,-rpath,$TC/lib -Wl,-rpath,$CUDA_LIB"
export NVCC_GENCODE='-gencode=arch=compute_70,code=sm_70'
export GIT_COMMITTER_NAME='SUET baseline build'
export GIT_COMMITTER_EMAIL='mkhalilo@ault24'
