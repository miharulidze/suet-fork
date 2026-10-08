/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include "suet_cc.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

int main(void)
{
	struct suet_cc cc;
	suet_cc_init(&cc, 8);
	for (int i = 0; i < 8; i++)
		suet_cc_track(&cc);
	suet_cc_congestion(&cc, false);
	assert(cc.cwnd == 8 && cc.ecn_events == 1);
	cc.ecn_enabled = true;
	suet_cc_congestion(&cc, false);
	assert(cc.cwnd == 4 && !suet_cc_can_send(&cc));
	suet_cc_congestion(&cc, true);
	assert(cc.cwnd == 4 && cc.trim_events == 1);
	suet_cc_ack(&cc, 8);
	assert(!cc.in_flight && !cc.recovery_left);
	for (int round = 0; round < 20; round++) {
		uint32_t count = cc.cwnd;
		for (uint32_t i = 0; i < count; i++)
			suet_cc_track(&cc);
		suet_cc_ack(&cc, count);
		assert(cc.cwnd <= 8 && cc.cwnd >= 1);
	}
	assert(cc.cwnd == 8);
	for (int round = 0; round < 10; round++) {
		suet_cc_track(&cc);
		suet_cc_congestion(&cc, true);
		suet_cc_ack(&cc, 1);
	}
	assert(cc.cwnd == 1);
	puts("Fixed CC and ECN/trimming AIMD: PASS");
}
