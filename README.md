# SUET: Software Ultra Ethernet Transport provider

SUET is a libfabric provider that implements wire-compatible UET PDS (Packet Delivery Sublayer) and SES (Semantic Sublayer) protocols in software. SUET can run on top of any datagram transport available in upstream libfabric, such as Verbs and UDP. 

SUET is developed at the Scalable Parallel Computing Lab (SPCL) at ETH Zurich for Ultra Ethernet and general AI and HPC networking protocol research.

### 1024 byte message ping pong traced with Wireshark UET dissector
![Wireshark](./ping-pong-wireshark.png "1024 Byte Ping Pong with Wireshark UET dissector")

## Quick Start

```bash
./build.sh
./build.sh --test
```

See `INSTALL.md` for more details.

To build and test SUET over the htsim network model:

```bash
./build.sh --htsim
./build.sh --test-htsim
```

The provider, runner, tests and build integration are in [htsim/](htsim/README.md).
The simulator dependency is fetched at a pinned revision, or supplied through
`HTSIM_SOURCE_DIR`. Use `--htsim-only` to reuse an existing SUET/libfabric install.

## Provider architecture

### Portable design
The SUET provider is a major rework of the RxD libfabric provider. Similarly to RxD, SUET enables connectionless reliable messaging endpoint semantics over connectionless unreliable endpoints (`FI_EP_RDM` over `FI_EP_DGRAM`), while also adding zero-copy send path and batched CQ polling optimizations. A core `FI_EP_DGRAM` provider serves as a portable abstraction layer for the wire protocol that encapsulates PDS/SES traffic. Thanks to that, SUET runs on Linux (UDP and Verbs) and macOS (UDP).

### SES/PDS boundary

The SES/PDS and PDS/datagram API headers define direct calls between layers.
There is no operations table or new function-pointer dispatch.

- SES owns operation buffers, message segmentation, endpoint matching, RMA
  validation, atomic execution, unexpected messages, and application completions.
- PDS owns PDCs, PSNs, transport queues, wire ACK/NACK handling, and
  response retention. Reliability manages windows and retry decisions through
  `suet_rel.h`. PDS private types live in `suet_pds.h`; SES-only types live in
  `suet_ses.h`. Helper implementations stay in the corresponding `.c` files.
  The SES/PDS contract lives in `suet_ses_pds_api.h`.
  `suet.h` includes the three layer headers and embeds their resource structures
  directly in the domain; no separate SES/PDS resource allocation is needed.
- Datagram owns provider setup, packet pools and registration, send/receive
  posting, CQ polling, and provider prefixes in `suet_dgram.c`. Its resources
  and lifecycle are declared in `suet_dgram.h`; the packet/event contract with
  PDS lives in `suet_pds_dgram_api.h`. Provider contexts stay private to datagram.
  PDS sees wire lengths excluding provider prefixes. Local send
  completion releases provider access; PDS independently tracks acknowledgment
  and retry retention. Each packet still occupies one pool allocation, with
  separate aligned datagram, PDS, and SES records. Datagram keeps provider-busy
  state and its posted-receive node private; PDS owns PDC identity, packet
  retirement state, and its queue node; SES owns its unexpected-message packet node.
  The shared packet view carries storage/I/O information and opaque layer
  contexts, without shared flags or list membership.
- Each transmission has a separate PDS record with an opaque SES context. PDS
  supplies a semantic-header region and asks SES for header bytes plus payload
  IOVs and registration descriptors. PDS assembles the wire packet and reports
  terminal completion through direct calls. Header preparation writes directly
  into the existing packet allocation; it adds no header or payload copy.
  SES uses the semantic-byte budget exposed by PDS to derive payload limits,
  including space for atomic extensions. SES never calculates PDS header sizes,
  traverses PDC queues, or assigns packet sequence numbers.
