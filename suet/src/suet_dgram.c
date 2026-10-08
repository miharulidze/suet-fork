/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2016-2017 Intel Corporation. All rights reserved.
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
#include "suet_ext.h"
#include "suet_pds_dgram_api.h"

#define SUET_MR_KEY_MAX_RETRIES 1024

static void suet_dgram_info_mr_modes(uint32_t version,
				     const struct fi_info *hints,
				     struct fi_info *core_info)
{
	/* We handle FI_MR_BASIC and FI_MR_SCALABLE irrespective of version */
	if (hints && hints->domain_attr &&
	    (hints->domain_attr->mr_mode & (OFI_MR_SCALABLE | OFI_MR_BASIC))) {
		core_info->mode = OFI_LOCAL_MR;
		core_info->domain_attr->mr_mode = hints->domain_attr->mr_mode;
	} else if (FI_VERSION_LT(version, FI_VERSION(1, 5))) {
		core_info->mode |= OFI_LOCAL_MR;
		/* Specify FI_MR_UNSPEC (instead of FI_MR_BASIC) so that
		 * providers that support only FI_MR_SCALABLE aren't dropped */
		core_info->domain_attr->mr_mode = OFI_MR_UNSPEC;
	} else {
		core_info->domain_attr->mr_mode |= FI_MR_LOCAL;
		core_info->domain_attr->mr_mode |= OFI_MR_BASIC_MAP;
	}
}

static int suet_dgram_info_to_core(uint32_t version,
				   const struct fi_info *suet_info_in,
				   const struct fi_info *base_info,
				   struct fi_info *core_info)
{
	suet_dgram_info_mr_modes(version, suet_info_in, core_info);
	core_info->caps = FI_MSG | FI_SOURCE | FI_SOURCE_ERR;
	core_info->mode = OFI_LOCAL_MR | FI_CONTEXT | FI_MSG_PREFIX;
	core_info->ep_attr->type = FI_EP_DGRAM;
	core_info->domain_attr->threading = FI_THREAD_DOMAIN;
	return 0;
}

static int suet_dgram_info_to_suet(uint32_t version,
				   const struct fi_info *core_info,
				   const struct fi_info *base_info,
				   struct fi_info *info)
{
	if (core_info->src_addrlen > SUET_DGRAM_AV_NAME_LENGTH) {
		FI_INFO(&suet_prov, FI_LOG_CORE,
			"core provider %s address length %zu exceeds "
			"SUET_DGRAM_AV_NAME_LENGTH (%d), skipping\n",
			core_info->fabric_attr->prov_name,
			core_info->src_addrlen, SUET_DGRAM_AV_NAME_LENGTH);
		return -FI_EINVAL;
	}

	suet_info_from_dgram(info, core_info->caps,
			     core_info->domain_attr->caps,
			     core_info->ep_attr->max_msg_size,
			     core_info->ep_attr->msg_prefix_size);
	if (core_info->nic) {
		info->nic = ofi_nic_dup(core_info->nic);
		if (!info->nic)
			return -FI_ENOMEM;
	}
	return 0;
}

int suet_dgram_getinfo(uint32_t version, const char *node, const char *service,
		       uint64_t flags, const struct fi_info *hints,
		       struct fi_info **info)
{
	return ofix_getinfo(version, node, service, flags, &suet_util_prov,
			    hints, suet_dgram_info_to_core,
			    suet_dgram_info_to_suet, info);
}

static int suet_dgram_get_core_info(uint32_t version, struct fi_info *info,
				    struct fi_info **core_info)
{
	struct suet_av_addr_tmp_storage save = {0};
	int ret;

	suet_av_info_unwrap_raw_dgram_addrs(info, &save);
	ret = ofi_get_core_info(version, NULL, NULL, 0, &suet_util_prov, info,
				NULL, suet_dgram_info_to_core, core_info);
	suet_av_info_wrap_raw_dgram_addrs(info, &save);
	return ret;
}

int suet_dgram_ep_sizes(struct suet_domain *domain, struct fi_info *info,
			size_t *tx_size, size_t *rx_size)
{
	struct fi_info *core_info;
	int ret;

	ret = suet_dgram_get_core_info(
		domain->util_domain.fabric->fabric_fid.api_version, info,
		&core_info);
	if (ret)
		return ret;
	*tx_size = MIN(core_info->tx_attr->size, info->tx_attr->size);
	*rx_size = MIN(core_info->rx_attr->size, info->rx_attr->size);
	fi_freeinfo(core_info);
	return 0;
}

int suet_dgram_fabric_open(struct fi_fabric_attr *attr, void *context,
			   void **handle)
{
	struct fi_info *info;
	struct fid_fabric *fabric;
	int ret;

	ret = ofi_get_core_info_fabric(&suet_prov, attr, &info);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_FABRIC,
			"Unable to get core info!\n");
		return -FI_EINVAL;
	}
	ret = fi_fabric(info->fabric_attr, &fabric, context);
	fi_freeinfo(info);
	if (!ret)
		*handle = fabric;
	return ret;
}

int suet_dgram_fabric_close(void *handle)
{
	struct fid_fabric *fabric = handle;

	return fi_close(&fabric->fid);
}

size_t suet_dgram_max_pkt_size(const struct suet_domain *domain)
{
	return domain->dgram.max_pkt_size;
}

