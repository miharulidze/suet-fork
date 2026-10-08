/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include "suet_lb.h"

#undef NDEBUG
#include <assert.h>
#include <stdio.h>

static void check_cycle(uint16_t seed, uint32_t paths)
{
	struct suet_lb lb, other;
	unsigned char seen[65536] = {0};
	uint32_t i;

	suet_lb_init(&lb, seed, paths);
	suet_lb_init(&other, seed, paths);
	for (i = 0; i < paths; i++) {
		uint16_t ev = suet_lb_next_ev(&lb);
		assert(!seen[ev]);
		seen[ev] = 1;
		assert(ev == suet_lb_next_ev(&other));
	}
	assert(suet_lb_next_ev(&lb) == seed);
	/* Interleaving an independent PDC cannot change this sequence. */
	for (i = 0; i < paths; i++)
		suet_lb_next_ev(&other);
	assert(suet_lb_next_ev(&lb) == (uint16_t) (seed + (paths > 1)));
}

int main(void)
{
	struct suet_lb lb;
	uint32_t i;

	suet_lb_init(&lb, 65535, 1);
	for (i = 0; i < 100000; i++)
		assert(suet_lb_next_ev(&lb) == 65535);
	check_cycle(0, 1);
	check_cycle(65535, 2);
	check_cycle(65530, 7);
	check_cycle(12345, 256);
	check_cycle(42, 65536);
	puts("Load balancing: stable EV, cyclic spraying, wraparound and "
	     "independent contexts PASS");
	return 0;
}
