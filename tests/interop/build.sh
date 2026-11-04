#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${LIBFABRIC_SOURCE:?}"
: "${LIBFABRIC_PREFIX:?}"
: "${UET_REF_SOURCE:?}"
: "${INTEROP_BUILD:?}"
mkdir -p "$INTEROP_BUILD/provider" "$INTEROP_BUILD/objects"
cc -O2 -g -Wall -Wextra -fPIC -shared -I"$LIBFABRIC_PREFIX/include" \
  "$HERE/endpoint_order.c" -ldl -o "$INTEROP_BUILD/endpoint_order.so"

# Configure a private reference copy; apply only the approved CRC switch.
python3 - "$UET_REF_SOURCE" "$INTEROP_BUILD/reference-source" <<'PYCONFIG'
from pathlib import Path
import re, shutil, sys
src, dst = map(Path, sys.argv[1:])
shutil.copytree(src, dst, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('.git', 'obj_*', '*.so', '__pycache__'))
p = dst / 'uet_pkt_hdr.h'
s, n = re.subn(r'(?m)^(#define\s+UET_DEFAULT_MAX_PAYLOAD_LEN\s+)1024\b',
               lambda m: m[1] + '4000', p.read_text())
assert n == 1, 'Expected the pinned reference default payload definition'
p.write_text(s)
PYCONFIG
UET_REF_SOURCE="$INTEROP_BUILD/reference-source"
patch --batch --forward -p1 -d "$UET_REF_SOURCE" < "$HERE/reference-crc-switch.patch"

sources=("$UET_REF_SOURCE"/*.c "$UET_REF_SOURCE"/util/*.c "$UET_REF_SOURCE"/imp_shim/*.c "$UET_REF_SOURCE"/nic_shim/*.c "$UET_REF_SOURCE"/crypto/*.c)
objects=()
for source in "${sources[@]}"; do
    case "$source" in */uet.c|*xdp*) continue ;; esac
    relative=${source#"$UET_REF_SOURCE"/}
    object=$INTEROP_BUILD/objects/${relative//\//_}.o
    cc -O2 -g -Wall -DENABLE_VERBS=1 -Wno-address-of-packed-member \
      -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" -I"$UET_REF_SOURCE/imp_shim" \
      -I"$UET_REF_SOURCE/nic_shim" -I"$UET_REF_SOURCE/crypto" \
      -I"$UET_REF_SOURCE/libfabric_headers" -I"$UET_REF_SOURCE/libfabric_headers/include" \
      -fPIC -c "$source" -o "$object"
    objects+=("$object")
done

# Ping-pong and stock fabtests load this same module and DGRAM backend.
cc -O2 -g -Wall -Wextra -DENABLE_VERBS=1 -fPIC -shared \
  -I"$HERE" -I"$LIBFABRIC_SOURCE" -I"$LIBFABRIC_SOURCE/include" \
  -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" -I"$UET_REF_SOURCE/nic_shim" \
  -I"$UET_REF_SOURCE/imp_shim" -I"$UET_REF_SOURCE/crypto" \
  "$HERE/ref_provider.c" "$HERE/dgram_backend.c" "${objects[@]}" \
  -L"$LIBFABRIC_PREFIX/lib" -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" \
  -Wl,--wrap=uet_nic_initialize -Wl,--wrap=uet_nic_resolve_ipv4_nh \
  -Wl,-Bsymbolic -lfabric -lpthread -ldl -o "$INTEROP_BUILD/provider/libuet_ref-fi.so"
cc -O2 -g -Wall -Wextra -DENABLE_VERBS=1 \
  -I"$HERE" -I"$UET_REF_SOURCE" -I"$UET_REF_SOURCE/util" \
  -I"$UET_REF_SOURCE/imp_shim" -I"$UET_REF_SOURCE/nic_shim" \
  -I"$UET_REF_SOURCE/crypto" -I"$LIBFABRIC_PREFIX/include" \
  -I"$LIBFABRIC_SOURCE" -I"$LIBFABRIC_SOURCE/include" \
  "$HERE/pingpong.c" -L"$LIBFABRIC_PREFIX/lib" \
  -Wl,-rpath,"$LIBFABRIC_PREFIX/lib" -lfabric -o "$INTEROP_BUILD/pingpong"
sha256sum "$HERE"/*.[ch] "$HERE/reference-crc-switch.patch" \
  "$INTEROP_BUILD/provider/libuet_ref-fi.so" "$INTEROP_BUILD/endpoint_order.so" \
  "$INTEROP_BUILD/pingpong" > "$INTEROP_BUILD/build-sha256.txt"
