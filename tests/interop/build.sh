#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${UET_REF_SOURCE:?Set UET_REF_SOURCE to the pinned uet-ref-prov checkout}"
: "${LIBFABRIC_SOURCE:?Set LIBFABRIC_SOURCE to the configured SUET/libfabric source}"
: "${LIBFABRIC_PREFIX:?Set LIBFABRIC_PREFIX to the corrected SUET/libfabric install}"
: "${INTEROP_BUILD:?Set INTEROP_BUILD to a separate build directory}"
: "${CC:=cc}"
: "${REFERENCE_VERBS_API:=1}"
[[ "$REFERENCE_VERBS_API" = 1 ]] || { echo "This test uses the explicit endpoint API (REFERENCE_VERBS_API=1)" >&2; exit 2; }
mkdir -p "$INTEROP_BUILD"
sources=("$UET_REF_SOURCE"/*.c "$UET_REF_SOURCE"/util/*.c
         "$UET_REF_SOURCE"/imp_shim/*.c "$UET_REF_SOURCE"/nic_shim/*.c
         "$UET_REF_SOURCE"/crypto/*.c)
objects=()
for source in "${sources[@]}"; do
    case "$source" in */uet.c|*xdp*) continue ;; esac
    relative=${source#"$UET_REF_SOURCE"/}
    object=$INTEROP_BUILD/${relative//\//_}.o
    "$CC" -O2 -g -Wall -DENABLE_VERBS="$REFERENCE_VERBS_API" -Wno-address-of-packed-member \
        -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" \
        -I"$UET_REF_SOURCE/imp_shim" -I"$UET_REF_SOURCE/nic_shim" \
        -I"$UET_REF_SOURCE/crypto" -I"$UET_REF_SOURCE/libfabric_headers" \
        -I"$UET_REF_SOURCE/libfabric_headers/include" \
        -fPIC -c "$source" -o "$object"
    objects+=("$object")
done
"$CC" -O2 -g -Wall -Wextra -DENABLE_VERBS="$REFERENCE_VERBS_API" \
    -I"$HERE" -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" \
    -I"$UET_REF_SOURCE/imp_shim" -I"$UET_REF_SOURCE/nic_shim" \
    -I"$UET_REF_SOURCE/crypto" \
    -I"$UET_REF_SOURCE/libfabric_headers" \
    -I"$UET_REF_SOURCE/libfabric_headers/include" \
    -I"$LIBFABRIC_SOURCE" -I"$LIBFABRIC_SOURCE/include" \
    -I"$LIBFABRIC_PREFIX/include" \
    "$HERE/pingpong.c" "$HERE/udp_backend.c" "${objects[@]}" \
    -L"$LIBFABRIC_PREFIX/lib" -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" \
    -Wl,--wrap=uet_nic_initialize -Wl,--wrap=uet_nic_resolve_ipv4_nh \
    -lfabric -lpthread -ldl -o "$INTEROP_BUILD/pingpong"
if [ -d "$UET_REF_SOURCE/.git" ]; then
    git -C "$UET_REF_SOURCE" rev-parse HEAD > "$INTEROP_BUILD/reference-commit.txt"
else
    : "${UET_REF_REVISION:?Set UET_REF_REVISION for an exported source tree}"
    printf '%s\n' "$UET_REF_REVISION" > "$INTEROP_BUILD/reference-commit.txt"
fi
sha256sum "$HERE"/*.c "$HERE"/*.h > "$INTEROP_BUILD/testbench-sha256.txt"
