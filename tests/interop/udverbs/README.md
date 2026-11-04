# SUET / reference performance using the shared DGRAM adapter

The reference FI provider and [shared DGRAM backend](../dgram_backend.c) are
built once and loaded by both interoperability ping-pong and stock fabtests.
Both use the same PDS/SES payload mapping and `FI_EP_DGRAM` transport; there
is no special performance-only packet envelope. See [the interoperability
build/run guide](../README.md) for the complete architecture and node setup.

This work is on `interop/dgram-backends-20261104`, based on `b4c3e14`.
The original [CRC-on/off experiment](README-crc-experiment.md),
[CRC-on report](README-crc-on.md) and [1 KiB report](README-1k.md), including
all raw results, are preserved with their original adapter details.

## Build once for both applications

```bash
ROOT=/root/suet-interop-20261103
UET_REF_SOURCE="$ROOT/ref" \
LIBFABRIC_SOURCE="$ROOT/fix6/work/libfabric" \
LIBFABRIC_PREFIX="$ROOT/fix6/work/build" \
INTEROP_BUILD=/root/suet-interop-20261104/dgram/build \
bash tests/interop/build.sh
```

The output is `/root/suet-interop-20261104/dgram/build/provider/libuet_ref-fi.so`,
used by both test programs. The source is pinned reference `9effe362` plus
the existing [CRC switch patch](patches/0001-pds-benchmark-crc-switch.patch)
and the 4,000-byte payload configuration. The shared PDS/SES mapping requires
`UET_BENCH_DISABLE_CRC=1`; CRC-on results apply to the earlier IP-frame adapter
and are explicitly archived rather than mixed into this comparison.

## Benchmark

```bash
python3 tests/interop/udverbs/compare.py --stage correctness --samples 1 \
  --sizes 16,4000,4001,65536,1048576 --control-port 35800 \
  --output /tmp/shared-dgram-correctness
python3 tests/interop/udverbs/compare.py --stage performance --samples 3 \
  --control-port 36000 --output /tmp/shared-dgram-performance
python3 tests/interop/udverbs/plot.py /tmp/shared-dgram-performance
```

Defaults are `--root /root/suet-interop-20261104/dgram`, `--backend verbs` and
`--providers suet,ref-off`. `--backend udp` selects the exact same shared
adapter over the UDP provider, with a 1,024-byte application payload to fit
its 1,472-byte DGRAM limit. Verbs uses the native 4 KiB transport profile and
4,000 application bytes per data packet.

Each peer uses one endpoint, CPU 2, ROD, window 64, inject disabled, and
384-entry verbs TX/RX queues. Native SUET uses GBN. Correctness uses full
byte validation. Timed bandwidth uses at least 10,000 iterations plus 10,000
warmups, with equal settings for both providers and three repetitions.
Latency is RTT/2 in microseconds, and bandwidth is application payload MB/s.
Plots show medians with observed min/max error bars.

## Matched profiling

```bash
python3 tests/interop/udverbs/profile.py --samples 1 \
  --provider-root /root/suet-interop-20261104/dgram \
  --control-port 34100 --output /tmp/matched-dgram-profiles
```

The profiler reuses the recorded 1-MiB benchmark commands, selects the shared
provider binary, and uses 100,000 iterations plus 10,000 warmups on both
implementations. It records both sender and receiver. `perf stat` records
user cycles, instructions, task-clock, cache misses and context switches for
the whole invocation; those counts include setup, warmup and teardown.
Separate `perf record -e cycles:u -F 199 --call-graph dwarf,8192` runs collect
call stacks. Flat and caller reports use the same 6–12 second interval after
the first sample, excluding startup, warmup and the reference close-wait loop.
Profiling runs are kept out of the three-sample benchmark medians.


## Matched profile: where the remaining gap comes from

With this single shared PDS/SES adapter, the counter runs measured **6,104.34
MB/s for SUET** and **2,665.65 MB/s for reference CRC-off**, a **2.29x gap**.
The diagnostic call-stack runs measured 6,081.03 and 2,671.60 MB/s respectively.
No CRC routine appears in the sampled profiles; all profile jobs exited zero.
Both peers used one endpoint/core, 4,000-byte payloads, window 64 and the same
verbs DGRAM provider, queues and fabric installation.

| Counter, per application byte | SUET sender | Reference sender | SUET receiver | Reference receiver |
| --- | ---: | ---: | ---: | ---: |
| User CPU cycles | 0.486 | 1.164 | 0.538 | 1.265 |
| Retired user instructions | 0.833 | 1.619 | 0.673 | 2.305 |

The reference consumes **2.39x sender cycles/byte** and **2.35x receiver
cycles/byte**. It retires **3.42x receiver instructions/byte**. This matches
the throughput gap: substantially more CPU work is performed per delivered
byte, even after CRC removal. Counter values cover the full invocation;
the profiles below isolate the identical steady-state interval.

