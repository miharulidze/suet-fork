#!/usr/bin/env bash
set -eu
ROOT=/tmp/suet-nccl-retry-mkhalilo-20261014
FIX=$ROOT/endpoint-fix
source "$ROOT/env.sh"
mkdir -p "$FIX/logs"
exec > "$FIX/logs/orchestration.log" 2>&1
run_phase() {
    phase=$1; shift
    set +e
    "$@" > "$FIX/logs/$phase.log" 2>&1
    rc=$?
    set -e
    printf '%s=%s\n' "$phase" "$rc" | tee -a "$FIX/logs/status.txt"
    return "$rc"
}
: > "$FIX/logs/status.txt"
if [ ! -d "$FIX/libfabric/.git" ]; then
    git clone --shared "$ROOT/suet-workspace/libfabric" "$FIX/libfabric"
    git -C "$ROOT/suet-workspace/libfabric" diff --binary > "$FIX/integration.patch"
    git -C "$FIX/libfabric" apply "$FIX/integration.patch"
fi
mkdir -p "$FIX/libfabric/prov/suet"
cp -a "$FIX/src/suet/suet/." "$FIX/libfabric/prov/suet/"
cd "$FIX/libfabric"
run_phase autogen ./autogen.sh
run_phase configure ./configure --prefix="$FIX/install" --enable-suet --enable-udp CFLAGS='-O2 -g'
run_phase build make -j32
run_phase install make install
cd "$FIX/src/suet"
run_phase endpoint-build "$CC" -g -Wall tests/test_endpoint_identity.c -I"$FIX/install/include" -L"$FIX/install/lib" -Wl,-rpath,"$FIX/install/lib" -lfabric -o "$FIX/test_endpoint_identity"
export LD_LIBRARY_PATH="$FIX/install/lib:$LD_LIBRARY_PATH"
run_phase endpoint-test "$FIX/test_endpoint_identity"
run_phase probe-build "$CC" -g -Wall tests/nccl/diagnostics/endpoint-identity.c -I"$FIX/install/include" -L"$FIX/install/lib" -Wl,-rpath,"$FIX/install/lib" -lfabric -o "$FIX/endpoint_probe"
run_phase endpoint-probe "$FIX/endpoint_probe"
export LD_LIBRARY_PATH="$FIX/install/lib:$ROOT/plugin-install/lib:$ROOT/src/nccl/build/lib:$LD_LIBRARY_PATH"
export FI_PROVIDER='udp;ofi_suet' OFI_NCCL_PROTOCOL=SENDRECV NCCL_NET=Libfabric
export NCCL_P2P_DISABLE=1 NCCL_SHM_DISABLE=1 NCCL_NET_GDR_LEVEL=LOC NCCL_PROTO=Simple
export NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH
run_phase nccl-2gpu timeout 180s "$ROOT/src/nccl-tests/build/all_reduce_perf" -b 8 -e 1M -f 2 -g 2 -w 2 -n 5 -c 1 || true
run_phase nccl-4gpu timeout 180s "$ROOT/src/nccl-tests/build/all_reduce_perf" -b 8 -e 1M -f 2 -g 4 -w 2 -n 5 -c 1 || true
printf 'finished\n' | tee -a "$FIX/logs/status.txt"
