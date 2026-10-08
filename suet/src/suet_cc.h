/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#ifndef _SUET_CC_H_
#define _SUET_CC_H_

#include <stdbool.h>
#include <stdint.h>

/* Direct-call congestion control, independent of PDC type and reliability.
 * Units are packets. The caller serializes access; no packet, wire or SES
 * state crosses this interface. Buffer lifetime remains in PDS.
 */
struct suet_cc {
	uint32_t cwnd;
	uint32_t max_cwnd;
	uint32_t recovery_left;
	uint32_t increase_credit;
	uint64_t ecn_events;
	uint64_t trim_events;
	bool ecn_enabled;
	uint32_t in_flight;
};

void suet_cc_init(struct suet_cc *cc, uint32_t cwnd);
bool suet_cc_can_send(const struct suet_cc *cc);
/* Reserve once per new tracked request, including a failed local submission.
 * Retransmissions reuse that reservation; local completion does not release it.
 */
void suet_cc_track(struct suet_cc *cc);
/* Only newly acknowledged requests release credit. PDS validates feedback
 * through reliability before reporting it here.
 */
void suet_cc_ack(struct suet_cc *cc, uint32_t count);

void suet_cc_congestion(struct suet_cc *cc, bool trimmed);

#endif
