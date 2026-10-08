/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include <config.h>
#if !ENABLE_DEBUG && !defined(NDEBUG)
#define NDEBUG
#endif
#include "suet.h"
#undef NDEBUG
#include "../suet/src/suet_domain.c"
#include <assert.h>

static uint64_t elapsed(void *context)
{
	return *(uint64_t *) context;
}

int main(void)
{
	struct suet_domain domain = {0};
	uint64_t now = 100, system_before = ofi_gettime_ms();
	struct fi_suet_clock clock = {sizeof(clock), FI_SUET_CLOCK_ASYNC_CLOSE,
				      elapsed, &now};
	struct fi_fid_var var = {FI_SUET_CLOCK, &clock};
	assert(!ofi_genlock_init(&domain.fep_lock, OFI_LOCK_MUTEX));
	assert(suet_domain_now_ms(&domain) >= system_before);
	assert(!suet_domain_control(&domain.util_domain.domain_fid.fid,
				    FI_SET_VAL, &var));
	assert(suet_domain_now_ms(&domain) == 100);
	now = 104;
	assert(suet_domain_now_ms(&domain) == 104);
	clock.size--;
	assert(suet_domain_control(&domain.util_domain.domain_fid.fid,
				   FI_SET_VAL, &var) == -FI_EINVAL);
	clock.size++;
	clock.flags = 1ULL << 63;
	assert(suet_domain_control(&domain.util_domain.domain_fid.fid,
				   FI_SET_VAL, &var) == -FI_EINVAL);
	clock.flags = 0;
	clock.elapsed_ms = NULL;
	domain.next_ri = 1;
	assert(suet_domain_control(&domain.util_domain.domain_fid.fid,
				   FI_SET_VAL, &var) == -FI_EBUSY);
	assert(suet_domain_now_ms(&domain) == 104);
	domain.next_ri = 0;
	assert(!suet_domain_control(&domain.util_domain.domain_fid.fid,
				    FI_SET_VAL, &var));
	assert(suet_domain_now_ms(&domain) >= system_before);
	dlist_init(&domain.pds.active_ipdc_list);
	dlist_init(&domain.pds.active_tpdc_list);
	assert(!suet_pds_drain(&domain, false));
	ofi_genlock_destroy(&domain.fep_lock);
	puts("Domain clock installation, lifetime and fallback: PASS");
	return 0;
}
