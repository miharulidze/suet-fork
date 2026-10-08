# SUET over the htsim network

`htsim_suet` runs the real SUET libfabric provider over the `htsim` DGRAM
provider. It does not instantiate `UecSrc` or `UecSink`. The network still uses
htsim's host serialization queues, fat-tree switches, ECMP routing, link
serialization, propagation delays, finite switch queues and packet drops.

The packet chain is:

```
traffic-matrix row -> fi_tsendmsg -> SUET SES/PDS -> htsim DGRAM TX queue
  -> host queue -> links/switches -> destination host -> DGRAM RX CQ
  -> SUET PDS/SES -> application CQ -> payload verification / trigger
```

`UecNIC` directly calls `UecSrc` and handles `UecBasePacket` control traffic.
This runner uses the existing host output queue as the NIC serialization
point instead. It does not double-charge a second NIC serialization delay.
The existing `htsim_uec` executable is unchanged.

## Build and run

All SUET-specific simulator code lives in this directory:

- `dgram.cpp` / `dgram.h`: libfabric DGRAM provider and network bridge.
- `suet_main.cpp`: network setup, libfabric objects, simulated clock and traffic driver.
- `dgram_test.cpp` / `simulation_test.py`: provider contract and end-to-end tests.
- `CMakeLists.txt` / `build.sh`: standalone build and dependency setup.

The upstream `uet-htsim` network model is an external dependency; no patches
or SUET integration files are required in that repository.

From the SUET repository root:

```sh
./build.sh --htsim
./build.sh --test-htsim

FI_PROVIDER_PATH="$PWD/suet-workspace/htsim-build" \
  suet-workspace/htsim-build/htsim_suet \
  -tm suet-workspace/deps/uet-htsim/htsim/sim/datacenter/connection_matrices/one.cm \
  -end 10000
```

`--htsim` builds libfabric, fabtests, the DGRAM plugin, the runner and its tests.
It fetches the simulator into `$WORKDIR/deps/uet-htsim` at the validated revision
`65ea315b25725df9791a5b93b61cbd6938cba0a8`. Set `HTSIM_REPO` and `HTSIM_REF`
to select another source/revision for a new managed checkout. Existing managed
checkouts must match the requested revision; they are never reset automatically.

To reuse an existing simulator checkout and SUET-enabled shared libfabric install:

```sh
HTSIM_SOURCE_DIR=/absolute/path/to/uet-htsim \
LIBFABRIC_ROOT=/absolute/path/to/suet-enabled/install \
  ./build.sh --htsim-only
./build.sh --test-htsim
```

A supplied `HTSIM_SOURCE_DIR` is used as-is without fetching or checking out
another revision. `--htsim-only` does not rebuild libfabric. The install must
contain SUET's clock extension; older builds fail when the runner installs it.
The usual `./build.sh` and `./build.sh --test` continue to build and test SUET
without requiring htsim or CMake.

The default build directory is `$WORKDIR/htsim-build`, where `WORKDIR` defaults
to `$PWD/suet-workspace`. Override it with `HTSIM_BUILD_DIR`; use the same
value when running `--test-htsim`. Dependencies for this build are CMake 3.16+,
a C++17 compiler, Python 3, and Git when fetching the simulator.

The CMake project can also be built directly:

```sh
cmake -S htsim -B suet-workspace/htsim-build \
  -DHTSIM_SOURCE_DIR=/absolute/path/to/uet-htsim \
  -DLIBFABRIC_ROOT=/absolute/path/to/suet-enabled/install
cmake --build suet-workspace/htsim-build \
  --target htsim_suet fabric_dgram_test suet_rel_test --parallel
ctest --test-dir suet-workspace/htsim-build --output-on-failure
```

Outputs are `htsim_suet` and `libhtsim-fi.so` (also on macOS) in the build
directory. Point `FI_PROVIDER_PATH` there when running the simulator manually;
tests set it automatically. The executable and plugin use the same libfabric
installation. The plugin uses only libfabric's installed public headers.
CMake records library search paths for the build outputs.

Optional arguments:

