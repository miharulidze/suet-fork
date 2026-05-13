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

static int suet_domain_dg_av_tree_compare_fn(struct ofi_rbmap *map, void *key,
					     void *data)
{
	struct suet_domain *domain;
	uint8_t raw_dg_addr[SUET_DG_AV_NAME_LENGTH];
	size_t raw_dg_addr_len = sizeof(raw_dg_addr);
	int ret;
	fi_addr_t dg_av_addr;

	memset(raw_dg_addr, 0, raw_dg_addr_len);
	domain = container_of(map, struct suet_domain, rbmap);
	dg_av_addr = suet_domain_dg_av_get_addr_by_peer_idx(
		domain, (int) (intptr_t) data);

	ret = fi_av_lookup(domain->dg_av, dg_av_addr, raw_dg_addr,
			   &raw_dg_addr_len);
	if (ret)
		return -1;

	return memcmp(key, raw_dg_addr, raw_dg_addr_len);
}

/*
 * The SUET code is agnostic wrt the datagram address format, but we need
 * to know the size of the address in order to iterate over them.  Because
 * the datagram AV may be configured for asynchronous operation, open a
 * temporary one to insert/lookup the address to get the size.  I agree it's
 * goofy.
 */
static int suet_domain_dg_av_set_addrlen(struct suet_domain *domain,
					 const void *addr)
{
	struct fid_av *tmp_av;
	struct fi_av_attr attr;
	uint8_t tmp_addr[SUET_DG_AV_NAME_LENGTH];
	fi_addr_t fiaddr;
	size_t len;
	int ret;

	FI_INFO(&suet_prov, FI_LOG_AV, "determine dgram address len\n");
	memset(&attr, 0, sizeof attr);
	attr.count = 1;

	ret = fi_av_open(domain->fep_domain, &attr, &tmp_av, NULL);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_AV, "failed to open av: %d (%s)\n",
			-ret, fi_strerror(-ret));
		return ret;
	}

	ret = fi_av_insert(tmp_av, addr, 1, &fiaddr, 0, NULL);
	if (ret != 1) {
		FI_WARN(&suet_prov, FI_LOG_AV, "addr insert failed: %d (%s)\n",
			-ret, fi_strerror(-ret));
		ret = -FI_EINVAL;
		goto close;
	}

	len = sizeof tmp_addr;
	ret = fi_av_lookup(tmp_av, fiaddr, tmp_addr, &len);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_AV, "addr lookup failed: %d (%s)\n",
			-ret, fi_strerror(-ret));
		goto close;
	}

	FI_INFO(&suet_prov, FI_LOG_AV, "set dgram address len: %zu\n", len);
	domain->dg_addrlen = len;
close:
	fi_close(&tmp_av->fid);
	return ret;
}

static int suet_domain_dg_av_set_peer_idx(struct suet_domain *domain,
					  fi_addr_t addr, int *peer_idx)
{
	int new_peer_idx;
	new_peer_idx = ofi_idx_insert(&(domain->peer_idx_to_dg_av_addr),
				      (void *) (uintptr_t) addr);
	if (new_peer_idx < 0)
		return -FI_ENOMEM;
	*peer_idx = new_peer_idx;
	return 0;
}

void suet_domain_dg_av_cleanup(struct suet_domain *domain)
{
	struct ofi_rbnode *node;
	fi_addr_t dg_av_addr;
	int peer_idx;
	int ret;

	while ((node = ofi_rbmap_get_root(&domain->rbmap))) {
		peer_idx = (int) (intptr_t) node->data;
		dg_av_addr = suet_domain_dg_av_get_addr_by_peer_idx(domain,
								    peer_idx);

		ret = fi_av_remove(domain->dg_av, &dg_av_addr, 1, 0);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_AV,
				"failed to remove dg addr: %d (%s)\n", -ret,
				fi_strerror(-ret));

		ofi_idx_remove_ordered(&domain->peer_idx_to_dg_av_addr,
				       peer_idx);
		ofi_rbmap_delete(&domain->rbmap, node);
	}
	ofi_rbmap_cleanup(&domain->rbmap);
	ofi_idx_reset(&domain->peer_idx_to_dg_av_addr);

	ofi_genlock_destroy(&domain->av_lock);

	if (domain->dg_av) {
		ret = fi_close(&domain->dg_av->fid);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_AV,
				"failed to close dg_av: %d (%s)\n", -ret,
				fi_strerror(-ret));
		domain->dg_av = NULL;
	}
}

int suet_domain_dg_av_init(struct suet_domain *domain)
{
	struct fi_av_attr av_attr = {0};
	int ret;

	av_attr.type = FI_AV_UNSPEC;
	ret = fi_av_open(domain->fep_domain, &av_attr, &domain->dg_av, NULL);
	if (ret)
		return ret;

	ofi_rbmap_init(&domain->rbmap, suet_domain_dg_av_tree_compare_fn);
	memset(&domain->peer_idx_to_dg_av_addr, 0,
	       sizeof(domain->peer_idx_to_dg_av_addr));

	ret = ofi_genlock_init(&domain->av_lock,
			       domain->util_domain.threading != FI_THREAD_SAFE ?
				       OFI_LOCK_NOOP :
				       OFI_LOCK_MUTEX);
	if (ret) {
		ofi_rbmap_cleanup(&domain->rbmap);
		fi_close(&domain->dg_av->fid);
		domain->dg_av = NULL;
		return ret;
	}

	return 0;
}

