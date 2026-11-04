> Historical testbench through `b4c3e14`, using the former custom UDP socket adapter. The current testbench replaces it with the same libfabric DGRAM adapter used for performance benchmarking.

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

## Current result

The final build passes all **96/96** cases: four provider pairings, ROD/RUD,
six sizes (16, 1,024, 1,025, 8,192, 65,537 and 1,048,576 bytes), ten rounds,
and SUET GBN/SR. GBN uses roce00 as server; SR reverses the physical hosts.
Payloads and receive lengths are validated, and both processes close normally
with zero SUET close-error counters. This includes single-packet messages
and multi-packet messages up to 1,024 segments per message.

All 35 existing UDP fabtests and validated UD-verbs smoke/bandwidth checks
were run for each committed provider revision. The final sustained bandwidth
comparison differs from baseline by less than 0.4%; details and raw results
are below. The validated installation is already available on both hosts at
`/root/suet-interop-20261103/fix6`; `base` is the unfixed comparison build.
The runner defaults to `fix6`. The build example below uses a fresh directory.

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
ROOT=/root/suet-interop-rebuild
mkdir -p "$ROOT"
git clone --branch interop/udp-pingpong-20261103 \
  https://github.com/miharulidze/suet-fork.git "$ROOT/source"
# Use git checkout <commit> here when reproducing an earlier revision.
git clone https://github.com/ultraethernet/uet-ref-prov.git "$ROOT/ref"
git -C "$ROOT/ref" checkout --detach 9effe362ec13a7c2c1fa5a79146f5846f0c898c6
cd "$ROOT/source"
WORKDIR="$ROOT/work" JOBS=8 bash build.sh > "$ROOT/build.log" 2>&1
UET_REF_SOURCE="$ROOT/ref" LIBFABRIC_SOURCE="$ROOT/work/libfabric" \
  LIBFABRIC_PREFIX="$ROOT/work/build" INTEROP_BUILD="$ROOT/interop" \
  bash tests/interop/build.sh > "$ROOT/interop-build.log" 2>&1
```

For exported reference sources, set `UET_REF_REVISION` to the pinned SHA.
Build artifacts record reference identity and test-source checksums. The
backend requires Linux and GNU linker `--wrap`; no system library is replaced.

## Run by hand

Server on roce00, for example SUET:

```bash
ROOT=/root/suet-interop-rebuild
FI_PROVIDER='udp;ofi_suet' FI_OFI_SUET_PAYLOAD_MTU=1024 UET_PDS=pds UET_PDS_TX_TIMEOUT=100 \
  timeout -k 2s 20s "$ROOT/interop/pingpong" \
  suet server 10.203.44.1 10.203.44.1 60401 rod 16 10 /tmp/server.packets
```

Client on roce01, for example reference:

```bash
ROOT=/root/suet-interop-rebuild
FI_PROVIDER='udp;ofi_suet' FI_OFI_SUET_PAYLOAD_MTU=1024 UET_PDS=pds UET_PDS_TX_TIMEOUT=100 \
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
# Single-packet messages, including one full payload segment.
python3 tests/interop/run.py --root /root/suet-interop-rebuild --output /tmp/interop-single \
  --sizes 16,1024 --rounds 10
# Multi-packet messages: boundary + 1, 8 segments, partial tail and 1 MiB.
python3 tests/interop/run.py --root /root/suet-interop-rebuild --output /tmp/interop-multi \
  --sizes 1025,8192,65537,1048576 --rounds 10
python3 tests/interop/run.py --root /root/suet-interop-rebuild --output /tmp/interop-sr --sr 1 \
  --sizes 16,1024,1025,8192,65537,1048576 --rounds 10 \
  --server-host roce01.cscs.ch --client-host roce00.cscs.ch \
  --server-ip 10.203.44.2 --client-ip 10.203.44.1
python3 tests/interop/validate.py --root /root/suet-interop-rebuild \
  --output /tmp/interop-validation