int suet_dgram_setname(struct suet_domain *domain, void *addr, size_t len)
{
	return fi_setname(&domain->dgram.ep->fid, addr, len);
}

int suet_dgram_getname(struct suet_domain *domain, void *addr, size_t *len)
{
	return fi_getname(&domain->dgram.ep->fid, addr, len);
}

const char *suet_dgram_cq_strerror(struct suet_domain *domain, int err,
				   const void *data, char *buf, size_t len)
{
	return fi_cq_strerror(domain->dgram.tx_cq, err, data, buf, len);
}

static fi_addr_t suet_dgram_av_lookup_peer(struct suet_domain *domain,
					   int peer_idx)
{
	return (fi_addr_t) (intptr_t) ofi_idx_lookup(
		&domain->dgram.peer_idx_to_av_addr, (int) peer_idx);
}

static int suet_dgram_av_tree_compare_fn(struct ofi_rbmap *map, void *key,
					 void *data)
{
	struct suet_domain *domain;
	uint8_t raw_dgram_addr[SUET_DGRAM_AV_NAME_LENGTH];
	size_t raw_dgram_addr_len = sizeof(raw_dgram_addr);
	int ret;
	fi_addr_t dgram_av_addr;

	memset(raw_dgram_addr, 0, raw_dgram_addr_len);
	domain = container_of(map, struct suet_domain, dgram.rbmap);
	dgram_av_addr =
		suet_dgram_av_lookup_peer(domain, (int) (intptr_t) data);

	ret = fi_av_lookup(domain->dgram.av, dgram_av_addr, raw_dgram_addr,
			   &raw_dgram_addr_len);
	if (ret)
		return -1;

	return memcmp(key, raw_dgram_addr, raw_dgram_addr_len);
}

/*
 * The SUET code is agnostic wrt the datagram address format, but we need
 * to know the size of the address in order to iterate over them.  Because
 * the datagram AV may be configured for asynchronous operation, open a
 * temporary one to insert/lookup the address to get the size.  I agree it's
 * goofy.
 */
static int suet_dgram_av_set_addrlen(struct suet_domain *domain,
				     const void *addr)
{
	struct fid_av *tmp_av;
	struct fi_av_attr attr;
	uint8_t tmp_addr[SUET_DGRAM_AV_NAME_LENGTH];
	fi_addr_t fiaddr;
	size_t len;
	int ret;

	FI_INFO(&suet_prov, FI_LOG_AV, "determine dgram address len\n");
	memset(&attr, 0, sizeof attr);
	attr.count = 1;

	ret = fi_av_open(domain->dgram.domain, &attr, &tmp_av, NULL);
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
	domain->dgram.addrlen = len;
close:
	fi_close(&tmp_av->fid);
	return ret;
}

static int suet_dgram_av_set_peer_idx(struct suet_domain *domain,
				      fi_addr_t addr, int *peer_idx)
{
	int new_peer_idx;
	new_peer_idx = ofi_idx_insert(&(domain->dgram.peer_idx_to_av_addr),
				      (void *) (uintptr_t) addr);
	if (new_peer_idx < 0)
		return -FI_ENOMEM;
	*peer_idx = new_peer_idx;
	return 0;
}

static void suet_dgram_av_cleanup(struct suet_domain *domain)
{
	struct ofi_rbnode *node;
	fi_addr_t dgram_av_addr;
	int peer_idx;
	int ret;

	while ((node = ofi_rbmap_get_root(&domain->dgram.rbmap))) {
		peer_idx = (int) (intptr_t) node->data;
		dgram_av_addr = suet_dgram_av_lookup_peer(domain, peer_idx);

		ret = fi_av_remove(domain->dgram.av, &dgram_av_addr, 1, 0);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_AV,
				"failed to remove dgram addr: %d (%s)\n", -ret,
				fi_strerror(-ret));

		ofi_idx_remove_ordered(&domain->dgram.peer_idx_to_av_addr,
				       peer_idx);
		ofi_rbmap_delete(&domain->dgram.rbmap, node);
	}
	ofi_rbmap_cleanup(&domain->dgram.rbmap);
	ofi_idx_reset(&domain->dgram.peer_idx_to_av_addr);

	ofi_genlock_destroy(&domain->dgram.av_lock);

	if (domain->dgram.av) {
		ret = fi_close(&domain->dgram.av->fid);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_AV,
				"failed to close dgram_av: %d (%s)\n", -ret,
				fi_strerror(-ret));
		domain->dgram.av = NULL;
	}
}

static int suet_dgram_av_init(struct suet_domain *domain)
{
	struct fi_av_attr av_attr = {0};
	int ret;

	av_attr.type = FI_AV_UNSPEC;
	ret = fi_av_open(domain->dgram.domain, &av_attr, &domain->dgram.av,
			 NULL);
	if (ret)
		return ret;

	ofi_rbmap_init(&domain->dgram.rbmap, suet_dgram_av_tree_compare_fn);
	memset(&domain->dgram.peer_idx_to_av_addr, 0,
	       sizeof(domain->dgram.peer_idx_to_av_addr));

	ret = ofi_genlock_init(&domain->dgram.av_lock,
			       domain->util_domain.threading != FI_THREAD_SAFE ?
				       OFI_LOCK_NOOP :
				       OFI_LOCK_MUTEX);
	if (ret) {
		ofi_rbmap_cleanup(&domain->dgram.rbmap);
		fi_close(&domain->dgram.av->fid);
		domain->dgram.av = NULL;
		return ret;
	}

	return 0;
}