- Ordered receive state lives in an opaque SES context associated with the
  transport stream. PDS strips its framing; SES validates and separates its
  header/extensions from the payload. Ordered dispatch receives that view and
  an opaque release handle. Retained packets keep a copy of the view in their
  existing SES record, including for peek/claim/discard. PDS persists any
  required response before committing the SES receive completion.
- Unexpected messages hold opaque response reservations. Completing or
  cancelling a reservation goes through PDS; SES does not mutate retained
  response lists. Closing a PDC invalidates the route while outstanding SES
  reservations remain safe to release.

The boundary shares semantic views, opaque lifetime handles, and response metadata. The existing
wire headers, Go-Back-N algorithm, per-domain locking, and datagram/zero-copy
buffer lifetime rules are unchanged. Calls require the domain FEP lock or
exclusive initialization/close. Domain close drains PDS, stops the datagram
endpoint, then releases layer state and packet storage. No new dispatch table,
reliability algorithm, or progress thread is introduced.

### Reliability boundary

`suet_rel.h` defines a direct-call contract; `suet_rel.c` implements Go-Back-N.
The implementation takes PSNs, decoded feedback and monotonic millisecond
timestamps, with no PDC types, wire headers, packet pointers or SES dependency.
PDC assigns PSNs and owns the packet-pointer arrays; reliability owns circular
bitmap windows and retry state. Bitmap slots and pointer slots share the same
index through `suet_rel_slot()`. ACK processing returns the retired slot range;
PDS detaches those pointers before progressing new transmissions. Packets still
borrowed by datagram remain on PDS's lifetime list until local completion.

ROD delivery ordering stays in PDS and discards packets beyond a gap without
retaining packet buffers. RUD dispatches admitted packets directly to SES.
TX messages track their own pending packets: RUD can complete a later message
once all its packets have explicit or cumulative ACKs and datagram has released
local access. ROD also
waits for the cumulative packet-retirement frontier. SACK alone records
receipt, without completing a message; acknowledged packet records may remain
in the reliability window after RUD releases the message.
The GBN implementation discards future PSNs independently of the PDC type;
selective repeat admits them and leaves delivery policy to PDS.
Replacing the reliability implementation does not require a function-pointer
table or per-packet allocation. Message completion, semantic response replay
and connection lifecycle remain in PDS.

The default window remains 128 packets. GBN retains cumulative ACKs, the
existing ACK cadence, timeout-driven recovery, and per-packet attempt
timestamps and retry limits. Trim NACKs additionally make retries eligible
immediately; CC still gates their transmission. Backoff starts at 1 ms and
saturates safely at 4 seconds. Local send failures remain tracked for recovery;
EAGAIN does not consume a retransmission attempt. Legacy request/ACK layouts
are unchanged. `max_unacked` is
clamped to 1..65535; negative retry limits become
zero. An ACK covering an untracked PSN is rejected without retiring packets.

### Congestion-control boundary

`suet_cc.h` defines direct calls for send admission, tracking and releasing
flight credit, and delivery/congestion feedback. `suet_cc.c` implements a fixed
window by default, with optional htsim DCTCP. The initial/maximum window is
`FI_OFI_SUET_MAX_UNACKED` (default 128 packets). Each
initiator PDC embeds its CC context; there are no allocations or function
pointers, and CC has no PDC-type, reliability, wire, or SES dependency.

PDS checks CC credit, reliability-window space and packet-buffer availability
before sending new data. Each transmission attempt, including a retry,
reserves one CC credit. ACK, declared loss, or local submission failure
releases that attempt's credit exactly once. Reliability retains unacknowledged
packets independently of flight accounting. Queued retries take priority over
new messages and are sent in PSN order when CC permits them.
Acknowledged buffers may remain borrowed by datagram without consuming flight
credit. A late ACK can cancel a queued retry without releasing credit twice.
Normal ACK processing admits queued retries; timeout discovery runs after the
datagram receive batch, avoiding premature retries while ACKs are being drained.

Tracked Close requests use the same credit accounting; ACK/NACK and CLEAR
responses do not. Neither policy requires a new wire layout or pacing timer.

