# SES/PDS regression checks

The normal suite retains all 33 original cases and adds zero-length messaging
and verified, segmented unexpected messages (65,537 bytes):

```sh
python3 test_suet.py --bin-dir /path/to/fabtests/bin -v
```

No test iterations or timeout limits were reduced. Output is captured in
temporary files so a verbose server cannot fill its output pipe while the
runner waits for the client. Timeouts include partial output.

## Packet preparation

`test_ses_pds.c` checks the direct SES/PDS contract: first, middle, and final
segments; repeated/out-of-order preparation without mutating the TX entry;
scatter/gather and zero-copy payload descriptions; arbitrary header headroom;
header/IOV capacity checks; empty messages; and the atomic header extension.
Receive checks cover truncated headers/extensions/atomic operands and delivery
from noncontiguous semantic-header/payload views. Size checks verify that
atomic payload limits reserve space for all semantic headers. It also checks independent SES
domain lifetimes, receive allocation after another domain closes, repeated
cleanup, and reinitialization. Response-retention checks cover pool exhaustion,
rejected unexpected intake, immutable response inputs, replay-range preservation
across semantic response replacement, and capacity recovery after release.
It runs without network access.

From the SUET repository, with a configured libfabric source/build tree that
includes this provider and its installed static library:

```sh
LIBFABRIC=/path/to/libfabric
PREFIX=/path/to/install
cc -g -Wall -DHAVE_CONFIG_H -D_GNU_SOURCE \
  -I"$LIBFABRIC" -I"$LIBFABRIC/include" -I"$LIBFABRIC/include/osx" \
  -Isuet/src tests/test_ses_pds.c "$PREFIX/lib/libfabric.a" \
  -lpthread -ldl -o /tmp/test_ses_pds
/tmp/test_ses_pds
```

Use the platform's internal include directory on other systems and any extra
static dependencies required by its enabled libfabric providers. Compile the
test with assertions enabled (without `-DNDEBUG`). The test matches the
configured libfabric debug/release header layout before enabling its own
assertions.

## Datagram and packet ownership

`test_pds_dgram.c` checks provider prefixes, repeated zero-copy sends, immediate
send failures, local success/error completions, ACK-before-completion retention,
RX ownership transfer, and shutdown. It also checks context alignment and that
retaining two unexpected-message segments in SES leaves PDS state and the
datagram list node untouched. The real PDS assembler is exercised with copied,
zero-copy, segmented, empty, and maximum-size atomic payloads, checking emitted
headers and payload bytes after repeated sends with a provider prefix. Run it with the same compiler and library settings
as above, replacing `test_ses_pds` with `test_pds_dgram` in both paths.

## Datagram resource ownership

`test_dgram_resources.c` exercises the real AV and MR layers with a small
provider double. It checks stable domain peer IDs across public AV removal,
reinsertion and multiple AVs; concurrent AV updates; a valid backend address
of zero; partial insertion and synchronous errors; RX source discovery; and
balanced backend AV lifetimes. MR checks cover requested keys, generated-key
collisions, bounded IOV registration, opaque descriptors, partial-failure
rollback and successful registration after failure. Build and run it using
the same command above, replacing `test_ses_pds` with `test_dgram_resources`.
No network access is needed.

## Go-Back-N reliability

`test_rel.c` links only `suet_rel.c`, without libfabric or PDC/SES definitions.
It checks circular slots across PSN wraparound with power-of-two and arbitrary
window sizes, cumulative ACK validation (including unsent holes), repeated
retirement/reuse, duplicate feedback, GBN retry rounds, backoff through retry
exhaustion, receive acceptance/cancellation, and ACK cadence. Run it directly:

```sh
cc -g -Wall -Wextra -Werror -fsanitize=address,undefined -Isuet/src \
  tests/test_rel.c suet/src/suet_rel.c -o /tmp/test_rel
/tmp/test_rel
```

The htsim CMake build also includes this test. `test_pds_dgram.c` additionally
injects real PDS ACKs across PSN wraparound while datagram still owns the TX
buffers. It checks pointer-slot retirement, invalid/duplicate ACKs and local
completions in a different order. The existing ownership and framing checks
remain intact.

The same PDS test covers fixed-window CC admission independently of bitmap and
buffer capacity, credit release through real cumulative ACKs, duplicate/invalid
ACK rejection, and slot reuse while an ACKed buffer awaits local completion.
Local resubmission and completion do not consume or release additional
congestion credit.

## External simulation clock

`test_clock.c` checks per-domain callback installation, elapsed-time reads,
invalid extension arguments, rejection after endpoint creation and restoration
of the normal clock. Build with the same command above, replacing
`test_ses_pds` with `test_clock`. The htsim integration additionally checks loss
recovery using simulated time and nonblocking PDS shutdown.

## Compatibility between builds

On macOS, use the same fabtests binaries with a different libfabric library
for each process. Both builds must use the same libfabric ABI. Swap the two
paths to check the opposite direction:

```sh
python3 test_suet.py --bin-dir /path/to/fabtests/bin -v \
  --server-env DYLD_LIBRARY_PATH=/path/to/original/lib \
  --client-env DYLD_LIBRARY_PATH=/path/to/refactored/lib
```

On Linux, use `LD_LIBRARY_PATH`. Environment overrides are repeatable and can
also supply `FI_SUET_*` settings without changing the other process.

## macOS validation environment

The refactor was validated on macOS over `udp;ofi_suet` on loopback. The
original provider was SUET commit `e3e21c5`. Both builds used libfabric
`7a494bde871a927a8fc7a917f1556eb01a089d39`, the provider registration patch, and
the two dependency patches documented by the repository build:

- `304fe27a299042685c029d03d692c54abeb44409` (UDP source error handling)
- `5ea1ba6ed6d56973b516cc24d3489d1c703b500a` (Verbs source error handling)

Configure used `--enable-suet --enable-udp CFLAGS='-O2 -g'`. Explicit CFLAGS
avoided an upstream macOS dynamic-loader failure involving `_coll_av_open`
with the default visibility flags. This was a validation build setting; no
upstream sources were changed for that workaround.

Results for the final refactor in that environment:

- All 33 original fabtests and both added integration cases passed (35 total).
- Five interoperability cases passed in each direction (ten total): empty
  messages, verified segmented unexpected messages, tagged peek/claim/discard,
  1 MiB RMA writes, and all supported atomic operations with delivery completion.
- All five standalone test programs passed, including with address/undefined-behavior
  sanitizers and assertions enabled in the directly compiled layer implementations.
- All three htsim CTests passed. A 32-flow, 64 MB topology run also verified
  every message while recovering from 4,095 network drops.
- Every provider source compiled without warnings. Changes in `suet_proto.h`
  rename C packet types and datagram address fields; wire layouts are unchanged.

The cumulative ACK tests additionally exercise every missing-bit position at
every physical head in 65- and 129-slot windows. Invalid ACKs must preserve
all submitted slots; full retirement must work across word and PSN wrap.
ROD has no out-of-order packet buffer. Endpoint ordering selects ROD when
ordering is requested and RUD otherwise; PDC lookup separates the two modes.
