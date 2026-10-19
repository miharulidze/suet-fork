# SUET endpoint-addressing fix: implementation and Ault validation

The authorized fix passes NCCL all-reduce with both 2 and 4 V100 GPUs on
`ault24`, from 8 bytes through 1 MiB, with zero validation errors. The
tests use `udp;ofi_suet`, the OFI SENDRECV protocol and ROD, with P2P, SHM
and GDR disabled. The plugin source and NCCL source remain unmodified.

The provider fix and regression tests were subsequently committed as
`69100a2`; the dependency pin is in separate commit `91c7893`. The provenance
below records the original experiment, when the fix was still uncommitted.
For new runs, use the [NCCL test guide](TESTING.md).

## Implementation

SUET previously reduced a remote endpoint address to its physical datagram
peer. Distinct RIs on one UDP socket therefore received the same AV handle,
and transmitted SES headers used the sender's local PID/RI.

The fix changes six provider files:

- `suet.h` and `suet_av.c`: each public AV insertion owns a complete remote
  address and a distinct table handle. Entries still share the stable
  domain-wide datagram peer. Resolution copies the address under the AV lock;
  removal and close release the logical entry. `fi_av_lookup` returns its
  full address, and invalid handles cannot silently become receive wildcards.
- `suet_ep.c`: each endpoint publishes its initiator ID in `fi_getname`.
  SUET assigns `(PIDonFEP << 12) | RI`, unique within its datagram FEP;
  directed matching also checks the physical peer.
- `suet_ses.h` and `suet_ses.c`: each transmission snapshots its destination
  PID/RI. SES headers use that destination and the local source initiator.
  Posted/unexpected message matching and fragment reassembly distinguish
  source initiators, including valid initiator ID zero.
- `suet_msg.c`: directed receives and peek resolve the selected logical
  source address rather than matching only its UDP peer.

