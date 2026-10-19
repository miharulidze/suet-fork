/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Fixed window by default, with optional htsim DCTCP feedback. */
#include "suet_cc.h"

#include <assert.h>
#include <string.h>

void suet_cc_init(struct suet_cc *cc, uint32_t packets, uint32_t mtu)
{
	assert(packets && mtu);
	memset(cc, 0, sizeof(*cc));
	cc->mtu = mtu;
	cc->max_cwnd = (uint64_t) packets * mtu;
	cc->cwnd = cc->max_cwnd;
}

bool suet_cc_can_send(const struct suet_cc *cc)
{
	return ((uint64_t) cc->in_flight + 1) * cc->mtu <= cc->cwnd;
}

void suet_cc_track(struct suet_cc *cc)
{
	assert(suet_cc_can_send(cc));
	cc->in_flight++;
}

void suet_cc_release(struct suet_cc *cc, uint32_t count)
{
	assert(count <= cc->in_flight);
	cc->in_flight -= count;
}

bool suet_cc_ack_requested(const struct suet_cc *cc)
{
	return cc->ecn_enabled && !suet_cc_can_send(cc);
}

static void suet_cc_decrease(struct suet_cc *cc, uint64_t bytes)
{
	cc->cwnd = bytes >= cc->cwnd - cc->mtu ?
			   cc->mtu : cc->cwnd - bytes;
}

/* UecSrc::updateCwndOnAck_DCTCP: increase by acked * MTU / cwnd,
 * or decrease by acked / 3 on CE. This is htsim's per-mark variant,
 * without alpha smoothing or recovery-flight suppression. Each newly
 * acknowledged packet contributes one MTU, matching packet admission.
 */
void suet_cc_ack(struct suet_cc *cc, uint32_t count, bool marked)
{
	uint64_t bytes = (uint64_t) count * cc->mtu;
	uint64_t increase;

	if (!count)
		return;
	if (marked)
		cc->ecn_events++;
	if (!cc->ecn_enabled)
		return;
	if (marked) {
		suet_cc_decrease(cc, bytes / 3);
	} else {
		increase = bytes * cc->mtu / cc->cwnd;
		cc->cwnd += increase < cc->max_cwnd - cc->cwnd ?
				    increase : cc->max_cwnd - cc->cwnd;
	}
}

/* UecSrc::updateCwndOnNack_DCTCP: subtract the trimmed packet's
 * volume, bounded below by one MTU. Reliability coalesces duplicate NACKs.
 */
void suet_cc_trim(struct suet_cc *cc)
{
	cc->trim_events++;
	if (cc->ecn_enabled)
		suet_cc_decrease(cc, cc->mtu);
}