int suet_dgram_av_get_peer_idx_by_addr(struct suet_domain *domain,
				       fi_addr_t dgram_av_addr)
{
	uint8_t raw_addr[SUET_DGRAM_AV_NAME_LENGTH];
	size_t addrlen = sizeof(raw_addr);
	struct ofi_rbnode *node;
	int ret, peer_idx = 0;

	ofi_genlock_lock(&domain->dgram.av_lock);
	memset(raw_addr, 0, addrlen);
	ret = fi_av_lookup(domain->dgram.av, dgram_av_addr, raw_addr, &addrlen);
	if (ret)
		goto out;

	node = ofi_rbmap_find(&domain->dgram.rbmap, raw_addr);
	if (!node)
		goto out;

	peer_idx = (int) (intptr_t) node->data;
out:
	ofi_genlock_unlock(&domain->dgram.av_lock);
	return peer_idx;
}

static int suet_dgram_av_insert_raw_addr(struct suet_domain *domain,
					 const void *raw_addr, int *peer_idx,
					 uint64_t flags, void *context)
{
	struct ofi_rbnode *node;
	fi_addr_t dgram_av_addr;
	int ret;

	node = ofi_rbmap_find(&domain->dgram.rbmap, (void *) raw_addr);
	if (node) {
		*peer_idx = (int) (intptr_t) node->data;
		return 0;
	}

	ret = fi_av_insert(domain->dgram.av, raw_addr, 1, &dgram_av_addr, flags,
			   context);
	if (ret != 1)
		return -FI_EINVAL;

	ret = suet_dgram_av_set_peer_idx(domain, dgram_av_addr, peer_idx);
	if (ret < 0)
		goto nomem;

	ret = ofi_rbmap_insert(&domain->dgram.rbmap, (void *) raw_addr,
			       (void *) (uintptr_t) (*peer_idx), NULL);
	if (ret) {
		ofi_idx_remove_ordered(&(domain->dgram.peer_idx_to_av_addr),
				       (int) (*peer_idx));
		goto nomem;
	}

	return ret;
nomem:
	fi_av_remove(domain->dgram.av, &dgram_av_addr, 1, flags);
	return ret;
}

static int suet_dgram_av_handle_addr_notavail(struct suet_domain *domain,
					      struct fi_cq_err_entry *err,
					      struct fi_cq_data_entry *comp,
					      fi_addr_t *dgram_av_addr)
{
	int peer_idx;
	ssize_t ret;

	ofi_genlock_lock(&domain->dgram.av_lock);

	/* err_data stores raw address */
	ret = fi_av_insert(domain->dgram.av, err->err_data, 1, dgram_av_addr, 0,
			   NULL);
	if (ret != 1) {
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"Failed to insert source address into dgram AV\n");
		*dgram_av_addr = FI_ADDR_UNSPEC;
		ofi_genlock_unlock(&domain->dgram.av_lock);
		return -FI_EINVAL;
	}

	FI_DBG(&suet_prov, FI_LOG_CQ,
	       "FI_SOURCE_ERR: inserted unknown source into dgram AV "
	       "as dgram_av_addr=%ld\n",
	       (long) *dgram_av_addr);

	ret = suet_dgram_av_insert_raw_addr(domain, err->err_data, &peer_idx, 0,
					    NULL);
	ofi_genlock_unlock(&domain->dgram.av_lock);
	if (ret)
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"suet_av_insert_raw_dgram_addr failed: %s\n",
			fi_strerror(-ret));

	comp->op_context = err->op_context;
	comp->flags = err->flags;
	comp->len = err->len;
	comp->data = err->data;

	return 0;
}

fi_addr_t suet_dgram_av_get_addr_by_peer_idx(struct suet_domain *domain,
					     int peer_idx)
{
	fi_addr_t addr;

	ofi_genlock_lock(&domain->dgram.av_lock);
	addr = suet_dgram_av_lookup_peer(domain, peer_idx);
	ofi_genlock_unlock(&domain->dgram.av_lock);
	return addr;
}

int suet_dgram_av_insert(struct suet_domain *domain, const void *addr,
			 int *peer_idx, uint64_t flags, void *context)
{
	int ret;

	ofi_genlock_lock(&domain->dgram.av_lock);
	if (!domain->dgram.addrlen) {
		ret = suet_dgram_av_set_addrlen(domain, addr);
		if (ret)
			goto out;
	}
	ret = suet_dgram_av_insert_raw_addr(domain, addr, peer_idx, flags,
					    context);
out:
	ofi_genlock_unlock(&domain->dgram.av_lock);
	return ret;
}

const char *suet_dgram_av_straddr(struct suet_domain *domain, const void *addr,
				  char *buf, size_t *len)
{
	return fi_av_straddr(domain->dgram.av, addr, buf, len);
}

/*
 * Calls fi_mr_regattr() on the underlying DGRAM domain.
 * When gen_key is true the function ignores the caller's
 * attr->requested_key and draws from the domain-internal namespace
 * (bit 31 set), retrying on collisions.
 */
