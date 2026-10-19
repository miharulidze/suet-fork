/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#ifndef _SUET_CC_H_
#define _SUET_CC_H_

#include <stdbool.h>
#include <stdint.h>

/* Direct-call congestion control, independent of PDC type and reliability.
 * Outstanding credit is in packets; windows are in MTU-normalized bytes
 * to preserve fractional-packet DCTCP adjustments. The caller serializes
 * access; no packet, wire or SES state crosses this interface. Buffer
 * lifetime remains in PDS.
 */
struct suet_cc {
	uint64_t cwnd;
	uint64_t max_cwnd;
	uint32_t mtu;
	uint64_t ecn_events;
	uint64_t trim_events;
	bool ecn_enabled;
	uint32_t in_flight;
};

void suet_cc_init(struct suet_cc *cc, uint32_t packets, uint32_t mtu);
bool suet_cc_can_send(const struct suet_cc *cc);
/* Reserve each transmission attempt, including retransmissions. */
void suet_cc_track(struct suet_cc *cc);
/* PDS releases an attempt once on ACK, loss, or local submission failure. */
void suet_cc_release(struct suet_cc *cc, uint32_t count);
/* Delivery feedback is separate from flight credit: a late ACK may cover
 * packets already removed from flight accounting after loss.
 */
void suet_cc_ack(struct suet_cc *cc, uint32_t count, bool marked);

void suet_cc_trim(struct suet_cc *cc);
/* After tracking a new packet, request feedback if the window is full. */
bool suet_cc_ack_requested(const struct suet_cc *cc);

#endif
