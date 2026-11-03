#!/bin/bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${UET_REF_SOURCE:?}"
: "${LIBFABRIC_SOURCE:?}"
: "${LIBFABRIC_PREFIX:?}"
: "${UD_BUILD:?}"
mkdir -p "$UD_BUILD/provider" "$UD_BUILD/objects"

: "${UD_REFERENCE_PAYLOAD:=4000}"
if [[ "${UD_REUSE_CORE:-0}" == 1 && -f "$UD_BUILD/reference-payload.txt" &&
      "$(cat "$UD_BUILD/reference-payload.txt")" != "$UD_REFERENCE_PAYLOAD" ]]; then
    echo "Reference profile changed: rebuild without UD_REUSE_CORE" >&2
    exit 2
fi
# Configure the reference's build-time payload limit in an isolated copy.
# Protocol implementation files remain identical to the pinned source.
python3 - "$UET_REF_SOURCE" "$UD_BUILD/reference-source" "$UD_REFERENCE_PAYLOAD" <<'PYCONFIG'
from pathlib import Path
import shutil,re,sys
src,dst,payload=Path(sys.argv[1]),Path(sys.argv[2]),int(sys.argv[3])
assert payload in (1024,4000),payload
shutil.copytree(src,dst,dirs_exist_ok=True,ignore=shutil.ignore_patterns('.git','obj_*','*.so','__pycache__'))
p=dst/'uet_pkt_hdr.h';s=p.read_text()
s,n=re.subn(r'(?m)^(#define\s+UET_DEFAULT_MAX_PAYLOAD_LEN\s+)1024\b',lambda m:m[1]+str(payload),s)
assert n==1,'Expected the pinned reference default payload definition'
p.write_text(s)
PYCONFIG
UET_REF_SOURCE="$UD_BUILD/reference-source"
printf '%s\n' "$UD_REFERENCE_PAYLOAD" > "$UD_BUILD/reference-payload.txt"

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