int suet_dgram_mr_reg(struct suet_domain *domain, struct fi_mr_attr *attr,
		      uint64_t flags, bool gen_key, void **handle)
{
	struct fid_mr *mr;
	int ret, tries = 0;

	if (!gen_key) {
		ret = fi_mr_regattr(domain->dgram.domain, attr, flags, &mr);
		goto out;
	}

	/* The counter may collide with a user-chosen requested_key already
	 * registered on the datagram domain, so advance and retry on
	 * -FI_ENOKEY. */
	do {
		attr->requested_key = domain->dgram.mr_key++ | (1UL << 31);
		ret = fi_mr_regattr(domain->dgram.domain, attr, flags, &mr);
	} while (ret == -FI_ENOKEY && tries++ < SUET_MR_KEY_MAX_RETRIES);

out:
	if (!ret)
		*handle = mr;
	return ret;
}

int suet_dgram_mr_close(void *handle)
{
	struct fid_mr *mr = handle;

	return fi_close(&mr->fid);
}

void *suet_dgram_mr_desc(void *handle)
{
	struct fid_mr *mr = handle;

	return mr ? fi_mr_desc(mr) : NULL;
}

#define SUET_TX_POOL_CHUNK_CNT 1024
#define SUET_RX_POOL_CHUNK_CNT 1024

/* Private provider metadata and the shared packet view occupy one pool slot.
 * Separate layer contexts, prefix space and wire bytes follow this record.
 */
struct suet_dgram_pkt_entry {
	struct suet_pkt_entry pkt;
	struct fi_context context;
	void *desc;
	struct dlist_entry entry;
	bool in_use;
};

static size_t suet_dgram_pkt_ctx_size(size_t size)
{
	return (size + SUET_BUF_POOL_ALIGNMENT - 1) &
	       ~(SUET_BUF_POOL_ALIGNMENT - 1);
}

bool suet_dgram_pkt_in_use(const struct suet_pkt_entry *pkt)
{
	const struct suet_dgram_pkt_entry *entry =
		container_of(pkt, struct suet_dgram_pkt_entry, pkt);

	return entry->in_use;
}

struct suet_pkt_entry *suet_dgram_pkt_alloc(struct suet_domain *domain)
{
	struct suet_dgram_pkt_entry *entry;

	entry = ofi_buf_alloc(domain->dgram.tx_pkt_entry_pool);
	if (!entry)
		return NULL;
	entry->in_use = false;
	dlist_init(&entry->entry);
	entry->pkt.zc_pld_iov_count = 0;
	entry->pkt.rx_flags = 0;
	entry->pkt.ev = 0;
	return &entry->pkt;
}

void suet_dgram_pkt_free(struct suet_pkt_entry *pkt)
{
	struct suet_dgram_pkt_entry *entry =
		container_of(pkt, struct suet_dgram_pkt_entry, pkt);

	assert(!entry->in_use);
	assert(dlist_empty(&entry->entry));
	ofi_buf_free(entry);
}

static inline int
suet_dgram_buf_region_alloc_fn(struct ofi_bufpool_region *region)
{
	struct suet_dgram_resources *dgram = region->pool->attr.context;
	struct fid_mr *mr;
	int ret;

	if (!dgram->do_local_mr) {
		region->context = NULL;
		return 0;
	}

	ret = fi_mr_reg(dgram->domain, region->mem_region,
			region->pool->region_size, FI_SEND | FI_RECV, 0, 0,
			OFI_MR_NOCACHE, &mr, NULL);

	region->context = mr;
	return ret;
}

static inline void
suet_dgram_buf_region_free_fn(struct ofi_bufpool_region *region)
{
	struct suet_dgram_resources *dgram = region->pool->attr.context;

	if (dgram->do_local_mr)
		fi_close(region->context);
}

static void suet_dgram_free_pkt_entry_pools(struct suet_domain *domain)
{
	if (domain->dgram.tx_pkt_entry_pool) {
		ofi_bufpool_destroy(domain->dgram.tx_pkt_entry_pool);
		domain->dgram.tx_pkt_entry_pool = NULL;
	}

	if (domain->dgram.rx_pkt_entry_pool) {
		ofi_bufpool_destroy(domain->dgram.rx_pkt_entry_pool);
		domain->dgram.rx_pkt_entry_pool = NULL;
	}
}

static void suet_dgram_pkt_entry_init_fn(struct ofi_bufpool_region *region,
					 void *buf)
{
	struct suet_dgram_pkt_entry *entry = buf;
	struct suet_dgram_resources *dgram = region->pool->attr.context;
	size_t prefix_size = region->pool == dgram->rx_pkt_entry_pool ?
				     dgram->rx_prefix_size :
				     dgram->tx_prefix_size;

	entry->desc = dgram->do_local_mr ?
			      fi_mr_desc((struct fid_mr *) region->context) :
			      NULL;
	entry->pkt.pds_ctx =
		(char *) entry + suet_dgram_pkt_ctx_size(sizeof(*entry));
	entry->pkt.ses_ctx = (char *) entry->pkt.pds_ctx +
			     suet_dgram_pkt_ctx_size(dgram->pds_pkt_size);
	entry->pkt.pkt = (char *) entry->pkt.ses_ctx +
			 suet_dgram_pkt_ctx_size(dgram->ses_pkt_size) +
			 prefix_size;
}

