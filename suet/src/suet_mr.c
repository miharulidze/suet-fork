/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2026 ETH Zurich. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "suet.h"

/*
 * Memory registration for SUET. Follows mostly the RxM pattern: user-facing
 * fi_mr_* calls register a real MR on the underlying DGRAM domain
 * (fep_domain) and wrap the result in a struct suet_mr. SUET emulates
 * RMA/atomics in software on the target side, so the MR is also tracked
 * in util_domain.mr_map unconditionally for address/key verification.
 *
 * The same suet_mr_reg_impl() helper is shared by the user-facing MR
 * ops and by TX-time on-demand registration (suet_mr_regv_internal) when the
 * user's send exceeds mr_reg_threshold, so both paths go through
 * identical code.
 */

static int suet_mr_map_insert(struct suet_domain *domain,
			      struct fi_mr_attr *user_attr,
			      struct suet_mr *suet_mr, uint64_t flags)
{
	uint64_t key;
	int ret;

	ofi_genlock_lock(&domain->util_domain.lock);
	ret = ofi_mr_map_insert(&domain->util_domain.mr_map, user_attr, &key,
				suet_mr, flags);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_DOMAIN, "mr_map insert failed: %d\n",
			ret);
	} else {
		/* In FI_MR_PROV_KEY mode the map assigns its own key; reflect
		 * the effective key back to the user via mr_fid.key so that
		 * fi_mr_key() returns what the remote peer needs. */
		suet_mr->mr_fid.key = key;
	}
	ofi_genlock_unlock(&domain->util_domain.lock);
	return ret;
}

static void suet_mr_map_remove(struct suet_mr *suet_mr)
{
	if (suet_mr->mr_fid.key == FI_KEY_NOTAVAIL)
		return;

	ofi_genlock_lock(&suet_mr->domain->util_domain.lock);
	(void) ofi_mr_map_remove(&suet_mr->domain->util_domain.mr_map,
				 suet_mr->mr_fid.key);
	ofi_genlock_unlock(&suet_mr->domain->util_domain.lock);
}

static int suet_mr_close(fid_t fid)
{
	struct suet_mr *suet_mr;
	int ret;

	suet_mr = container_of(fid, struct suet_mr, mr_fid.fid);

	suet_mr_map_remove(suet_mr);

	ret = fi_close(&suet_mr->dgram_mr->fid);
	if (ret)
		FI_WARN(&suet_prov, FI_LOG_DOMAIN,
			"fi_close(dgram mr) failed: %d\n", ret);

	ofi_mutex_destroy(&suet_mr->amo_lock);
	ofi_atomic_dec32(&suet_mr->domain->util_domain.ref);
	free(suet_mr);
	return ret;
}

static struct fi_ops suet_mr_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = suet_mr_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

/*
 * Calls fi_mr_regattr() on the underlying DGRAM domain.
 * When gen_key is true the function ignores the caller's
 * attr->requested_key and draws from the domain-internal namespace
 * (bit 31 set), retrying on collisions.
 */
static int suet_mr_reg_impl(struct suet_domain *domain, struct fi_mr_attr *attr,
			    uint64_t flags, bool gen_key, struct fid_mr **mr)
{
	int ret, tries = 0;

	if (!gen_key)
		return fi_mr_regattr(domain->dgram.domain, attr, flags, mr);

	/* The counter may collide with a user-chosen requested_key already
	 * registered on the datagram domain, so advance and retry on
	 * -FI_ENOKEY. */
	do {
		attr->requested_key = domain->dgram.mr_key++ | (1UL << 31);
		ret = fi_mr_regattr(domain->dgram.domain, attr, flags, mr);
	} while (ret == -FI_ENOKEY && tries++ < SUET_MR_KEY_MAX_RETRIES);

	return ret;
}

void suet_mr_closev_internal(struct fid_mr **mr, size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		if (!mr[i])
			continue;
		ret = fi_close(&mr[i]->fid);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_EP_DATA,
				"fi_close(dgram mr[%zu]) failed: %d\n", i, ret);
		mr[i] = NULL;
	}
}

/*
 * Register an IOV on the underlying fep_domain for a single send,
 * capped at reg_limit total bytes. Used by the TX path when the send
 * length exceeds suet_env.mr_reg_threshold.
 */
