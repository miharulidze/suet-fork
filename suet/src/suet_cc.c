/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Fixed congestion window by default, with optional bounded ECN response. */
#include "suet_cc.h"

#include <assert.h>
#include <string.h>

void suet_cc_init(struct suet_cc *cc, uint32_t cwnd)
{
	assert(cwnd);
	memset(cc, 0, sizeof(*cc));
	cc->max_cwnd = cwnd;
	cc->cwnd = cwnd;
	cc->in_flight = 0;
}

bool suet_cc_can_send(const struct suet_cc *cc)
{
	return cc->in_flight < cc->cwnd;
}

void suet_cc_track(struct suet_cc *cc)
{
	assert(suet_cc_can_send(cc));
	cc->in_flight++;
}

void suet_cc_ack(struct suet_cc *cc, uint32_t count)
{
	assert(count <= cc->in_flight);
	cc->in_flight -= count;
	if (!cc->ecn_enabled)
		return;
	if (cc->recovery_left) {
		cc->recovery_left = count < cc->recovery_left ?
					    cc->recovery_left - count :
					    0;
		return;
	}
	if (cc->cwnd == cc->max_cwnd) {
		cc->increase_credit = 0;
		return;
	}
	cc->increase_credit += count;
	if (cc->increase_credit >= cc->cwnd && cc->cwnd < cc->max_cwnd) {
		cc->increase_credit -= cc->cwnd;
		cc->cwnd++;
	}
}

/* Optional bounded AIMD; reliability and delivery type do not enter CC.
 * A recovery budget suppresses repeated reductions within one flight.
 */
void suet_cc_congestion(struct suet_cc *cc, bool trimmed)
{
	if (trimmed)
		cc->trim_events++;
	else
		cc->ecn_events++;
	if (!cc->ecn_enabled || cc->recovery_left)
		return;
	cc->recovery_left = cc->in_flight ? cc->in_flight : 1;
	cc->cwnd = cc->cwnd > 1 ? cc->cwnd / 2 : 1;
	cc->increase_credit = 0;
}