### Simulation clock

`suet/src/suet_ext.h` exposes an optional per-domain clock through
`fi_set_val(&domain->fid, FI_SUET_CLOCK, &clock)`. Install it before creating
endpoints. The callback returns monotonic elapsed milliseconds; its context
must remain valid until the domain closes. The record is copied by SUET.
A null callback restores the normal clock. Retry intervals and wire formats
are unchanged.

A discrete-event driver can also set `FI_SUET_CLOCK_ASYNC_CLOSE`. In this mode,
`fi_close(domain)` returns `-FI_EAGAIN` while PDS teardown needs further
progress. The driver must keep advancing its network and retry closing all
participating domains. Without this flag, the existing blocking close behavior
is preserved. This supports the [htsim_suet runner](htsim/README.md) without simulator
dependencies in the SUET provider.

### Key provider features

- Basic UET SES request/response flow
- Guaranteed delivery UET SES requests
- UET Reliable Ordered Delivery (ROD) PDC
- Zero-RTT PDC establishment
- Zero-copy send path, per-packet memcpy at the receiver
- Unexpected message buffering
- Go-Back-N reliability (always enabled; `FI_SUET_RETRY` is no longer supported)

Many of the UET specification features are not yet supported, including but not limited to:
- RUD PDC, SR-based reliability and endpoint congestion control
- JobID validation and complete PIDonFEP isolation
- TSS layer

### Test coverage:
- SUET tested with libfabric fabtests, OpenMPI and MPICH
- Basic header formats tested against Wireshark UET dissector
- SES/PDS regression and mixed-build compatibility checks: see [tests/README.md](tests/README.md)

### NCCL support experiment

Follow the [NCCL test guide](tests/nccl/TESTING.md) for reproducible builds,
the checked 2-/4-GPU matrix, and library/transport verification.

The 2026-10-14 CSCS Ault retry successfully built unmodified NCCL, AWS OFI
NCCL and SUET `refactor` on the approved replacement node `ault24`, using
scratch-installed Conda GCC 13.4/CUDA 12.8 and approved libfabric base
`94f3b1ac`. The existing SUET patches apply to that revision unchanged.

The baseline failed at runtime: incorrect values with 2 GPUs and an OFI
connection-management error with 4 GPUs. The authorized endpoint-addressing
fix now passes both GPU counts from 8 bytes through 1 MiB, with zero
validation errors. These ROD tests force the Libfabric network path with
GPU P2P/SHM and GDR disabled. The OFI plugin source is unchanged and was
rebuilt against the corrected SUET/libfabric install.

The [NCCL experiment index](tests/nccl/README.md) preserves the initial
build failure. The [retry report](tests/nccl/retry-2026-10-14.md) records
toolchain installation, exact commands, raw logs, the endpoint-identity
reproduction, and the proposed fix. The [implementation and validation
report](tests/nccl/endpoint-fix-2026-10-14.md) records the applied fix, passing
GPU logs, and the plugin RPATH issue encountered during validation. A new
four-endpoint UDP regression also passes with both ROD and RUD; the NCCL
GPU runs exercise ROD.

### libfabric API mapping and feature support

SUET provider employs the following mapping between libfabric and UET abstractions:
- SUET fabric represents all UET Fabric Endpoints (FEPs) within a given physical NIC port. FEPs are exposed as DGRAM provider endpoints.
- SUET domain represents a single FEP. It is associated with a DGRAM provider domain-endpoint pair, through which traffic of all SUET endpoints is multiplexed.
- SUET endpoint represents a single UET Resource Index (RI) and exposes data transfer operations with UET opcodes to the user.

SUET provider supports the following operations with `FI_EP_RDM`:
- `fi_(t)send`, `fi_(t)sendv`, `fi_(t)sendmsg`, `fi_(t)inject`, `fi_(t)senddata`, `fi_(t)injectdata`
- `fi_(t)recv*`, `fi_(t)recvmsg`, FI_MULTI_RECV, FI_PEEK/FI_CLAIM/FI_DISCARD
- `fi_write`, `fi_writev`, `fi_writemsg`, `fi_inject_write`, `fi_writedata`, `fi_inject_writedata`
- non-fetching `fi_atomic`, `fi_atomicv`, `fi_atomicmsg`, `fi_inject_atomic`