```

`--pairs` lists server-client providers; defaults include all four pairings.
`--modes` defaults to ROD/RUD. Use `--root` to select each revision's isolated
remote installation. Choose a new output directory for each run. Outputs
contain both process logs and an honest status
table; the runner returns nonzero on any failure. Packet captures stay in
the named remote results directory and can be decoded with `decode.py`.
The SR option selects SUET's reliability algorithm; reference-provider code
and configuration are otherwise unchanged. The runner configures a shared
1,024-byte payload MTU. `--payload-mtu 0`
deliberately probes the old transport-derived SUET size, not a compatible
deployment. The [UET specification](https://ultraethernet.org/wp-content/uploads/sites/20/2026/01/UE-Specification-1.0.2-1.pdf)
sections 3.3.1 and 3.4.1.11 require a consistent, out-of-band configured
payload MTU across communicating FEPs. ROD/RUD does not negotiate it.

Validation runs the existing complete UDP fabtests suite with original
iteration counts, three validated UD-verbs smoke tests (message/tagged
ping-pong and RMA write), then `fi_rdm_bw -p 'verbs;ofi_suet'` across the
two hosts: three samples each at 64 KiB/1 MiB, 2,000 iterations, 100 warmup,
window 64, CPU 2 pinned on each host. Throughput samples omit per-byte
validation so it does not mask transport costs; correctness is checked
separately. Keep CPU placement,
provider, interface and iteration counts identical for each revision. Compare
the median of each size against the unmodified base and inspect raw samples;
do not claim a performance regression from a single noisy sample. The 64-KiB
short run lasts only about 20 ms. For a sustained comparison after a recorded
correctness pass, use the same longer settings for both revisions:

```bash
python3 tests/interop/validate.py --root /root/suet-interop-20261103/fix6 \
  --output /tmp/interop-sustained --skip-suite \
  --bandwidth-iterations 100000 --bandwidth-warmup 10000