static int suet_dgram_pkt_entry_pool_create(struct suet_domain *domain,
					    size_t chunk_cnt,
					    struct ofi_bufpool **pool)
{
	struct ofi_bufpool_attr attr = {
		.size = domain->dgram.max_mtu_sz +
			suet_dgram_pkt_ctx_size(
				sizeof(struct suet_dgram_pkt_entry)) +
			suet_dgram_pkt_ctx_size(domain->dgram.pds_pkt_size) +
			suet_dgram_pkt_ctx_size(domain->dgram.ses_pkt_size),
		.alignment = SUET_BUF_POOL_ALIGNMENT,
		.max_cnt = 0,
		.chunk_cnt = chunk_cnt,
		.alloc_fn = suet_dgram_buf_region_alloc_fn,
		.free_fn = suet_dgram_buf_region_free_fn,
		.init_fn = suet_dgram_pkt_entry_init_fn,
		.context = &domain->dgram,
		.flags = OFI_BUFPOOL_HUGEPAGES,
	};
	int ret;

	ret = ofi_bufpool_create_attr(&attr, pool);
	if (ret)
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"Unable to create packet pool\n");
	return ret;
}

static int suet_dgram_init_pkt_entry_pools(struct suet_domain *domain)
{
	int ret;

	ret = suet_dgram_pkt_entry_pool_create(
		domain, SUET_TX_POOL_CHUNK_CNT,
		&domain->dgram.tx_pkt_entry_pool);
	if (ret)
		goto err;

	ret = suet_dgram_pkt_entry_pool_create(
		domain, SUET_RX_POOL_CHUNK_CNT,
		&domain->dgram.rx_pkt_entry_pool);
	if (ret)
		goto err;

	return 0;
err:
	suet_dgram_free_pkt_entry_pools(domain);
	return ret;
}

ssize_t suet_dgram_send(struct suet_domain *domain, struct suet_pkt_entry *pkt)
{
	struct suet_dgram_pkt_entry *entry =
		container_of(pkt, struct suet_dgram_pkt_entry, pkt);
	size_t prefix_size = domain->dgram.tx_prefix_size;
	struct iovec iov[SUET_IOV_LIMIT + 1];
	struct fi_msg_ev msg = {0};
	ssize_t ret;

	assert(!entry->in_use);
	msg.msg.msg_iov = iov;
	msg.msg.addr = pkt->dgram_av_addr;
	msg.msg.context = &entry->context;
	msg.ev = pkt->ev;
	if (pkt->zc_pld_iov_count) {
		/* Keep prefixes in a local view, including on retransmission.
		 */
		memcpy(iov, pkt->zc_pld_iov,
		       (pkt->zc_pld_iov_count + 1) * sizeof(*iov));
		iov[0].iov_base = (char *) pkt->pkt - prefix_size;
		iov[0].iov_len += prefix_size;
		pkt->zc_pld_desc[0] = entry->desc;
		msg.msg.desc = pkt->zc_pld_desc;
		msg.msg.iov_count = pkt->zc_pld_iov_count + 1;
	} else {
		iov[0].iov_base = (char *) pkt->pkt - prefix_size;
		iov[0].iov_len = pkt->pkt_size + prefix_size;
		msg.msg.desc = &entry->desc;
		msg.msg.iov_count = 1;
	}
	ret = domain->dgram.sendmsg_ev ?
		      fi_sendmsg_ev(domain->dgram.ep, &msg, 0) :
		      fi_sendmsg(domain->dgram.ep, &msg.msg, 0);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"error sending packet: %d (%s)\n", (int) ret,
			fi_strerror((int) -ret));
		return ret;
	}
	entry->in_use = true;
	return 0;
}

static ssize_t suet_dgram_ep_recv_pkt(struct suet_domain *domain)
{
	struct suet_dgram_pkt_entry *entry;
	ssize_t ret;

	entry = ofi_buf_alloc(domain->dgram.rx_pkt_entry_pool);
	if (!entry)
		return -FI_ENOMEM;
	entry->in_use = false;
	dlist_init(&entry->entry);
	ret = fi_recv(domain->dgram.ep,
		      (char *) entry->pkt.pkt - domain->dgram.rx_prefix_size,
		      domain->dgram.max_mtu_sz, entry->desc, FI_ADDR_UNSPEC,
		      &entry->context);
	if (ret) {
		suet_dgram_pkt_free(&entry->pkt);
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "failed to repost\n");
		return ret;
	}
	entry->in_use = true;
	dlist_insert_tail(&entry->entry, &domain->dgram.rx_pkt_list);
	return 0;
}

/* CQ formats and provider contexts remain entirely within datagram. */
static void suet_dgram_tx_complete(struct suet_domain *domain, void *context,
				   int status)
{
	struct suet_dgram_pkt_entry *entry =
		container_of(context, struct suet_dgram_pkt_entry, context);

	entry->in_use = false;
	suet_pds_tx_done(domain, &entry->pkt, status);
}

