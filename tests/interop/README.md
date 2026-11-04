# SUET / reference interoperability over libfabric DGRAM

Ping-pong and stock fabtests load the same `uet_ref` module and use the same
[dgram_backend.c](dgram_backend.c). Select `verbs` or `udp` as the underlying
`FI_EP_DGRAM` provider. SUET uses its native DGRAM backend. The reference uses
its original PDS/SES implementation at commit
`9effe362ec13a7c2c1fa5a79146f5846f0c898c6`, with only the
[CRC switch](reference-crc-switch.patch) and a 4,000-byte maximum-payload
configuration. No reference reliability, NACK or ordering changes are applied.

All source and tools live directly in this directory:

| Files | Purpose |
| --- | --- |
| `pingpong.c`, `run.py` | Validated ping/pong with either provider as client or server |
| `ref_provider.c`, `ref_address.h` | Reference-provider libfabric FI_MSG adapter and address format |
| `dgram_backend.c`, `dgram_backend.h` | Shared reference NIC backend for verbs and UDP DGRAM |
| `build.sh`, `reference-crc-switch.patch` | Build the adapter, application and RUD setup helper |
| `endpoint_order.c` | Select an unordered endpoint contract for stock RUD fabtests |
| `compare.py`, `plot.py` | Matched fabtests correctness, latency and bandwidth comparison |
| `profile.py`, `analyze_profile.py` | CPU profiling of the same benchmark commands |
| `decode.py` | Decode optional DGRAM packet captures |