| Steady-state self-cycle samples | SUET sender | Reference sender | SUET receiver | Reference receiver |
| --- | ---: | ---: | ---: | ---: |
| Copies and zeroing | 0.83% | 26.47% | 41.47% | 16.77% |
| Heap management | 7.06% | 10.39% | 3.27% | 9.82% |
| Locking | 25.06% | 18.02% | 11.22% | 15.20% |
| Timekeeping | 3.32% | 3.07% | 1.64% | 13.38% |
| Reliability bitmaps | 0.00% | 5.03% | 0.00% | 6.30% |

Percentages are within each core at different throughputs; a lower percentage
can still mean greater cost per application byte. The caller stacks and source
identify these concrete costs:

- **Extra full-payload copies.** The reference PDS copies payload into its
  allocated frame in `uet_pds_tx_pkt`; the shared asynchronous DGRAM NIC then
  copies into its registered TX slot. On RX, the NIC copies into the reference
  frame, and SES copies into the posted application buffer. That is two TX
  and two RX payload copies. Native SUET's large-message TX uses registered
  IOVs, and its receive path copies into the application buffer. The reference
  sender's copy stacks specifically account for 15.16% in PDS packet assembly
  and 8.05% in the NIC TX copy, versus less than 1% total copying/zeroing on the
  native sender.
- **Per-packet allocation and freeing.** The reference allocates a PDS packet
  descriptor/frame in `uet_pds_tx_pkt`, allocates an RX frame in
  `uet_pds_sec_rx_pkt`, and frees retained packets while advancing its window.
  Native SUET reuses packet/descriptor pools. This difference is visible in
  the calloc/free stacks and the heap-management samples above.
- **Progress, timer and window bookkeeping.** Every `uet_cq_read` takes the
  endpoint mutex and runs `uet_ep_progress_locked`: message aging, sync-group
  aging, one receive packet, transmit progress and deferred sends. The reference
  also physically shifts bitmap/pointer windows (`bm_shift_right`) as ACKs
  advance. Native SUET drains batches and uses its circular reliability window.
  Reference receiver timekeeping is 13.38% of samples, versus 1.64% for SUET;
  reference bitmap work is another 6.30%.
- **Locking is present in both stacks.** The caller reports distinguish
  reference API/CQ locks from verbs send/CQ locks. Native sender locking is
  itself substantial; the result does not justify attributing the whole gap
  to a reference-only lock or to the NIC adapter alone.

The FI adapter uses the reference vector entrypoint, but the reference collapses
a single IOV to its contiguous-buffer representation. These scalar benchmarks
do **not** execute the multi-IOV gather allocation/copy path. No scalar/vector
optimization is claimed as an explanation for these measurements.

The next performance changes to evaluate are packet-buffer pools, eliminating
frame/registered-slot copies through an explicit asynchronous buffer-lifetime
contract, and batching reference progress/timer/window updates. Those are
proposed optimizations, not measured speedups. The current profile attributes
the remaining cost across these paths; it does not establish an additive
speedup from independently removing them.

Raw evidence: [all profiling commands/exits](results/matched-dgram-profile-20261104/runs.json),
[counter/category analysis](results/matched-dgram-profile-20261104/analysis.json),
[SUET sender callers](results/matched-dgram-profile-20261104/suet-record-r0-1-callers.txt),
[reference sender callers](results/matched-dgram-profile-20261104/ref-off-record-r0-1-callers.txt),
[SUET receiver callers](results/matched-dgram-profile-20261104/suet-record-r0-0-callers.txt),
[reference receiver callers](results/matched-dgram-profile-20261104/ref-off-record-r0-0-callers.txt).
Run `analyze_profile.py RESULTS_DIRECTORY` to regenerate the analysis JSON.

## Shared-adapter validation baseline

Both hosts built the same reference adapter and libfabric-only application.
The initial eight single-packet controls/mixed cases passed on verbs and UDP.
With packet capture disabled, ten 1-MiB rounds passed in both mixed roles and
both ROD/RUD modes over verbs. The native basic fabtests suite passed 35/35.
Build/source/library provenance is recorded for
[roce00](results/dgram-baseline-roce00.cscs.ch-provenance.json) and
[roce01](results/dgram-baseline-roce01.cscs.ch-provenance.json).

The baseline UDP 1-MiB mixed cases exposed loss-recovery bugs. SUET's ROD gap
NACK omits Expected_PSN in its payload; the pinned reference handles ROD
out-of-order NACKs with individual retries instead of GoBackN. Native GBN on
RUD also reports an in-window gap as an out-of-window error. These are tracked
as protocol fixes, with baseline failures preserved. The
[UEC ROD requirements, section 3.5.21.1](https://ultraethernet.org/uec-1-0-spec)
require the missing expected PSN in a ROD NACK payload and ordered GoBackN
retransmission beginning there.
