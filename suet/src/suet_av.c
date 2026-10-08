/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2015-2018 Intel Corporation. All rights reserved.
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

#include "suet.h"
#include <inttypes.h>

static int suet_av_set_usr_av_addr(struct suet_av *av, int peer_idx)
{
	int idx;

	idx = ofi_idx_insert(&(av->usr_av_addr_to_peer_idx),
			     (void *) (uintptr_t) peer_idx);
	if (idx < 0)
		return -FI_ENOMEM;

	if (ofi_idm_set(&(av->peer_idx_to_usr_av_addr), (int) peer_idx,
			(void *) (uintptr_t) idx) < 0)
		goto nomem;

	return idx;

nomem:
	ofi_idx_remove_ordered(&(av->usr_av_addr_to_peer_idx), idx);
	return -FI_ENOMEM;
}

static int suet_av_insert(struct fid_av *av_fid, const void *raw_addr,
			  size_t count, fi_addr_t *fi_addr, uint64_t flags,
			  void *context)
{
	struct suet_av *av;
	struct suet_domain *domain;
	const struct suet_av_addr *addr;
	int i = 0, ret = 0, success_cnt = 0;
	int peer_idx;
	int util_addr, *sync_err = NULL;

	av = container_of(av_fid, struct suet_av, util_av.av_fid);
	domain = container_of(av->util_av.domain, struct suet_domain,
			      util_domain);
	ret = ofi_verify_av_insert(&av->util_av, flags, context);
	if (ret)
		return ret;

	if (flags & FI_SYNC_ERR) {
		sync_err = context;
		memset(sync_err, 0, sizeof(*sync_err) * count);
	}

	/* Public mappings are independent of the domain-wide peer table. */
	ofi_genlock_lock(&av->util_av.lock);

	for (; i < count; i++, raw_addr = (uint8_t *) raw_addr +
					  sizeof(struct suet_av_addr)) {
		addr = (const struct suet_av_addr *) raw_addr;
		ret = suet_dgram_av_insert(domain, addr->raw_dgram_addr,
					   &peer_idx, flags,
					   sync_err ? &sync_err[i] : context);
		if (ret)
			break;

		util_addr = (int) (intptr_t) ofi_idm_lookup(
			&av->peer_idx_to_usr_av_addr, (int) peer_idx);
		if (!util_addr) {
			util_addr = suet_av_set_usr_av_addr(av, peer_idx);
			if (util_addr < 0) {
				ret = util_addr;
				break;
			}
		}
		if (fi_addr)
			fi_addr[i] = (util_addr - 1);

		success_cnt++;
	}

	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_AV,
			"failed to insert address %d: %d (%s)\n", i, -ret,
			fi_strerror(-ret));
		if (fi_addr)
			fi_addr[i] = FI_ADDR_NOTAVAIL;
		else if (sync_err)
			sync_err[i] = -ret;
		i++;
	}
	ofi_genlock_unlock(&av->util_av.lock);

	for (; i < count; i++) {
		if (fi_addr)
			fi_addr[i] = FI_ADDR_NOTAVAIL;
		else if (sync_err)
			sync_err[i] = FI_ECANCELED;
	}

	return success_cnt;
}

static int suet_av_insertsvc(struct fid_av *av, const char *node,
			     const char *service, fi_addr_t *fi_addr,
			     uint64_t flags, void *context)
{
	return -FI_ENOSYS;
}

static int suet_av_insertsym(struct fid_av *av_fid, const char *node,
			     size_t nodecnt, const char *service, size_t svccnt,
			     fi_addr_t *fi_addr, uint64_t flags, void *context)
{
	return -FI_ENOSYS;
}

