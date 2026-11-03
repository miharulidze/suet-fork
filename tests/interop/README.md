# SUET / uet-ref-prov UDP ping-pong

Two independent processes run on roce00 and roce01. The client sends
`ping`; the server validates it, prints it, then sends `pong`. The client
validates and prints the pong. Both print every TX/RX event with its round
number. Every message contains a deterministic round-specific payload,
checked byte for byte together with receive length. Normal provider close
is part of the pass criterion, rather than bypassed with `_exit`.

SUET can be either server or client. The same executable supports SUET
and reference endpoints, including same-provider controls. TCP port 60401
carries only endpoint identity and start/end tokens; application data is
sent through SUET's `udp;ofi_suet` FI_EP_RDM over UDP FI_EP_DGRAM, or the
reference provider's test UDP NIC backend. Polling drives provider progress
while waiting on the control channel. Both roles are real endpoint processes,
not a fork/socketpair simulation.

## Scope of the UDP backend

The pinned reference implementation has raw IP/Ethernet NIC backends,
not a native UDP socket backend. `udp_backend.c` installs a test backend
using GNU linker wrapping. It removes Ethernet, IPv4, entropy and the
validated reference IP-profile CRC on transmit, and rebuilds that envelope
on receive. UDP carries the unchanged PDS/SES bytes and message payload.
It never edits ACK flags, return codes, IDs, segmentation or payloads.
This exercises PDS/SES interoperability over a shared UDP payload profile;
it does not establish complete native UET IP/UDP encapsulation compatibility.
The backend is adapted from the earlier experimental adapter; `pingpong.c`
and the two-node runner are new implementations.

The reference explicit endpoint API (`ENABLE_VERBS=1`) is used to select
JobID 0, matching SUET. This is an API selection, not a verbs transport:
all reference data goes through the UDP backend. Default reference endpoints
authorize JobID 1, which is a different configuration and would be rejected.
Because the reference has no overflow-list buffering, the server posts its
next receive after validating ping and before transmitting pong. That keeps
the next ping eligible without adding a TCP barrier to each round.

## Versions and build

- SUET base: fork `refactor` commit `8076f2ecff1db8aff32b86f8b6ab08038046568a`.
- Reference: `9effe362ec13a7c2c1fa5a79146f5846f0c898c6`.
- Libfabric base: the repository default `94f3b1ac`, with the existing patches.
- Hosts: `ssh -J mkhalilov@blue.inf.ethz.ch root@roce00.cscs.ch` and roce01.
- Data addresses: roce00 `10.203.44.1`, roce01 `10.203.44.2`, interface
  `ens785f0np0`. UD-verbs validation uses `mlx5_0-dgram`.

Use an isolated directory on each host, for example:

```bash
ROOT=/root/suet-interop-20261103
mkdir -p "$ROOT/base"
git clone https://github.com/miharulidze/suet-fork.git "$ROOT/base/source"
# Check out the interop branch/commit being tested here.
git clone https://github.com/ultraethernet/uet-ref-prov.git "$ROOT/ref"
git -C "$ROOT/ref" checkout --detach 9effe362ec13a7c2c1fa5a79146f5846f0c898c6
cd "$ROOT/base/source"
WORKDIR="$ROOT/base/work" JOBS=8 bash build.sh > "$ROOT/base/build.log" 2>&1
UET_REF_SOURCE="$ROOT/ref" LIBFABRIC_SOURCE="$ROOT/base/work/libfabric" \
  LIBFABRIC_PREFIX="$ROOT/base/work/build" INTEROP_BUILD="$ROOT/base/interop" \
  bash tests/interop/build.sh > "$ROOT/base/interop-build.log" 2>&1
```

For exported reference sources, set `UET_REF_REVISION` to the pinned SHA.
Build artifacts record reference identity and test-source checksums. The
backend requires Linux and GNU linker `--wrap`; no system library is replaced.

## Run by hand

Server on roce00, for example SUET:

```bash
ROOT=/root/suet-interop-20261103/base
FI_PROVIDER='udp;ofi_suet' UET_PDS=pds UET_PDS_TX_TIMEOUT=100 \
  timeout -k 2s 20s "$ROOT/interop/pingpong" \
  suet server 10.203.44.1 10.203.44.1 60401 rod 16 10 /tmp/server.packets
```

Client on roce01, for example reference:

```bash
ROOT=/root/suet-interop-20261103/base
FI_PROVIDER='udp;ofi_suet' UET_PDS=pds UET_PDS_TX_TIMEOUT=100 \
  timeout -k 2s 20s "$ROOT/interop/pingpong" \
  ref client 10.203.44.2 10.203.44.1 60401 rod 16 10 /tmp/client.packets
```