int suet_mr_regv_internal(struct suet_domain *domain, const struct iovec *iov,
			  size_t count, size_t reg_limit, uint64_t access,
			  struct fid_mr **mr)
{
	struct iovec cur_buf_slice;
	struct fi_mr_attr attr = {
		.mr_iov = &cur_buf_slice,
		.iov_count = 1,
		.access = access,
	};
	size_t i, len;
	int ret;

	for (i = 0; i < count && reg_limit; i++) {
		len = MIN(iov[i].iov_len, reg_limit);
		cur_buf_slice.iov_base = iov[i].iov_base;
		cur_buf_slice.iov_len = len;
		ret = suet_mr_reg_impl(domain, &attr, 0, true, &mr[i]);
		if (ret)
			goto err;
		reg_limit -= len;
	}
	return 0;
err:
	suet_mr_closev_internal(mr, i);
	return ret;
}

static void suet_mr_init(struct suet_mr *suet_mr, struct suet_domain *domain,
			 void *context)
{
	suet_mr->mr_fid.fid.fclass = FI_CLASS_MR;
	suet_mr->mr_fid.fid.context = context;
	suet_mr->mr_fid.fid.ops = &suet_mr_fi_ops;
	suet_mr->mr_fid.mem_desc = suet_mr;
	suet_mr->mr_fid.key = FI_KEY_NOTAVAIL;
	suet_mr->domain = domain;
	ofi_mutex_init(&suet_mr->amo_lock);
	ofi_atomic_inc32(&domain->util_domain.ref);
}

static int suet_mr_regattr(struct fid *fid, const struct fi_mr_attr *attr,
			   uint64_t flags, struct fid_mr **mr)
{
	struct suet_domain *domain;
	struct fi_mr_attr dgram_attr = *attr;
	struct suet_mr *suet_mr;
	int ret;

	domain = container_of(fid, struct suet_domain,
			      util_domain.domain_fid.fid);

	suet_mr = calloc(1, sizeof(*suet_mr));
	if (!suet_mr)
		return -FI_ENOMEM;

	ofi_mr_update_attr(domain->util_domain.fabric->fabric_fid.api_version,
			   domain->util_domain.info_domain_caps, attr,
			   &dgram_attr, flags);

	ret = suet_mr_reg_impl(domain, &dgram_attr, flags, false,
			       &suet_mr->dgram_mr);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_DOMAIN,
			"fi_mr_regattr(dgram) failed: %d\n", ret);
		goto err;
	}

	suet_mr_init(suet_mr, domain, attr->context);

	/* SUET emulates RMA/atomics in software on the target side, so MRs
	 * with remote access must be registered in the mr_map for local
	 * address/key verification in suet_ses_verify_mr_iov. MRs without
	 * remote access (pure local send/recv) don't need a map entry. */
	if (attr->access & (FI_REMOTE_READ | FI_REMOTE_WRITE)) {
		ret = suet_mr_map_insert(domain, &dgram_attr, suet_mr, flags);
		if (ret) {
			goto map_err;
		}
	}

	*mr = &suet_mr->mr_fid;
	return 0;

map_err:
	fi_close(&suet_mr->mr_fid.fid);
	return ret;
err:
	free(suet_mr);
	return ret;
}

static int suet_mr_regv(struct fid *fid, const struct iovec *iov, size_t count,
			uint64_t access, uint64_t offset,
			uint64_t requested_key, uint64_t flags,
			struct fid_mr **mr, void *context)
{
	struct fi_mr_attr attr = {
		.mr_iov = iov,
		.iov_count = count,
		.access = access,
		.offset = offset,
		.requested_key = requested_key,
		.context = context,
	};

	return suet_mr_regattr(fid, &attr, flags, mr);
}

static int suet_mr_reg(struct fid *fid, const void *buf, size_t len,
		       uint64_t access, uint64_t offset, uint64_t requested_key,
		       uint64_t flags, struct fid_mr **mr, void *context)
{
	struct iovec iov = {
		.iov_base = (void *) buf,
		.iov_len = len,
	};

	return suet_mr_regv(fid, &iov, 1, access, offset, requested_key, flags,
			    mr, context);
}

struct fi_ops_mr suet_domain_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = suet_mr_reg,
	.regv = suet_mr_regv,
	.regattr = suet_mr_regattr,
};
