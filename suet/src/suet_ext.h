/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#ifndef SUET_EXT_H
#define SUET_EXT_H

#include <stddef.h>
#include <stdint.h>

/* fi_set_val(domain, FI_SUET_CLOCK, &clock), before creating endpoints.
 * The callback returns monotonic elapsed milliseconds. SUET copies this
 * record; the caller owns context until domain close. NULL restores the
 * normal clock. Existing retransmission intervals are unchanged.
 */
#define FI_SUET_CLOCK (-(0x5e7 << 16))
/* fi_close(domain) returns -FI_EAGAIN until PDS teardown has progressed. */
#define FI_SUET_CLOCK_ASYNC_CLOSE (1ULL << 0)
struct fi_suet_clock {
	size_t size;
	uint64_t flags;
	uint64_t (*elapsed_ms)(void *context);
	void *context;
};

/* Optional DGRAM backend contract. fi_get_val(domain,
 * FI_SUET_DGRAM_RX_METADATA, &uint64_mask) opts into these CQ flag bits.
 * Backends derive ECN from the IP CE bits and trimming from configured DSCP;
 * absent support means no metadata, never inference from a short payload.
 * These private CQ flags are consumed by suet_dgram, not exposed to apps.
 */
#define FI_SUET_DGRAM_RX_METADATA     (FI_SUET_CLOCK - 1)
#define FI_SUET_DGRAM_ECN	      (UINT64_C(1) << 60)
#define FI_SUET_DGRAM_TRIMMED	      (UINT64_C(1) << 61)
#define FI_SUET_DGRAM_TRIMMED_LASTHOP (UINT64_C(1) << 62)
#define FI_SUET_DGRAM_METADATA_MASK                  \
	(FI_SUET_DGRAM_ECN | FI_SUET_DGRAM_TRIMMED | \
	 FI_SUET_DGRAM_TRIMMED_LASTHOP)

#endif