static void suet_dgram_rx_complete(struct suet_domain *domain,
				   struct fi_cq_data_entry *comp,
				   fi_addr_t src_addr)
{
	struct suet_dgram_pkt_entry *entry = container_of(
		comp->op_context, struct suet_dgram_pkt_entry, context);
	uint64_t flags = comp->flags & domain->dgram.rx_metadata;

	entry->in_use = false;
	dlist_remove_init(&entry->entry);
	if (comp->len < domain->dgram.rx_prefix_size) {
		suet_dgram_pkt_free(&entry->pkt);
		return;
	}
	entry->pkt.rx_flags = 0;
	entry->pkt.ev = 0;
	if (flags & FI_SUET_DGRAM_EV)
		entry->pkt.ev = (uint16_t) comp->data;
	if (flags & FI_SUET_DGRAM_ECN)
		entry->pkt.rx_flags |= SUET_DGRAM_RX_ECN;
	if (flags & FI_SUET_DGRAM_TRIMMED)
		entry->pkt.rx_flags |= SUET_DGRAM_RX_TRIMMED;
	if (flags & FI_SUET_DGRAM_TRIMMED_LASTHOP)
		entry->pkt.rx_flags |=
			SUET_DGRAM_RX_TRIMMED | SUET_DGRAM_RX_TRIMMED_LASTHOP;
	entry->pkt.pkt_size = comp->len - domain->dgram.rx_prefix_size;
	suet_pds_receive(domain, &entry->pkt, src_addr);
}

/*
 * Handle an RX CQ error completion. Returns 0 if the error was
 * resolved via AV-fixup and the caller should proceed to process
 * cq_entry/dgram_av_addr as a normal RX completion; negative otherwise.
 */
static int suet_dgram_rx_cq_handle_error(struct suet_domain *domain,
					 struct fi_cq_data_entry *cq_entry,
					 fi_addr_t *dgram_av_addr)
{
	struct fi_cq_err_entry err = {0};
	struct suet_dgram_pkt_entry *entry;
	ssize_t ret;

	ret = fi_cq_readerr(domain->dgram.rx_cq, &err, 0);
	if (ret < 0) {
		FI_WARN(&suet_prov, FI_LOG_CQ, "fi_cq_readerr(rx) error: %s\n",
			fi_strerror((int) -ret));
		return ret;
	}

	entry = container_of(err.op_context, struct suet_dgram_pkt_entry,
			     context);

	if (err.err == FI_EADDRNOTAVAIL && err.err_data &&
	    err.err_data_size > 0 &&
	    !suet_dgram_av_handle_addr_notavail(domain, &err, cq_entry,
						dgram_av_addr)) {
		return 0;
	}

	domain->counters.rx_cq_errors++;
	FI_WARN(&suet_prov, FI_LOG_CQ,
		"Non-recoverable RX CQ error: %s (rx_cq_errors=%lu)\n",
		fi_strerror(-err.err),
		(unsigned long) domain->counters.rx_cq_errors);

	if (suet_env.max_rx_cq_errors > 0 &&
	    domain->counters.rx_cq_errors >=
		    (uint64_t) suet_env.max_rx_cq_errors) {
		int written;

		FI_WARN(&suet_prov, FI_LOG_CQ,
			"RX CQ error count %lu reached threshold %d, "
			"surfacing to bound EPs\n",
			(unsigned long) domain->counters.rx_cq_errors,
			suet_env.max_rx_cq_errors);
		written = suet_domain_broadcast_cq_err(domain, (int) -err.err,
						       false);
		if (!written) {
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"fatal: no bound EPs to surface RX CQ error "
				"to, "
				"aborting\n");
			abort();
		}
		/* Surfaced; reset counter so we don't re-broadcast on every
		 * subsequent CQE. The threshold becomes the broadcast cadence.
		 */
		domain->counters.rx_cq_errors = 0;
	}

	entry->in_use = false;
	dlist_remove_init(&entry->entry);
	suet_dgram_pkt_free(&entry->pkt);
	suet_dgram_ep_recv_pkt(domain);
	return -FI_EAVAIL;
}

static void suet_dgram_tx_cq_handle_error(struct suet_domain *domain)
{
	struct fi_cq_err_entry err = {0};
	ssize_t ret;

	ret = fi_cq_readerr(domain->dgram.tx_cq, &err, 0);
	if (ret < 0) {
		FI_WARN(&suet_prov, FI_LOG_CQ, "fi_cq_readerr(tx) error: %s\n",
			fi_strerror((int) -ret));
		return;
	}

	domain->counters.tx_cq_errors++;
	FI_WARN(&suet_prov, FI_LOG_CQ,
		"Received FI_SEND error from core provider: %s "
		"(tx_cq_errors=%lu)\n",
		fi_strerror(-err.err),
		(unsigned long) domain->counters.tx_cq_errors);

	if (suet_env.max_tx_cq_errors > 0 &&
	    domain->counters.tx_cq_errors >=
		    (uint64_t) suet_env.max_tx_cq_errors) {
		int written;

		FI_WARN(&suet_prov, FI_LOG_CQ,
			"TX CQ error count %lu reached threshold %d, "
			"surfacing to bound EPs\n",
			(unsigned long) domain->counters.tx_cq_errors,
			suet_env.max_tx_cq_errors);
		written = suet_domain_broadcast_cq_err(domain, (int) -err.err,
						       true);
		if (!written) {
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"fatal: no bound EPs to surface TX CQ error "
				"to, "
				"aborting\n");
			abort();
		}
		domain->counters.tx_cq_errors = 0;
	}

	/* PDS retains its retry policy for local send errors. */
	suet_dgram_tx_complete(domain, err.op_context, -err.err);
}

