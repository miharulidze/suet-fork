/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Fixed congestion window; ACKs release credit without changing cwnd. */
#include "suet_cc.h"

#include <assert.h>

void suet_cc_init(struct suet_cc *cc, uint32_t cwnd)
{
	assert(cwnd);
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
}