| Argument | Meaning / default |
| --- | --- |
| `-topo FILE` | Existing htsim topology file; otherwise generate a fat tree |
| `-tiers 2\|3` | Generated topology tiers; default 3 |
| `-linkspeed GBPS` | Generated topology link speed; default 100 |
| `-hop_latency NS` | Generated topology link delay; default 1000 |
| `-q BYTES` | Switch queue capacity; default 1 MiB |
| `-end US` | Simulation deadline; default 100000 |
| `-poll_ns NS` | Host libfabric progress interval; default 100 |
| `-dgram_queue COUNT` | Per-endpoint datagram queue limit; default 1024 |
| `-overhead BYTES` | Network bytes in addition to SUET wire bytes; default 42 |
| `-seed N` | Topology/routing seed; default 1 |
| `-drop_first N` | Deliberately drop the first N transmitted packets |

A topology file supplies its own link rates and delays. `-q` supplies the
queue capacity at every switch tier, as in the existing UEC runner.
Queues use htsim's ECN implementation with overflow drops by default.
`-trim_queues 1` selects composite queues that trim overflowing data packets.
CE and trimming metadata cross the DGRAM interface into SUET. UEC congestion
control, UecNIC multi-port arbitration and PCIe modeling are not implemented.
SUET's optional bounded AIMD response is not NSCC or UEC congestion control.

## Time, traffic and completion

There are no sockets, background progress threads or wall-clock sleeps on the
packet path. A simulator event polls the application CQs, admits pending sends
into htsim and schedules the next poll. The configured poll interval models
host progress latency; link/switch events retain their own exact timestamps.

The runner installs a per-domain elapsed-millisecond callback using
`fi_set_val(domain, FI_SUET_CLOCK, &clock)` before creating endpoints. The
callback derives time from `EventList::now()`. SUET retains its existing 1 ms
initial retry interval and exponential backoff. Advancing wall time cannot
cause a simulator retransmission.

`FI_SUET_CLOCK_ASYNC_CLOSE` makes SUET domain close retryable with `-FI_EAGAIN`.
The runner closes application endpoints/CQs/AVs, continues progressing every
domain and the network, and retries domain close until the PDS close handshake
finishes. The DGRAM endpoint also defers close until all accepted sends have
entered the network. Normal SUET clock and blocking-close defaults are unchanged.

Each connection-matrix row becomes one verified tagged message; rows sharing
hosts reuse their SUET domains/endpoints and PDS connections. The runner uses
unique internal tags so concurrent messages cannot match the wrong receive.
It honors matrix start times **in picoseconds**, explicit completion triggers,
single-shot/multishot/barrier trigger definitions and configured link failures.
`send_done_trigger` follows the SUET transmit CQ completion with
`FI_DELIVERY_COMPLETE`; `recv_done_trigger` follows the verified receive CQ
completion. These are completion dependencies, not UecSrc's last-packet-sent
notification. Matrix `prio` does not select a separate queue class. Data packets use
normal priority; control packets and trimmed headers use high priority.

A successful run reports each message's start, finish and completion time,
then packet/drop totals including the shutdown handshake. It verifies every
payload byte and returns a nonzero status for CQ errors, data mismatches or an
expired simulation deadline, including a deadline reached during shutdown.
Message buffers consume real memory while transfers are active.

## DGRAM contract and tests

The single-threaded provider advertises `FI_EP_DGRAM`, `FI_MSG`, `FI_SOURCE`
and `FI_SOURCE_ERR`, with manual progress and `FI_THREAD_DOMAIN`. It provides
AVs, message-format CQs, posted receives, send/receive IOVs, inject, endpoint
names, cancellation and local registration descriptors. It deliberately does
not implement transport reliability. A full queue returns `-FI_EAGAIN`; no
posted receive or a full RX CQ drops the arriving datagram. Unknown sources
produce `FI_EADDRNOTAVAIL` CQ errors with a raw source address for SUET to insert.

Accepting a send copies its bytes. Taking it into the network posts local TX
completion, independently of arrival or loss. Network frames contain addresses,
bytes and receive metadata, so in-flight frames never retain provider FIDs or application
buffer pointers. Receive completions are generated only at arrival.

The standalone reliability test checks bitmap windows, sequence wraparound and
GBN recovery without linking libfabric. The provider test checks completion timing, source discovery, truncation,
backpressure, cancellation and object lifetimes. The integration test checks
segmentation, zero-length messages, deterministic runs, slower-link timing,
incast with a small DGRAM queue, timed/triggered messages, simulated-time loss
recovery and timeout reporting. Run `-drop_first 1` to exercise retransmission.