/*
 * Poll the TX CQ once and dispatch up to suet_env.cq_read_batch_size
 * completions. Returns the number of entries processed, 0 if none.
 */
static int suet_dgram_tx_cq_poll(struct suet_domain *domain)
{
	ssize_t ret;
	int i;

	ret = fi_cq_read(domain->dgram.tx_cq, domain->dgram.tx_cq_entries,
			 domain->dgram.cq_read_batch_size);
	if (ret < 0) {
		if (ret == -FI_EAVAIL)
			suet_dgram_tx_cq_handle_error(domain);
		else if (ret != -FI_EAGAIN)
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"fi_cq_read(tx) error: %s\n",
				fi_strerror((int) -ret));
		return 0;
	}

	for (i = 0; i < ret; i++)
		suet_dgram_tx_complete(
			domain, domain->dgram.tx_cq_entries[i].op_context, 0);
	return (int) ret;
}

/*
 * Poll the RX CQ once and dispatch up to suet_env.cq_read_batch_size
 * completions. RX needs the source FEP datagram address (dgram_av_addr) to
 * demultiplex incoming packets to the right FEP. Returns the
 * number of entries processed, 0 if none.
 */
static int suet_dgram_rx_cq_poll(struct suet_domain *domain)
{
	ssize_t ret;
	int i;
	struct fi_cq_data_entry comp = {0};
	bool ev = domain->dgram.rx_metadata & FI_SUET_DGRAM_EV;

	ret = fi_cq_readfrom(domain->dgram.rx_cq, &domain->dgram.rx_cq_entries,
			     domain->dgram.cq_read_batch_size,
			     domain->dgram.rx_cq_addrs);
	if (ret < 0) {
		if (ret == -FI_EAVAIL) {
			/* Error applies to a single completion; recover or
			 * bail. On recovery the handler populates
			 * comp/rx_cq_addrs[0] with the real
			 * entry. */
			if (suet_dgram_rx_cq_handle_error(
				    domain, &comp,
				    &domain->dgram.rx_cq_addrs[0]) < 0)
				return 0;
			suet_dgram_ep_recv_pkt(domain);
			suet_dgram_rx_complete(domain, &comp,
					       domain->dgram.rx_cq_addrs[0]);
			return 1;
		}
		if (ret != -FI_EAGAIN)
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"fi_cq_readfrom(rx) error: %s\n",
				fi_strerror((int) -ret));
		return 0;
	}

	for (i = 0; i < ret; i++)
		suet_dgram_ep_recv_pkt(domain);
	for (i = 0; i < ret; i++) {
		if (ev) {
			comp = domain->dgram.rx_cq_entries.data[i];
		} else {
			comp.op_context =
				domain->dgram.rx_cq_entries.msg[i].op_context;
			comp.flags = domain->dgram.rx_cq_entries.msg[i].flags;
			comp.len = domain->dgram.rx_cq_entries.msg[i].len;
		}
		suet_dgram_rx_complete(domain, &comp,
				       domain->dgram.rx_cq_addrs[i]);
	}

	return (int) ret;
}

void suet_dgram_progress(struct suet_domain *domain)
{
	int tx_got, rx_got;
	int i;

	for (i = 0; !suet_env.spin_count || i < suet_env.spin_count; i++) {
		tx_got = suet_dgram_tx_cq_poll(domain);
		rx_got = suet_dgram_rx_cq_poll(domain);
		if (!tx_got && !rx_got) {
			break;
		}
	}
}

int suet_dgram_stop(struct suet_domain *domain)
{
	int ret;

	if (!domain->dgram.ep)
		return 0;
	ret = fi_close(&domain->dgram.ep->fid);
	if (ret)
		return ret;
	domain->dgram.ep = NULL;
	return 0;
}

int suet_dgram_cleanup(struct suet_domain *domain)
{
	struct suet_dgram_pkt_entry *entry;
	int ret;

	ret = suet_dgram_stop(domain);
	if (ret)
		return ret;
	while (!dlist_empty(&domain->dgram.rx_pkt_list)) {
		dlist_pop_front(&domain->dgram.rx_pkt_list,
				struct suet_dgram_pkt_entry, entry, entry);
		dlist_init(&entry->entry);
		entry->in_use = false;
		suet_dgram_pkt_free(&entry->pkt);
	}
	suet_dgram_free_pkt_entry_pools(domain);
	if (domain->dgram.av)
		suet_dgram_av_cleanup(domain);
	if (domain->dgram.rx_cq) {
		ret = fi_close(&domain->dgram.rx_cq->fid);
		if (ret)
			return ret;
		domain->dgram.rx_cq = NULL;
	}
	if (domain->dgram.tx_cq) {
		ret = fi_close(&domain->dgram.tx_cq->fid);
		if (ret)
			return ret;
		domain->dgram.tx_cq = NULL;
	}
	if (domain->dgram.domain) {
		ret = fi_close(&domain->dgram.domain->fid);
		if (ret)
			return ret;
		domain->dgram.domain = NULL;
	}
	return 0;
}