Generated results, plots, logs and captures belong outside the checkout; pass
an external `--output`, such as `/tmp/suet-interop-run`. Raw logs default to a
separate temporary directory, recorded as `log_directory`; `--log-dir` overrides
it. The old `results` directories and obsolete experiment helpers have been
removed. Earlier measurements remain in
[Git history](https://github.com/miharulidze/suet-fork/tree/57c6b0a4519885189542bce74e3338f56f4f60cd/tests/interop).

## Build on both roce machines

Connect using:

```bash
ssh -J mkhalilov@blue.inf.ethz.ch root@roce00.cscs.ch
ssh -J mkhalilov@blue.inf.ethz.ch root@roce01.cscs.ch
```

Use the SUET install at `/root/suet-interop-20261104/native-gap/build`
(libfabric base `94f3b1ac` plus the repository patches) and the reference export
at `/root/suet-interop-20261103/ref` (`9effe362`). To build a fresh native SUET
install, run the repository's root `build.sh` with an external `WORKDIR` first;
it builds libfabric, SUET and fabtests. Substitute that install and libfabric
source path below and pass its prefix through `--fabric` to the controllers.

From this checkout on each machine:

```bash
UET_REF_SOURCE=/root/suet-interop-20261103/ref \
LIBFABRIC_SOURCE=/root/suet-interop-20261104/native-gap/libfabric \
LIBFABRIC_PREFIX=/root/suet-interop-20261104/native-gap/build \
INTEROP_BUILD=/root/suet-interop-20261104/dgram/build \
bash tests/interop/build.sh
```

The single builder produces `pingpong`, `provider/libuet_ref-fi.so` and
`endpoint_order.so` under `INTEROP_BUILD`. It patches only a private reference
source copy. Ping-pong and stock fabtests load that same provider module;
there is no separate reference core or custom UDP socket backend in ping-pong.
Build on Linux with a C compiler, libfabric headers/libraries, reference build
dependencies, Python 3, `patch` and `sha256sum` installed.

## Run interoperability tests

The controller defaults to roce00 as server and roce01 as client. Data addresses
are `10.203.44.1` and `10.203.44.2` on `ens785f0np0`. Assign these addresses and
ensure the RDMA links are active before testing verbs.

```bash
python3 tests/interop/run.py \
  --pairs suet-ref,ref-suet --modes rod,rud --backends verbs,udp \
  --sizes 16,4001,1048576 --rounds 10 \
  --output /tmp/suet-interop-run

python3 tests/interop/run.py --sr 1 --modes rud \
  --pairs suet-ref,ref-suet --sizes 16,1048576 \
  --control-port 35500 --output /tmp/suet-interop-rud-sr
```

Pair names are `server-client`: `suet-ref` means SUET server and reference
client. Each client sends ping; the server validates it and sends pong; the
client validates pong. Both print the operations. The full byte pattern,
length and round number are checked. `--server-host`, `--client-host`,
`--server-ip` and `--client-ip` change the physical direction. `--root` selects
the parent of the shared `build` directory; `--fabric` selects the SUET install.
`--sr 0` uses native GBN and `--sr 1` selects native SR.

TCP exchanges endpoint identities and synchronizes the peers. Data always uses
DGRAM. Addresses come from `fi_getname`; the application translates only the
out-of-band logical PID/RI/initiator fields needed for AV insertion. It does not
translate UET packet headers.

The shared backend carries PDS/SES bytes. It removes the reference NIC's
internal Ethernet/IP envelope and disabled CRC trailer on TX, reconstructing
the envelope on RX. Both transports use `UET_BENCH_DISABLE_CRC=1`. Verbs uses a
4 KiB DGRAM limit, leaving 4,000 application bytes per request. UDP uses 1,024
application bytes to fit its 1,472-byte DGRAM limit. Both providers therefore
use matching segmentation for each backend.

Captures are off by default. For a small diagnostic run, add
`--capture --sizes 16,4001 --rounds 3`. Remote captures go under
`/tmp/suet-interop`, and their directory is recorded in the summary. Decode a
copied capture with:

```bash
python3 tests/interop/decode.py /tmp/server.packets --limit 20
```

## Fabtests correctness and performance

Both implementations use the same stock `fi_rdm_pingpong` and `fi_rdm_bw`
binaries, with one endpoint and one process per node, CPU 2, window 64 and
384-entry TX/RX DGRAM queues. Native SUET uses GBN. The reference has CRC off.

```bash
python3 tests/interop/compare.py --mode rod --backend verbs \
  --stage correctness --samples 1 --sizes 16,4000,4001,65536,1048576 \
  --output /tmp/suet-fabtests-correctness

python3 tests/interop/compare.py --mode rod --backend verbs \
  --stage performance --samples 3 --control-port 40400 \
  --output /tmp/suet-fabtests-performance

python3 tests/interop/plot.py /tmp/suet-fabtests-performance
```

Use `--mode rud` to test unordered delivery or `--backend udp` to change the
transport. For RUD, `endpoint_order.so` changes only the returned FI_MSG endpoint
ordering contract during discovery; it refuses explicit ordering requirements
and RMA/atomic tests. It does not intercept data-path calls or modify protocols.

Correctness runs enable fabtests byte validation. Performance runs use matched
iterations/warmups for both providers. Bandwidth uses at least 10,000 warmups
and 10,000 measured iterations. Plotting needs Python's `matplotlib` package
and reads only existing measurements; it does not run experiments. The figure
omits the 4,000-byte application message point and retains 4 KiB (4,096 bytes).
Tables and CSV retain every measured size.

Latency is fabtests' `usec/xfer` column unchanged: fabtests already divides the
round-trip time by two. The plot adds no further division. Each point is the
median across repetitions; error bars show the observed minimum and maximum.
Bandwidth is application-payload MB/s.

## Profile the same workload

Use an external `runs.json` from `compare.py` as the baseline. Both peers need
`perf` and permission to access its counters.

```bash
python3 tests/interop/profile.py \
  --baseline /tmp/suet-fabtests-performance/runs.json \
  --provider-root /root/suet-interop-20261104/dgram \
  --fabric /root/suet-interop-20261104/native-gap/build \
  --samples 1 --control-port 40600 --output /tmp/suet-profile
python3 tests/interop/analyze_profile.py /tmp/suet-profile
```

## Known interoperability limitation

The recorded original-protocol mixed-provider matrix passed 31/32 cases.
Single-packet ping-pong passed for ROD and RUD over both backends in both
roles. RUD passed all 20 cases, including native GBN/SR coverage. ROD passed
6/6 verbs cases and 5/6 UDP cases.

The remaining failure was a 1 MiB UDP ROD ping from the reference client to
the SUET server. Reference exhausted its NACK retry budget and reported a CQ
I/O error; SUET timed out receiving the complete ping. Its pinned ROD sender
retransmits the later `nack_psn` rather than replaying from Expected_PSN in the
`ROD_OOO` NACK payload. The initial packet gap's loss/reordering cause was not
established. No reference protocol change or SUET workaround is applied.