For ASan/UBSan checks of the new provider and runner, use a separate
`HTSIM_BUILD_DIR` with `SUET_SANITIZE=ON ./build.sh --htsim-only`, or configure
CMake with `-DSUET_SANITIZE=ON`, and run the same tests. The macOS
validation used `ASAN_OPTIONS=detect_leaks=0`; this does not constitute a leak
audit of the simulator and its dependencies.

Validated on macOS with the SUET-enabled libfabric build and the existing
`one.cm` and `perm_32n_32c_2MB.cm` matrices. The 32-flow permutation over
`leaf_spine_tiny.topo` completed all 32 messages, verified all 64 MB of payload,
and recovered from 4,095 network drops at the default seed and queue size.
The simulator checks and the DGRAM contract test passed both normally and
with ASan/UBSan. SUET's four focused test programs and its 35-case UDP suite
also passed after the clock/close changes.

Select endpoint ordering with `-pdc rod` (default, `FI_ORDER_SAS`) or
`-pdc rud` (no ordering requested). The runner checks the request type on the
wire. Both use the same provider reliability and CC implementation.
`-reorder_every N` delays every Nth data packet by 10 us;
`-drop_per_mille N` injects reproducible random data loss; and
`-burst_every N -burst_length M` drops M packets per N data transmissions.
The integration suite verifies both delivery types with these faults and
multiple concurrent messages sharing a peer.


`-trim_every N` trims every Nth data transmission starting with the first,
including the opening SYN; `-trim_lasthop 1` exercises the last-hop NACK code.
`FI_OFI_SUET_SELECTIVE_REPEAT=1` selects SR and `FI_OFI_SUET_ECN=1` enables
congestion-window response. Go-Back-N and fixed-window CC remain defaults.
The test matrix covers both PDC types with both algorithms, reordering,
random/burst loss, actual queue CE marking, 16 KiB trimming queues, and injected
trimming with both NACK codes. It checks payloads, wire SACKs, echoed CE marks,
and trim NACK counts. All four CTests include the independent CC unit test.

## Entropy and oblivious packet spraying

The build applies `patches/0002-sendmsg-ev.patch` to libfabric alongside the
provider-registration patch. An existing install used with `--htsim-only`
must have this API extension too.

`FI_SENDMSG_EV` in the DGRAM provider's `fi_getinfo` capabilities enables
`fi_sendmsg_ev(ep, &msg, flags)`, where `struct fi_msg_ev` contains an embedded
`struct fi_msg msg` and a host-order `uint16_t ev`. Only htsim advertises this
capability. SUET uses ordinary `fi_sendmsg()` for backends without it, including
UDP. The extension is local to this project, not an upstream libfabric API.

Set `FI_OFI_SUET_SPRAY_PATHS=16` to cycle through 16 entropy values per PDC.
The default is 1 (stable EV); values are clamped to 1..65536. `suet_lb.c/h`
implements the direct-call policy independently of CC, reliability and PDC
ordering. Requests and retransmissions each obtain an EV; ACKs and NACKs echo
the triggering request's EV, including deferred SES responses. There are no
per-packet allocations or additional function-pointer dispatch in this layer.
Spraying has no effect on a backend without `FI_SENDMSG_EV`.

htsim maps EV to `Packet::pathid`, the same ECMP input used by built-in
`UecSrc`. Switch hashing determines the physical route: 16 EVs do not promise
16 distinct paths. The bridge preserves endpoint addresses and SUET packet
bytes. EV is separate simulator metadata, not an extra SUET header or a
change to UDP endpoint ports. On receive, the negotiated
`FI_SUET_DGRAM_RX_METADATA` contract supplies EV in `FI_CQ_FORMAT_DATA`'s data
field when `FI_SUET_DGRAM_EV` is set, including source-discovery CQ errors;
it never uses `FI_REMOTE_CQ_DATA`.

`data_evs` in the result reports distinct request EVs observed, not distinct
physical routes. The integration suite exercises spraying with both ROD/RUD
and GBN/selective repeat under reordering, random/burst loss, trimming and
congested incast. ROD still rejects out-of-order requests, so spraying can
increase its retransmissions; select unordered delivery (`-pdc rud`) when the
application permits it.