int suet_dgram_init(struct suet_domain *suet_domain, struct fid_fabric *fabric,
		    struct fi_info *info, void *context, size_t mtu_limit,
		    size_t pds_pkt_size, size_t ses_pkt_size)
{
	struct suet_fabric *suet_fabric = container_of(
		fabric, struct suet_fabric, util_fabric.fabric_fid);
	struct fi_info *dgram_info = NULL;
	struct fi_cq_attr cq_attr = {0};
	size_t min_cq_size;
	int ret, i;

	suet_domain->dgram.pds_pkt_size = pds_pkt_size;
	suet_domain->dgram.ses_pkt_size = ses_pkt_size;
	dlist_init(&suet_domain->dgram.rx_pkt_list);
	ret = suet_dgram_get_core_info(fabric->api_version, info, &dgram_info);

	if (ret)
		goto err;

	ret = fi_domain(suet_fabric->dgram_ctx, dgram_info,
			&suet_domain->dgram.domain, context);
	if (ret)
		goto err;

	suet_domain->dgram.sendmsg_ev = !!(dgram_info->caps & FI_SENDMSG_EV);
	suet_domain->dgram.rx_metadata = 0;
	if (fi_get_val(&suet_domain->dgram.domain->fid,
		       FI_SUET_DGRAM_RX_METADATA,
		       &suet_domain->dgram.rx_metadata))
		suet_domain->dgram.rx_metadata = 0;
	suet_domain->dgram.rx_metadata &= FI_SUET_DGRAM_METADATA_MASK;

	suet_domain->dgram.max_mtu_sz =
		MIN(dgram_info->ep_attr->max_msg_size, mtu_limit);
	suet_domain->dgram.max_pkt_size = suet_domain->dgram.max_mtu_sz -
					  dgram_info->ep_attr->msg_prefix_size;

	/* Shared FEP: create the datagram endpoint on the domain */
	suet_domain->dgram.do_local_mr = ofi_mr_local(dgram_info);
	suet_domain->dgram.tx_prefix_size =
		dgram_info->tx_attr->mode & FI_MSG_PREFIX ?
			dgram_info->ep_attr->msg_prefix_size :
			0;
	suet_domain->dgram.rx_prefix_size =
		dgram_info->rx_attr->mode & FI_MSG_PREFIX ?
			dgram_info->ep_attr->msg_prefix_size :
			0;

	ret = fi_endpoint(suet_domain->dgram.domain, dgram_info,
			  &suet_domain->dgram.ep, suet_domain);
	if (ret)
		goto err;

	ret = suet_dgram_av_init(suet_domain);
	if (ret)
		goto err;

	ret = fi_ep_bind(suet_domain->dgram.ep, &suet_domain->dgram.av->fid, 0);
	if (ret)
		goto err;

	cq_attr.format = FI_CQ_FORMAT_MSG;
	cq_attr.wait_obj = FI_WAIT_NONE;

	cq_attr.size = dgram_info->tx_attr->size;
	ret = fi_cq_open(suet_domain->dgram.domain, &cq_attr,
			 &suet_domain->dgram.tx_cq, suet_domain);
	if (ret)
		goto err;

	ret = fi_ep_bind(suet_domain->dgram.ep, &suet_domain->dgram.tx_cq->fid,
			 FI_TRANSMIT);
	if (ret)
		goto err;

	if (suet_domain->dgram.rx_metadata & FI_SUET_DGRAM_EV)
		cq_attr.format = FI_CQ_FORMAT_DATA;
	cq_attr.size = dgram_info->rx_attr->size;
	ret = fi_cq_open(suet_domain->dgram.domain, &cq_attr,
			 &suet_domain->dgram.rx_cq, suet_domain);
	if (ret)
		goto err;

	ret = fi_ep_bind(suet_domain->dgram.ep, &suet_domain->dgram.rx_cq->fid,
			 FI_RECV);
	if (ret)
		goto err;

	suet_domain->dgram.cq_read_batch_size = suet_env.cq_read_batch_size;
	min_cq_size = MIN(dgram_info->tx_attr->size, dgram_info->rx_attr->size);
	if ((size_t) suet_domain->dgram.cq_read_batch_size > min_cq_size) {
		FI_INFO(&suet_prov, FI_LOG_DOMAIN,
			"clamping cq_read_batch_size from %d to available CQ "
			"size "
			"%zu\n",
			suet_domain->dgram.cq_read_batch_size, min_cq_size);
		suet_domain->dgram.cq_read_batch_size = (int) min_cq_size;
	}

	ret = fi_enable(suet_domain->dgram.ep);
	if (ret)
		goto err;

	ret = suet_dgram_init_pkt_entry_pools(suet_domain);
	if (ret)
		goto err;

	for (i = 0; i < dgram_info->rx_attr->size; i++)
		if (suet_dgram_ep_recv_pkt(suet_domain))
			break;
	suet_domain->dgram.mr_key = 0;
	fi_freeinfo(dgram_info);
	return 0;
err:
	if (suet_dgram_cleanup(suet_domain))
		FI_WARN(&suet_prov, FI_LOG_DOMAIN, "datagram cleanup failed\n");
	fi_freeinfo(dgram_info);
	return ret;
}