Swap `suet`/`ref` to reverse provider roles, or use identical providers for
controls. Change `rod` to `rud` for unordered endpoints. The reference RTO
is 100 ms because packet logging can exceed its 5-ms default. It changes no
wire semantics. Both processes must exit zero and print `RESULT ... PASS`.
Timeouts, CQ errors, invalid payloads and close hangs fail the test.

## Two-node matrix and per-commit validation

From this checkout on the controlling computer:

```bash
python3 tests/interop/run.py --output /tmp/interop-matrix \
  --sizes 16,1024,8192,65537 --rounds 10
python3 tests/interop/run.py --output /tmp/interop-sr --sr 1
python3 tests/interop/validate.py --root /root/suet-interop-20261103/base \
  --output /tmp/interop-validation
```

`--pairs` lists server-client providers; defaults include all four pairings.
`--modes` defaults to ROD/RUD. Use `--root` to select each revision's isolated
remote installation. Outputs contain both process logs and an honest status
table; the runner returns nonzero on any failure. Packet captures stay in
the named remote results directory and can be decoded with `decode.py`.

Validation runs the existing complete UDP fabtests suite with original
iteration counts, three validated UD-verbs smoke tests (message/tagged
ping-pong and RMA write), then `fi_rdm_bw -p 'verbs;ofi_suet'` across the
two hosts: three samples each at 64 KiB/1 MiB, 2,000 iterations, 100 warmup,
window 64, CPU 2 pinned on each host. Throughput samples omit per-byte
validation so it does not mask transport costs; correctness is checked
separately. Keep CPU placement,
provider, interface and iteration counts identical for each revision. Compare
the median of each size against the unmodified base and inspect raw samples;
do not claim a performance regression from a single noisy sample.

## Baseline before provider fixes

The new harness builds on both hosts. With three rounds at 16 B and 8 KiB,
all eight same-provider controls pass, while all eight cross-provider cases
fail. Small cross-provider exchanges validate initial payloads before SUET
reports `FI_EINVAL` with provider code zero. The reference capture identifies
an ACK with default SES response and `RC_NULL`, which SUET classifies as an
error. Large SUET-to-reference sends also receive `RC_OP_VIOLATION` (24).
These are retained regressions, not treated as passing interop results.

Per-revision correctness logs, bandwidth samples and summaries are stored
under `results/`. Bug-fix commits add their own evidence and explanation.

The initial baseline passed 35/35 UDP fabtests, three validated UD-verbs
smoke cases, and six additional validated bandwidth samples. Performance
medians without per-byte validation are **6,155.64 MB/s at 64 KiB** and
**5,998.56 MB/s at 1 MiB**. An extra RMA-read capability probe returned
`FI_ENODATA` (61); this provider advertises write, not read. That unsupported
probe is retained in `base-performance/summary.json` with its actual exit
codes, and is not part of subsequent supported-operation smoke tests.

### Fix 1: default null responses

`DEFAULT_RESPONSE/RC_NULL` is transport-only. SUET now ignores it as an
operation response, while still processing the ACK's transport fields.
It cannot match an unrelated message ID or promote an explicit semantic
completion. Other opcodes with return code zero remain invalid.

The focused SES/PDS regression passes. All 35 UDP fabtests and three
supported UD-verbs smoke tests pass. Verified medians are 6,054.69 MB/s (64 KiB) and
5,968.81 MB/s (1 MiB), changes of -1.64% and -0.50% from baseline.
The first copied fabtests retained a baseline RPATH; those results are
invalidated. The complete rerun in `fix1-verified-validation/` checks the
loaded library on both hosts before testing. All eight
same-provider controls still pass. With the next receive preposted, both
cross-provider roles validate all ten 16-byte rounds in ROD and RUD; SUET
then hangs during close. Those four runs correctly remain failed in
`fix1-preposted/summary.json`. Close ACK handling and common payload-MTU
configuration are separate work, not claimed fixed here.

### Fix 2: explicit Close Command ACK

The reference ACKs the Close PSN through `ACK_PSN`, with `CACK_PSN` still
covering the last data packet. SUET waited for CACK to include Close and
retransmitted until the reference reported the already-freed PDC unknown.
The close predicate now accepts that explicit acknowledgement, rejects
probe ACK tokens and handles PSN wrap. A captured-wire regression passes.

All 35 UDP tests and three supported UD-verbs smoke tests pass against the
loader-verified fix2 library. Medians are 6,051.90 MB/s and 5,967.63 MB/s
(-1.69%/-0.52% from baseline). The GDB state in `fix2-close-state2.log`
confirms `pdc_closes_ok=1` and an empty initiator list. A target PDC remains,
so the cross-provider runs still time out during finalization; target-driven
close is a separate fix. Initial copied-binary validation is explicitly
invalidated; use `fix2-verified-validation/`.
