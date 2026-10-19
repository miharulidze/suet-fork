/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include "suet_cc.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static void fill_window(struct suet_cc *cc)
{
	while (suet_cc_can_send(cc))
		suet_cc_track(cc);
}

static void check_fixed(void)
{
	struct suet_cc cc;

	suet_cc_init(&cc, 8, 3000);
	fill_window(&cc);
	assert(cc.in_flight == 8 && !suet_cc_ack_requested(&cc));
	suet_cc_release(&cc, 1);
	suet_cc_ack(&cc, 1, true);
	suet_cc_trim(&cc);
	assert(cc.cwnd == 24000 && cc.in_flight == 7);
	assert(cc.ecn_events == 1 && cc.trim_events == 1);
	assert(suet_cc_can_send(&cc));
	suet_cc_release(&cc, 7);
	suet_cc_ack(&cc, 7, false);
	assert(!cc.in_flight && cc.cwnd == cc.max_cwnd);
}

static void check_dctcp(void)
{
	struct suet_cc cc;
	uint64_t window;

	suet_cc_init(&cc, 8, 3000);
	cc.ecn_enabled = true;
	assert(!suet_cc_ack_requested(&cc));
	fill_window(&cc);
	assert(suet_cc_ack_requested(&cc));
	/* htsim: marked volume / 3, retaining sub-packet precision. */
	suet_cc_release(&cc, 1);
	suet_cc_ack(&cc, 1, true);
	assert(cc.cwnd == 23000 && cc.in_flight == 7);
	assert(!suet_cc_can_send(&cc));
	suet_cc_release(&cc, 1);
	suet_cc_ack(&cc, 1, true);
	assert(cc.cwnd == 22000 && cc.ecn_events == 2);
	assert(suet_cc_can_send(&cc));
	/* No recovery-flight suppression; htsim subtracts each fresh mark. */
	suet_cc_release(&cc, 3);
	suet_cc_ack(&cc, 3, true);
	assert(cc.cwnd == 19000 && cc.in_flight == 3);
	suet_cc_release(&cc, 2);
	suet_cc_ack(&cc, 2, false);
	assert(cc.cwnd == 19000 + 6000 * 3000 / 19000);
	window = cc.cwnd;
	suet_cc_release(&cc, 0);
	suet_cc_ack(&cc, 0, true);
	assert(cc.cwnd == window && cc.ecn_events == 3);
	suet_cc_trim(&cc);
	assert(cc.cwnd == window - 3000 && cc.in_flight == 1);
	suet_cc_trim(&cc);
	assert(cc.cwnd == window - 6000 && cc.trim_events == 2);
	for (int i = 0; i < 20; i++)
		suet_cc_trim(&cc);
	assert(cc.cwnd == 3000 && !suet_cc_can_send(&cc));
	suet_cc_release(&cc, 1);
	suet_cc_ack(&cc, 1, true);
	assert(cc.cwnd == 3000 && !cc.in_flight);
	/* A one-packet window requests an ACK instead of waiting for a batch. */
	suet_cc_track(&cc);
	assert(suet_cc_ack_requested(&cc));
	suet_cc_release(&cc, 1);
	suet_cc_ack(&cc, 1, false);
	assert(cc.cwnd == 6000 && !suet_cc_ack_requested(&cc));
	for (int i = 0; i < 20; i++) {
		fill_window(&cc);
		uint32_t count = cc.in_flight;
		suet_cc_release(&cc, count);
		suet_cc_ack(&cc, count, false);
		assert(cc.cwnd >= cc.mtu && cc.cwnd <= cc.max_cwnd);
	}
	assert(cc.cwnd == cc.max_cwnd);
}

int main(void)
{
	check_fixed();
	check_dctcp();
	puts("Fixed CC and htsim DCTCP: PASS");
}
