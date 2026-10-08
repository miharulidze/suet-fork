/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include "suet_lb.h"

#include <assert.h>

void suet_lb_init(struct suet_lb *lb, uint16_t seed, uint32_t paths)
{
	assert(paths && paths <= UINT16_MAX + UINT32_C(1));
	lb->base_ev = seed;
	lb->next = 0;
	lb->paths = paths;
}

/* Oblivious cyclic spraying: every EV occurs once per cycle, with no
 * congestion feedback, allocation or dependence on delivery ordering.
 */
uint16_t suet_lb_next_ev(struct suet_lb *lb)
{
	uint16_t ev = (uint16_t) (lb->base_ev + lb->next);

	if (++lb->next == lb->paths)
		lb->next = 0;
	return ev;
}
