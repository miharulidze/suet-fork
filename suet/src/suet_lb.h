/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#ifndef _SUET_LB_H_
#define _SUET_LB_H_

#include <stdint.h>

/* Direct-call load balancing, independent of PDC type, reliability and CC.
 * PDS requests one EV per transmission attempt, including retransmissions.
 * Replies echo the triggering request's EV instead. The caller serializes
 * access. No packet, PSN, wire header or backend state crosses this API.
 */
struct suet_lb {
	uint16_t base_ev;
	uint32_t next;
	uint32_t paths;
};

/* 1..65536 entropy values. One keeps a stable EV; more spray cyclically.
 * The backend hashes EV to a route; paths is not a physical route count.
 */
void suet_lb_init(struct suet_lb *lb, uint16_t seed, uint32_t paths);
uint16_t suet_lb_next_ev(struct suet_lb *lb);

#endif
