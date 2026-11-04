#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${LIBFABRIC_SOURCE:?}"
: "${LIBFABRIC_PREFIX:?}"
: "${UET_REF_SOURCE:?}"
: "${INTEROP_BUILD:?}"
# Build the exact same loadable reference provider and DGRAM NIC used by fabtests.
UD_BUILD="$INTEROP_BUILD" bash "$HERE/udverbs/build.sh"
cc -O2 -g -Wall -Wextra -DENABLE_VERBS=1 \
  -I"$HERE" -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" \
  -I"$UET_REF_SOURCE/imp_shim" -I"$UET_REF_SOURCE/nic_shim" \
  -I"$UET_REF_SOURCE/crypto" -I"$LIBFABRIC_PREFIX/include" \
  -I"$LIBFABRIC_SOURCE" -I"$LIBFABRIC_SOURCE/include" \
  "$HERE/pingpong.c" -L"$LIBFABRIC_PREFIX/lib" \
  -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" -lfabric -o "$INTEROP_BUILD/pingpong"
sha256sum "$HERE/pingpong.c" "$INTEROP_BUILD/pingpong" >> "$INTEROP_BUILD/build-sha256.txt"