```

`--bandwidth-samples` defaults to three. The output records the configuration
alongside raw samples. Run bandwidth checks without builds or interop matrices
in parallel on these hosts.

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

### Fix 3: target-driven teardown

SUET now issues Close Request CPs for target PDCs left during domain close,
routes received Close Requests to the initiator map, and waits for the
peer's Close Command before releasing target state. Requests retry with a
bounded budget; an orphaned peer cannot leave domain close spinning forever.
The focused regression covers ROD/RUD, retries, peer replies, request routing
and orphan cleanup. The network runner requires zero `pdc_close_in_err`
counters, so an orphan timeout cannot masquerade as a clean handshake.

All eight provider-pair/mode cases pass ten 16-byte rounds and clean close,
including both cross-provider roles. All 35 UDP fabtests and three supported
UD-verbs smoke tests pass. Bandwidth medians are 6,037.96/5,933.71 MB/s
(-1.91%/-1.08% from baseline), within baseline sample variation. Large-message
interop requires the common payload MTU introduced in fix 4.


### Fix 4: common payload MTU

SUET previously derived a 1,416-byte segment payload from the UDP transport,
while the reference expects 1,024 bytes. The UET deployment must configure
one common size; this is not an obligation for the receiver to infer arbitrary
segmentation. `FI_OFI_SUET_PAYLOAD_MTU` now provides that configuration and
checks that it fits the backend. Zero preserves the legacy default.

The focused regression covers a valid 1,024-byte setting, a valid size too
large for the backend, invalid values, and the unchanged default. The GBN and
SR matrices each cover all four provider pairings, ROD/RUD, six sizes
(16, 1,024, 1,025, 8,192, 65,537 and 1,048,576 bytes), and ten rounds.
All 96 cases pass, including clean close with zero close-error counters.
Results are recorded in `fix4-gbn/` and `fix4-sr/`.

All 35 UDP fabtests and three validated UD-verbs smoke tests pass. With the
same legacy/default MTU as the baseline, UD-verbs medians are 5,971.39 MB/s
at 64 KiB and 5,938.40 MB/s at 1 MiB (-2.99%/-1.00%), within baseline sample
variation. Those throughput results do not measure the separately configured
1,024-byte interop profile.


### Fix 5: request ACKs at message tails

The sender requested ACKs when filling its congestion window, but omitted
AR on a message tail that could leave the PDC idle. With a reference peer
using coalesced ACKs, short exchanges then depended on RTO retransmission.
Every message tail now requests an ACK, including single-packet/empty
messages; window-driven requests remain enabled for intermediate packets.

The focused PDS framing test covers ROD/RUD, copied and zero-copy data,
empty messages and atomic framing. In the saved ten-round 16-byte trace,
the number of original SUET packets carrying AR changes from 0/10 to 10/10;
retransmissions change from nine to one. This does not claim to eliminate
all retransmissions on a logging-heavy test host.

All 35 UDP fabtests and three validated UD-verbs smoke tests pass. Medians
are 6,018.83/5,998.80 MB/s at 64 KiB/1 MiB (-2.22%/+0.004% from baseline).
The GBN matrix passes all 24 cases at 16 bytes, 1,025 bytes and 1 MiB,
with ten rounds across all provider pairings and ROD/RUD.
The equivalent SR matrix also passes all 24 cases, giving 48/48 with clean
shutdown and zero close-error counters for this revision.


### Fix 6: request an ACK for Close Command

The normal Close Command path omitted AR even though specification section
3.5.16.4 requires both close CPs to request an ACK. The reference currently
replies regardless, but relying on that behavior leaves SUET dependent on
a peer implementation detail. Close Command now sets AR on its first send;
its existing retry path preserves it. The focused ROD/RUD regression checks
the emitted control packet, fresh PSN, zero payload and close-wait state.

The complete 35-case UDP suite and all three validated UD-verbs smoke tests
pass. The short-run medians were lower, so baseline and final were also
compared with 100,000 iterations, 10,000 warmups and three samples per size,
with no concurrent builds or interop runs. Raw results are retained; these
are separate configurations, not interchangeable samples:

| Configuration | Baseline 64 KiB, MB/s | Final 64 KiB, MB/s | Baseline 1 MiB, MB/s | Final 1 MiB, MB/s |
| --- | ---: | ---: | ---: | ---: |
| Original short comparison | 6,155.64 | 5,882.15 | 5,998.56 | 5,754.37 |
| Sustained comparison | 6,513.46 | 6,492.63 | 6,120.77 | 6,115.60 |

Sustained changes are -0.32% and -0.08%; both final medians stay within
0.4% of baseline. See `base-sustained/`, `fix6-sustained/`,
`base-final-recheck/` and `fix6-validation/`. The short 64-KiB phase lasts
about 20 ms and is sensitive to startup/frequency variation.

The final matrices in `fix6-gbn/` and `fix6-sr-reversed/` pass 96/96 cases.
Sixteen reference-side capture checks confirm both directions and delivery
modes: a 1,025-byte SUET message uses 1,024 + 1 byte payloads, and a 1-MiB
message uses 1,024 payload segments. They also verify the actual ROD/RUD
wire type, AR on message tails, and AR on Close Commands. The two
`fix6-*-wire-and-provenance.json` files record these counts, capture paths,
compiler/library identities and source hashes matched against the compiled
provider tree on each host. Full captures remain on the hosts; representative
small captures are retained under `results/fixtures/`.


## One-to-one UD-verbs comparison

The [DGRAM-backed reference-provider benchmark adapter](udverbs/README.md)
runs the same stock fabtests latency/bandwidth binaries on SUET and the
reference stack, using libfabric `FI_EP_DGRAM` on both. It includes the native
4 KiB UD comparison, source/build provenance, raw samples, CSV and plots,
alongside the archived 1 KiB comparison. The standalone
`benchmark/ud-crc-on-off-20261103` branch also supplies a reference CRC-switch
patch and measures both CRC modes while preserving the original CRC-on data.