These roles follow `uet-ref-prov` commit `ae19b9776c42cd78f57118b9b7993ae913625e46`,
whose [request builder](https://github.com/ultraethernet/uet-ref-prov/blob/ae19b9776c42cd78f57118b9b7993ae913625e46/uet_api.c#L5068-L5073)
takes PID/RI from the destination AV and initiator from the local endpoint.
[UE specification Table 2-39](https://ultraethernet.org/wp-content/uploads/sites/20/2026/01/UE-Specification-1.0.2-1.pdf#page=117)
defines the same mapping.

Address and packet layouts are unchanged. SUET's existing numeric node/service
bootstrap addresses have `num_ri=0` and unspecified PID/RI/initiator, before
the in-band name exchange. They retain the legacy PID 0 / RI 0 bootstrap route;
ordinary named destinations are checked against the receiving endpoint's PID.
Use exchanged `fi_getname` addresses for directed and multiple-endpoint traffic.
This preserves existing fabtests; it is not a complete UET control-plane or
JobID/PID isolation implementation.

## Verification

| Check | Result | Evidence |
| --- | --- | --- |
| Original endpoint probe, macOS | Distinct names alias to AV handle 0; expected exit 1 | [Baseline probe](logs/2026-10-14-endpoint-fix/mac-endpoint-probe-baseline.log) |
| Fixed endpoint probe, macOS and Ault | Distinct names return handles 0 and 1; exit 0 | [macOS](logs/2026-10-14-endpoint-fix/mac-endpoint-probe-fixed.log), [Ault](logs/2026-10-14-endpoint-fix/endpoint-probe.log) |
| Four endpoints sharing one UDP socket | RIs 0/1 reach RIs 2/3; directed, unexpected, wildcard and empty transfers pass under ROD and RUD | [macOS](logs/2026-10-14-endpoint-fix/mac-endpoint-identity.log), [Ault](logs/2026-10-14-endpoint-fix/endpoint-test.log) |
| Focused AV, SES/PDS, PDS/datagram and clock regressions | Pass; AV and directly compiled SES also pass ASan/UBSan | [Focused log](logs/2026-10-14-endpoint-fix/mac-focused.log) |
| Existing UDP fabtests | 35 passed, 0 failed; original iteration counts retained | [Fabtests](logs/2026-10-14-endpoint-fix/mac-fabtests.log) |
| htsim CTests | All 5 passed against the corrected library | [htsim log](logs/2026-10-14-endpoint-fix/mac-htsim.log) |
| NCCL all-reduce, 2 GPUs | Exit 0; all sizes validate, `Out of bounds values : 0 OK` | [2-GPU log](logs/2026-10-14-endpoint-fix/nccl-fixed-2gpu.log) |
| NCCL all-reduce, 4 GPUs | Exit 0; all sizes validate, `Out of bounds values : 0 OK` | [4-GPU log](logs/2026-10-14-endpoint-fix/nccl-fixed-4gpu.log) |

The public API regression is [test_endpoint_identity.c](../test_endpoint_identity.c).
The AV and SES tests also cover address ownership/removal and interleaved
SOM-last messages with the same message ID but different source initiators.
[Test instructions](../README.md#logical-endpoint-identity) describe compilation.
Local builds used the existing macOS dependency integration in a separate
`/tmp/suet-endpoint-fix-20261014` tree. SUET compiled without warnings.
The standalone SES sanitizer object uses `-DNDEBUG` to match the release
libfabric pool layout; test assertions remain enabled.

## Ault builds and loader verification

Allocation `892469` reserved `ault24` exclusively with four GPUs and 128 GiB
for three hours. The build step requested 32 CPUs explicitly. It reused the
scratch-installed GCC 13.4.0 / CUDA 12.8.93 toolchain and the approved
libfabric base `94f3b1ac6c3e08ca47804bd327f7c328b60f67d2`, with the original
two datagram source-error cherry-picks and SUET integration/EV patches.
The dependency HEAD was `0c270f7482fa8a5cbc1e8f52551d389c645c7483`.

NCCL remains commit `3e46c089f4ae4732722a419eb6096aead7e47a30` (2.32.3),
the OFI plugin remains `745b951f443511bbc79beb0868cd8a967907eb42`, and
nccl-tests remains `afd59abab774a6a4b492eb344bd0c845a0b7bb4d` (2.21.1).
SUET is the uncommitted fix on `refactor` HEAD `05120e5`.
The [source checksums](logs/2026-10-14-endpoint-fix/source-sha256.txt)
identify the provider files and integration regression actually tested.

The first GPU rerun still failed because the original plugin had ELF
`DT_RPATH` pointing at the baseline libfabric. `LD_LIBRARY_PATH` did not
override it. The [original dynamic section](logs/2026-10-14-endpoint-fix/plugin-original-dynamic.log)
and [loader trace](logs/2026-10-14-endpoint-fix/loaded-provider-2gpu.log)
prove that this rerun loaded the old provider; it does not assess the fix.
Those logs are retained as `nccl-2gpu.log` / `nccl-4gpu.log`.

The unchanged plugin was then built in a fresh checkout with
`--with-libfabric` pointing at the corrected install. Both authoritative
`nccl-fixed-*` logs include loader traces showing:

```text
calling init: /tmp/suet-nccl-retry-mkhalilo-20261014/endpoint-fix/install/lib/libfabric.so.1
calling init: /tmp/suet-nccl-retry-mkhalilo-20261014/endpoint-fix/plugin-install/lib/libnccl-net.so
```

Executed build commands, after staging the updated provider and cloning the
already-integrated dependency into `$FIX/libfabric`, were:

```bash
ROOT=/tmp/suet-nccl-retry-mkhalilo-20261014
FIX=$ROOT/endpoint-fix
source "$ROOT/env.sh"
cd "$FIX/libfabric"
./autogen.sh
./configure --prefix="$FIX/install" --enable-suet --enable-udp CFLAGS='-O2 -g'
make -j32
make install

git clone --shared "$ROOT/src/aws-ofi-nccl" "$FIX/aws-ofi-nccl"
cd "$FIX/aws-ofi-nccl"
./autogen.sh
./configure --prefix="$FIX/plugin-install" --with-libfabric="$FIX/install" \
  --with-cuda="$CUDA_HOME" --with-nvcc="$TC/bin/nvcc" \
  --with-hwloc="$TC" --disable-tests
make -j32 NVCC_GENCODE="$NVCC_GENCODE"
make install
```

The passing tests used:

```bash
export LD_LIBRARY_PATH="$FIX/plugin-install/lib:$FIX/install/lib:$ROOT/src/nccl/build/lib:$LD_LIBRARY_PATH"
export FI_PROVIDER='udp;ofi_suet' OFI_NCCL_PROTOCOL=SENDRECV NCCL_NET=Libfabric
export NCCL_P2P_DISABLE=1 NCCL_SHM_DISABLE=1
export NCCL_NET_GDR_LEVEL=LOC NCCL_PROTO=Simple
export NCCL_DEBUG=INFO NCCL_DEBUG_SUBSYS=INIT,NET,GRAPH
LD_DEBUG=libs timeout 180s "$ROOT/src/nccl-tests/build/all_reduce_perf" \
  -b 8 -e 1M -f 2 -g 2 -w 2 -n 5 -c 1
LD_DEBUG=libs timeout 180s "$ROOT/src/nccl-tests/build/all_reduce_perf" \
  -b 8 -e 1M -f 2 -g 4 -w 2 -n 5 -c 1
```

This demonstrates that the addressing fix resolves the observed data and
connection failures for the tested same-node host-buffer configuration.
It does not isolate which individual sub-change was necessary or establish
GPU-direct, multinode or NCCL/RUD behavior.

## Saved artifacts and cleanup

Login-node scratch directory:
`/scratch/mkhalilo/suet-nccl-20261014/endpoint-fix/`.

- `source.tar.gz`: the changed provider and test sources staged for the build.
- `fixed-stack.tar.gz`: corrected SUET/libfabric, relinked OFI plugin and
  endpoint-test binaries, approximately 15 MiB compressed.
- `evidence.tar.gz`: compute-node build/test/loader logs and dependency patch.
- `SHA256SUMS`: archive checksums.

Restore the fixed-stack archive into the canonical `$ROOT` above. It supplies
`endpoint-fix/install` and `endpoint-fix/plugin-install`; reuse the original
packed toolchain and NCCL/nccl-tests artifacts described in the baseline
retry report. The plugin's embedded RPATH must resolve to the corrected install.

Allocation `892469` was released after archiving, with elapsed time 26:38.
[Allocation cleanup](logs/2026-10-14-endpoint-fix/allocation-final.log) and
[stage exit codes](logs/2026-10-14-endpoint-fix/status.txt) are preserved.
No source commit or push was made.
