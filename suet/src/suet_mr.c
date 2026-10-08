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

/* Public MR objects and remote access verification belong to SUET.
 * Datagram registration handles and descriptors are opaque to this layer.
 */
void *suet_mr_desc(void *desc)
{
	struct suet_mr *mr = desc;

	return mr ? suet_dgram_mr_desc(mr->dgram_ctx) : NULL;
}

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

	ret = suet_dgram_mr_close(suet_mr->dgram_ctx);
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

void suet_mr_closev_internal(void **mr, size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		if (!mr[i])
			continue;
		ret = suet_dgram_mr_close(mr[i]);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_EP_DATA,
				"fi_close(dgram mr[%zu]) failed: %d\n", i, ret);
		mr[i] = NULL;
	}
}

/*
 * Register an IOV through the datagram layer for a single send,
 * capped at reg_limit total bytes. Used by the TX path when the send
 * length exceeds suet_env.mr_reg_threshold.
 */
int suet_mr_regv_internal(struct suet_domain *domain, const struct iovec *iov,
			  size_t count, size_t reg_limit, uint64_t access,
			  void **mr, void **desc)
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
		ret = suet_dgram_mr_reg(domain, &attr, 0, true, &mr[i]);
		if (ret)
			goto err;
		desc[i] = suet_dgram_mr_desc(mr[i]);
		reg_limit -= len;
	}
	return 0;
err:
	suet_mr_closev_internal(mr, i);
	memset(desc, 0, count * sizeof(*desc));
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

	ret = suet_dgram_mr_reg(domain, &dgram_attr, flags, false,
				&suet_mr->dgram_ctx);
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