static int suet_av_remove(struct fid_av *av_fid, fi_addr_t *fi_addr,
			  size_t count, uint64_t flags)
{
	int ret = 0;
	size_t i;
	int peer_idx;
	struct suet_av *av;

	av = container_of(av_fid, struct suet_av, util_av.av_fid);
	ofi_genlock_lock(&av->util_av.lock);
	for (i = 0; i < count; i++) {
		peer_idx = (int) (intptr_t) ofi_idx_lookup(
			&av->usr_av_addr_to_peer_idx,
			SUET_IDX_OFFSET((int) fi_addr[i]));
		if (!peer_idx) {
			ret = -FI_EINVAL;
			continue;
		}

		ofi_idx_remove_ordered(&(av->usr_av_addr_to_peer_idx),
				       (int) SUET_IDX_OFFSET(fi_addr[i]));
		ofi_idm_clear(&(av->peer_idx_to_usr_av_addr), (int) peer_idx);
	}

	if (ret)
		FI_WARN(&suet_prov, FI_LOG_AV,
			"Unable to remove address from AV\n");

	ofi_genlock_unlock(&av->util_av.lock);
	return ret;
}

static const char *suet_av_straddr(struct fid_av *av, const void *raw_addr,
				   char *buf, size_t *len)
{
	const struct suet_av_addr *addr =
		(const struct suet_av_addr *) raw_addr;
	struct suet_av *suet_av;
	suet_av = container_of(av, struct suet_av, util_av.av_fid);
	return suet_dgram_av_straddr(container_of(suet_av->util_av.domain,
						  struct suet_domain,
						  util_domain),
				     addr->raw_dgram_addr, buf, len);
}

static int suet_av_lookup(struct fid_av *av, fi_addr_t fi_addr, void *raw_addr,
			  size_t *addrlen)
{
	return -FI_ENOSYS;
}

static struct fi_ops_av suet_av_ops = {
	.size = sizeof(struct fi_ops_av),
	.insert = suet_av_insert,
	.insertsvc = suet_av_insertsvc,
	.insertsym = suet_av_insertsym,
	.remove = suet_av_remove,
	.lookup = suet_av_lookup,
	.straddr = suet_av_straddr,
};

static int suet_av_close(struct fid *fid)
{
	struct suet_av *av;
	int ret;

	av = container_of(fid, struct suet_av, util_av.av_fid);

	ret = ofi_av_close(&av->util_av);
	if (ret)
		return ret;

	ofi_idx_reset(&(av->usr_av_addr_to_peer_idx));
	ofi_idm_reset(&(av->peer_idx_to_usr_av_addr), NULL);

	free(av);
	return 0;
}

static struct fi_ops suet_av_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = suet_av_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int suet_av_create(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		   struct fid_av **av_fid, void *context)
{
	int ret;
	struct suet_av *av;
	struct suet_domain *domain;
	struct util_av_attr util_attr;

	if (!attr)
		return -FI_EINVAL;

	if (attr->name)
		return -FI_ENOSYS;

	// TODO implement dynamic AV sizing
	attr->count = roundup_power_of_two(attr->count ? attr->count :
							 suet_env.max_peers);
	domain = container_of(domain_fid, struct suet_domain,
			      util_domain.domain_fid);
	av = calloc(1, sizeof(*av));
	if (!av)
		return -FI_ENOMEM;
	memset(&(av->usr_av_addr_to_peer_idx), 0,
	       sizeof(av->usr_av_addr_to_peer_idx));
	memset(&(av->peer_idx_to_usr_av_addr), 0,
	       sizeof(av->peer_idx_to_usr_av_addr));

	util_attr.addrlen = sizeof(fi_addr_t);
	util_attr.context_len = 0;
	util_attr.flags = 0;
	attr->type = domain->util_domain.av_type != FI_AV_UNSPEC ?
			     domain->util_domain.av_type :
			     FI_AV_TABLE;

	ret = ofi_av_init(&domain->util_domain, attr, &util_attr, &av->util_av,
			  context);
	if (ret)
		goto err1;

	av->util_av.av_fid.fid.ops = &suet_av_fi_ops;
	av->util_av.av_fid.ops = &suet_av_ops;
	*av_fid = &av->util_av.av_fid;
	return 0;

err1:
	free(av);
	return ret;
}

int suet_av_peer_idx_from_usr_av_addr(struct suet_av *av, fi_addr_t addr)
{
	int peer_idx;

	ofi_genlock_lock(&av->util_av.lock);
	peer_idx = (int) (intptr_t) ofi_idx_lookup(&av->usr_av_addr_to_peer_idx,
						   SUET_IDX_OFFSET((int) addr));
	ofi_genlock_unlock(&av->util_av.lock);
	return peer_idx;
}
