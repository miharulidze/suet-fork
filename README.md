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

## Provider architecture

### Portable design
The SUET provider is a major rework of the RxD libfabric provider. Similarly to RxD, SUET enables connectionless reliable messaging endpoint semantics over connectionless unreliable endpoints (`FI_EP_RDM` over `FI_EP_DGRAM`), while also adding zero-copy send path and batched CQ polling optimizations. A core `FI_EP_DGRAM` provider serves as a portable abstraction layer for the wire protocol that encapsulates PDS/SES traffic. Thanks to that, SUET runs on Linux (UDP and Verbs) and macOS (UDP).

### Key provider features

- Basic UET SES request/response flow
- Guaranteed delivery UET SES requests
- UET Reliable Ordered Delivery (ROD) PDC
- Zero-RTT PDC establishment
- Zero-copy send path, per-packet memcpy at the receiver
- Unexpected message buffering
- Go-Back-N reliability

Many of the UET specification features are not yet supported, including but not limited to:
- RUD PDC, SR-based reliability and endpoint congestion control
- JobID and PIDonFEP validation
- TSS layer

### Test coverage:
- SUET tested with libfabric fabtests, OpenMPI and MPICH
- Basic header formats tested against Wireshark UET dissector

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