int suet_domain_dg_av_get_peer_idx_by_addr(struct suet_domain *domain,
					   fi_addr_t dg_av_addr)
{
	uint8_t raw_addr[SUET_DG_AV_NAME_LENGTH];
	size_t addrlen = sizeof(raw_addr);
	struct ofi_rbnode *node;
	int ret;

	memset(raw_addr, 0, addrlen);
	ret = fi_av_lookup(domain->dg_av, dg_av_addr, raw_addr, &addrlen);
	if (ret)
		return 0;

	node = ofi_rbmap_find(&domain->rbmap, raw_addr);
	if (!node)
		return 0;

	return (int) (intptr_t) node->data;
}

int suet_domain_dg_av_insert_raw_addr(struct suet_domain *domain,
				      const void *raw_addr, int *peer_idx,
				      uint64_t flags, void *context)
{
	struct ofi_rbnode *node;
	fi_addr_t dg_av_addr;
	int ret;

	node = ofi_rbmap_find(&domain->rbmap, (void *) raw_addr);
	if (node) {
		*peer_idx = (int) (intptr_t) node->data;
		return 0;
	}

	ret = fi_av_insert(domain->dg_av, raw_addr, 1, &dg_av_addr, flags,
			   context);
	if (ret != 1)
		return -FI_EINVAL;

	ret = suet_domain_dg_av_set_peer_idx(domain, dg_av_addr, peer_idx);
	if (ret < 0)
		goto nomem;

	ret = ofi_rbmap_insert(&domain->rbmap, (void *) raw_addr,
			       (void *) (uintptr_t) (*peer_idx), NULL);
	if (ret) {
		ofi_idx_remove_ordered(&(domain->peer_idx_to_dg_av_addr),
				       (int) (*peer_idx));
		goto nomem;
	}

	return ret;
nomem:
	fi_av_remove(domain->dg_av, &dg_av_addr, 1, flags);
	return ret;
}

int suet_domain_dg_av_handle_addr_notavail(struct suet_domain *domain,
					   struct fi_cq_err_entry *err,
					   struct fi_cq_msg_entry *comp,
					   fi_addr_t *dg_av_addr)
{
	int peer_idx;
	ssize_t ret;

	ofi_genlock_lock(&domain->av_lock);

	/* err_data stores raw address */
	ret = fi_av_insert(domain->dg_av, err->err_data, 1, dg_av_addr, 0,
			   NULL);
	if (ret != 1) {
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"Failed to insert source address into dg AV\n");
		*dg_av_addr = FI_ADDR_UNSPEC;
		ofi_genlock_unlock(&domain->av_lock);
		return -FI_EINVAL;
	}

	FI_DBG(&suet_prov, FI_LOG_CQ,
	       "FI_SOURCE_ERR: inserted unknown source into dg AV "
	       "as dg_av_addr=%ld\n",
	       (long) *dg_av_addr);

	ret = suet_domain_dg_av_insert_raw_addr(domain, err->err_data,
						&peer_idx, 0, NULL);
	ofi_genlock_unlock(&domain->av_lock);
	if (ret)
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"suet_av_insert_raw_dg_addr failed: %s\n",
			fi_strerror(-ret));

	comp->op_context = err->op_context;
	comp->flags = err->flags;
	comp->len = err->len;

	return 0;
}

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
	const struct suet_av_addr *addr, *tmp;
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

	ofi_genlock_lock(&domain->av_lock);
	if (!domain->dg_addrlen) {
		tmp = (const struct suet_av_addr *) raw_addr;
		ret = suet_domain_dg_av_set_addrlen(domain, tmp->raw_dg_addr);
		if (ret)
			goto out;
	}

	for (; i < count; i++, raw_addr = (uint8_t *) raw_addr +
					  sizeof(struct suet_av_addr)) {
		addr = (const struct suet_av_addr *) raw_addr;
		ret = suet_domain_dg_av_insert_raw_addr(
			domain, addr->raw_dg_addr, &peer_idx, flags,
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
out:
	ofi_genlock_unlock(&domain->av_lock);

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
	struct suet_domain *domain;

	av = container_of(av_fid, struct suet_av, util_av.av_fid);
	domain = container_of(av->util_av.domain, struct suet_domain,
			      util_domain);
	ofi_genlock_lock(&domain->av_lock);
	for (i = 0; i < count; i++) {
		peer_idx = suet_av_peer_idx_from_usr_av_addr(av, fi_addr[i]);
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

	ofi_genlock_unlock(&domain->av_lock);
	return ret;
}

static const char *suet_av_straddr(struct fid_av *av, const void *raw_addr,
				   char *buf, size_t *len)
{
	const struct suet_av_addr *addr =
		(const struct suet_av_addr *) raw_addr;
	struct suet_av *suet_av;
	suet_av = container_of(av, struct suet_av, util_av.av_fid);
	return suet_av->dg_av->ops->straddr(suet_av->dg_av, addr->raw_dg_addr,
					    buf, len);
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

	av->dg_av = domain->dg_av;
	av->util_av.av_fid.fid.ops = &suet_av_fi_ops;
	av->util_av.av_fid.ops = &suet_av_ops;
	*av_fid = &av->util_av.av_fid;
	return 0;

err1:
	free(av);
	return ret;
}