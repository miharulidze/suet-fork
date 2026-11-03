#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${UET_REF_SOURCE:?}"
: "${LIBFABRIC_SOURCE:?}"
: "${LIBFABRIC_PREFIX:?}"
: "${UD_BUILD:?}"
mkdir -p "$UD_BUILD/provider" "$UD_BUILD/objects"
sources=("$UET_REF_SOURCE"/*.c "$UET_REF_SOURCE"/util/*.c "$UET_REF_SOURCE"/imp_shim/*.c "$UET_REF_SOURCE"/nic_shim/*.c "$UET_REF_SOURCE"/crypto/*.c)
objects=()
for source in "${sources[@]}"; do
    case "$source" in */uet.c|*xdp*) continue ;; esac
    relative=${source#"$UET_REF_SOURCE"/}
    object=$UD_BUILD/objects/${relative//\//_}.o
    if [[ "${UD_REUSE_CORE:-0}" != 1 || ! -f "$object" ]]; then
    cc -O2 -g -Wall -DENABLE_VERBS=1 -Wno-address-of-packed-member \
      -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" -I"$UET_REF_SOURCE/imp_shim" \
      -I"$UET_REF_SOURCE/nic_shim" -I"$UET_REF_SOURCE/crypto" \
      -I"$UET_REF_SOURCE/libfabric_headers" -I"$UET_REF_SOURCE/libfabric_headers/include" \
      -fPIC -c "$source" -o "$object"
    fi
    objects+=("$object")
done
cc -O2 -g -Wall -Wextra -DENABLE_VERBS=1 -fPIC -shared \
  -I"$LIBFABRIC_SOURCE" -I"$LIBFABRIC_SOURCE/include" \
  -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" -I"$UET_REF_SOURCE/nic_shim" \
  -I"$UET_REF_SOURCE/imp_shim" -I"$UET_REF_SOURCE/crypto" \
  "$HERE/ref_provider.c" "$HERE/ud_backend.c" "${objects[@]}" \
  -L"$LIBFABRIC_PREFIX/lib" -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" \
  -Wl,--wrap=uet_nic_initialize -Wl,--wrap=uet_nic_resolve_ipv4_nh \
  -Wl,-Bsymbolic -lfabric -lpthread -ldl -o "$UD_BUILD/provider/libuet_ref-fi.so"
sha256sum "$HERE"/*.[ch] "$UD_BUILD/provider/libuet_ref-fi.so" > "$UD_BUILD/build-sha256.txt"