## Performance

When running on top of Verbs datagram provider, SUET reaches ~7.5 GB/s/core on message sizes above 384 KiB on our Intel Xeon Gold 6140 CPU 2.30GHz with 100 Gbit/s IB EDR. Linear bandwidth scaling is achieved through SUET domain parallelism. Each thread manages its own domain(s) and their endpoint(s). For the best performance, endpoints on the same domain are expected to be served by the same thread to avoid lock contention. For example, in the context of AI training traffic, parallel rings can be mapped to independent SUET threads.

## License

SUET is available under BSD or GPLv2 licenses, similar to the upstream libfabric. For more details see: [https://github.com/ofiwg/libfabric/blob/main/COPYING](https://github.com/ofiwg/libfabric/blob/main/COPYING)

Selective repeat can be selected independently of endpoint ordering with
`FI_OFI_SUET_SELECTIVE_REPEAT=1` (Go-Back-N remains the default). ROD drops
out-of-order requests without buffering; RUD processes admitted fragments by
semantic message ID. The first arriving fragment establishes receive state,
including when it precedes SOM. Both algorithms preserve local datagram buffer
lifetime. RUD TX completion is per message; ROD retains cumulative completion ordering.

Selective repeat emits the specification's 32-byte ACK_CC header with a
64-bit SACK bitmap, an eight-PSN-aligned base, signed offsets, and MPR. ACK_CC
and ACK_CCX are accepted by both algorithms. Positive SACK evidence is
monotonic and does not imply a default SES response or message completion.
The selective-repeat window is bounded to 128–32640 packets to fit MPR and
signed ACK offsets. The existing guaranteed-response/CLEAR handling is
retained; this is not a claim of full UET protocol conformance.

ECN and trimming use optional receive metadata from the datagram backend.
The htsim backend negotiates `FI_SUET_DGRAM_RX_METADATA` and reports CE,
trimmed, and last-hop-trimmed flags in receive completions. Backends without
this extension keep their existing behavior. A trimmed request produces UET
NACK code 1 or 2 without reaching SES or advancing the receive bitmap, even
when the SYN request was trimmed. Valid trim NACKs schedule the missing PSN
for retry; duplicates coalesce and SACKed packets are not retransmitted by either algorithm.
Other NACKs retain timeout recovery to avoid OOO feedback retry storms.
Retry exhaustion is tracked per packet rather than across unrelated retries.

`FI_OFI_SUET_ECN=1` enables the DCTCP variant from htsim's
`UecSrc::updateCwndOnAck_DCTCP` / `updateCwndOnNack_DCTCP` in `suet_cc`.
Unmarked ACKs increase the window by `acked * MTU / cwnd`; marked ACKs
subtract `acked / 3`; a fresh trim NACK subtracts one MTU. This is htsim's
per-mark variant, without standard DCTCP alpha smoothing or recovery-flight
suppression. Windows use bytes to retain fractional-packet adjustments;
each transmission attempt reserves one MTU, including short packets.
Loss or local submission failure releases that attempt's flight credit while
reliability retains the packet. Retries are queued in PSN order, take priority
over new data, and must reacquire congestion-window credit before sending.
Late ACKs cancel queued retries without releasing credit twice.
The minimum is one packet and the maximum is `FI_OFI_SUET_MAX_UNACKED`.
The sender sets AR when the adaptive congestion window fills, so windows
smaller than the receiver ACK batch continue to make progress.
The default remains a fixed window. ACKs echo CE using the UET M flag;
duplicate ACKs cannot release credit or adjust the window twice, and
reliability coalesces duplicate trim NACKs. This is not UET NSCC. Native UDP
currently does not supply the optional network metadata.
