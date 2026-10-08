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

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <pthread.h>
#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_trigger.h>
#include <stdbool.h>

#include "suet_proto.h"
#include <ofi.h>
#include <ofi_atomic.h>
#include <ofi_enosys.h>
#include <ofi_indexer.h>
#include <ofi_iov.h>
#include <ofi_list.h>
#include <ofi_proto.h>
#include <ofi_rbuf.h>
#include <ofi_tree.h>
#include <ofi_util.h>

#ifndef _SUET_H_
#define _SUET_H_

#include "suet_dgram.h"
#include "suet_ext.h"
#include "suet_pds.h"
#include "suet_ses.h"

#define SUET_MAX_TX_BITS 10
#define SUET_MAX_RX_BITS 10

#define SUET_BUF_POOL_ALIGNMENT 16
#define SUET_MAX_PENDING	128
#define SUET_ADDR_INVALID	0

#define SUET_REMOTE_CQ_DATA	(1 << 0)
#define SUET_NO_TX_COMP		(1 << 1)
#define SUET_NO_RX_COMP		(1 << 2)
#define SUET_INJECT		(1 << 3)
#define SUET_TAG_HDR		(1 << 4)
#define SUET_MULTI_RECV		(1 << 6)

#define SUET_IDX_OFFSET(x) (x + 1)

struct suet_env {
	int spin_count;
	int max_peers;
	int max_unacked;
	int max_eps;
	int rescan;
	int print_domain_counters;
	int max_tx_cq_errors;
	int max_rx_cq_errors;
	int cq_read_batch_size;
	int max_pkt_retry;
	int selective_repeat;
	int max_gtd_del_resp_pool_size;
	int unexp_msg_pool_size;
	size_t zc_mr_reg_threshold;
};

extern struct suet_env suet_env;
extern struct fi_provider suet_prov;
extern struct fi_info suet_info;
extern struct fi_fabric_attr suet_fabric_attr;
extern struct util_prov suet_util_prov;
extern struct fi_ops_msg suet_ops_msg;
extern struct fi_ops_tagged suet_ops_tagged;
extern struct fi_ops_rma suet_ops_rma;
extern struct fi_ops_atomic suet_ops_atomic;
extern struct fi_ops_mr suet_domain_mr_ops;

struct suet_mr {
	struct fid_mr mr_fid;
	void *dgram_ctx; /* datagram registration handle */
	struct suet_domain *domain;
	ofi_mutex_t amo_lock;
};

int suet_mr_regv_internal(struct suet_domain *domain, const struct iovec *iov,
			  size_t count, size_t reg_limit, uint64_t access,
			  void **mr, void **desc);
void suet_mr_closev_internal(void **mr, size_t count);
void *suet_mr_desc(void *desc);

/*
 * Provider architecture:
 *
 * Fabric represents all FEPs associated with a physical NIC port which is
 * exposed through DGRAM provider. Fabric supports unreliable out-of-order
 * datagram service, such as Ethernet, Ethernet/IP/UDP or Verbs UD.
 *
 * Single provider domain represents a UET Fabric endpoint (FEP). It is
 * associated with DGRAM provider domain-endpoint pair, through which traffic of
 * all endpoints (Resource Indices (RIs)) is multiplexed.
 *
 * Endpoints represent RIs that expose to the user data transfer operations with
 * UET opcodes.
 *
 * Bandwidth scaling is achieved through parallelism of DGRAM domains, e.g.,
 * each domain is associated with a UDP socket (port 4793 per UET spec, or
 * OS-assigned in the current implementation) bound to an FEP IPv4/v6 address.
 * Each thread manages its own domain(s) and endpoints on it. For the best
 * performance, endpoints on the same domain are expected to be served by the
 * same thread to avoid domain lock contention.
 */

struct suet_fabric { /* UET NIC */
	struct util_fabric util_fabric;
	void *dgram_ctx; /* datagram fabric handle */
};

struct suet_domain; /* UET Fabric endpoint (FEP) */
struct suet_ep; /* UET Resource Index (RI) */

struct suet_domain_perfconters {
	uint64_t nacks_tx;
	uint64_t nacks_rx;
	uint64_t stale_acks_rx;
	uint64_t error_acks_rx;
	uint64_t tx_cq_errors;
	uint64_t rx_cq_errors;
	uint64_t pdc_closes_ok;
	uint64_t pdc_close_in_err;
	uint64_t pdc_closes_rx;
	uint64_t dup_drops;
	uint64_t ses_err_completions;
};

struct suet_domain {
	struct util_domain util_domain;

	uint16_t next_ri;
	uint16_t pid_on_fep;
	uint32_t ep_counter;
	struct ofi_genlock fep_lock;
	struct fi_suet_clock clock;
	struct suet_ep **ep_table;

	size_t zc_mr_reg_threshold;
	ssize_t max_inline_msg;
	ssize_t max_inline_rma;
	ssize_t max_inline_atom;
	ssize_t max_pkt_sz;

	struct suet_dgram_resources dgram;
	struct suet_pds_resources pds;
	struct suet_ses_resources ses;

	struct suet_domain_perfconters counters;
};

struct suet_av {
	struct util_av util_av;
	struct indexer usr_av_addr_to_peer_idx;
	struct index_map peer_idx_to_usr_av_addr;
};

/*
 * Temporarily unwrap suet_av_addr addresses in an fi_info so the core
 * datagram provider sees its raw addresses.  Call
 * suet_av_info_wrap_raw_dgram_addrs() afterwards to put the original pointers
 * back. These are suet-internal AV-wrapping helpers; they don't belong in
 * suet_proto.h with the wire formats.
 */
struct suet_av_addr_tmp_storage {
	void *src_addr;
	size_t src_addrlen;
	void *dest_addr;
	size_t dest_addrlen;
};

static inline void
suet_av_info_unwrap_raw_dgram_addrs(struct fi_info *info,
				    struct suet_av_addr_tmp_storage *save)
{
	save->src_addr = info->src_addr;
	save->src_addrlen = info->src_addrlen;
	save->dest_addr = info->dest_addr;
	save->dest_addrlen = info->dest_addrlen;

	if (info->src_addr &&
	    info->src_addrlen >= sizeof(struct suet_av_addr)) {
		struct suet_av_addr *suet_a = info->src_addr;
		info->src_addr = suet_a->raw_dgram_addr;
		info->src_addrlen = suet_a->raw_dgram_addrlen;
	}
	if (info->dest_addr &&
	    info->dest_addrlen >= sizeof(struct suet_av_addr)) {
		struct suet_av_addr *suet_a = info->dest_addr;
		info->dest_addr = suet_a->raw_dgram_addr;
		info->dest_addrlen = suet_a->raw_dgram_addrlen;
	}
}

static inline void
suet_av_info_wrap_raw_dgram_addrs(struct fi_info *info,
				  const struct suet_av_addr_tmp_storage *save)
{
	info->src_addr = save->src_addr;
	info->src_addrlen = save->src_addrlen;
	info->dest_addr = save->dest_addr;
	info->dest_addrlen = save->dest_addrlen;
}

int suet_av_peer_idx_from_usr_av_addr(struct suet_av *av, fi_addr_t addr);

struct suet_cq;
typedef int (*suet_cq_write_fn)(struct suet_cq *cq,
				struct fi_cq_tagged_entry *cq_entry);
struct suet_cq {
	struct util_cq util_cq;
	suet_cq_write_fn write_fn;
};

struct suet_ep {
	enum suet_pdc_type pdc_type;
	struct util_ep util_ep;

	uint16_t resource_index; /* assigned RI for this EP (12-bit) */
	uint16_t pid_on_fep; /* PIDonFEP from domain */

	size_t rx_size;
	size_t tx_size;
	size_t min_multi_recv_size;
	uint32_t tx_flags;
	uint32_t rx_flags;

	struct dlist_entry tx_list;
	struct dlist_entry unexp_list;
	struct dlist_entry unexp_tag_list;
	struct dlist_entry rx_list;
	struct dlist_entry rx_tag_list;
};

static inline struct suet_domain *suet_ep_domain(struct suet_ep *ep)
{
	return container_of(ep->util_ep.domain, struct suet_domain,
			    util_domain);
}

static inline struct suet_av *suet_ep_av(struct suet_ep *ep)
{
	return container_of(ep->util_ep.av, struct suet_av, util_av);
}

static inline struct suet_cq *suet_ep_tx_cq(struct suet_ep *ep)
{
	return container_of(ep->util_ep.tx_cq, struct suet_cq, util_cq);
}

static inline struct suet_cq *suet_ep_rx_cq(struct suet_ep *ep)
{
	return container_of(ep->util_ep.rx_cq, struct suet_cq, util_cq);
}

/* SUET doesn't advertise FI_MR_LOCAL, so desc is optional; but if the
 * user does provide it, libfabric semantics treat it as a per-iov
 * mapping and partial coverage is not defined. We require the vector
 * to be uniform: either every slot non-NULL (honor user MRs) or every
 * slot NULL / desc==NULL (provider decides). Mixed results in -FI_EINVAL.
 *
 * On return, *out is either NULL (treat as no user desc) or the
 * original desc (all slots valid). */
static inline int suet_normalize_mr_desc(void **desc, size_t iov_count,
					 void ***out)
{
	size_t i, n_set = 0;

	if (!desc) {
		*out = NULL;
		return 0;
	}
	for (i = 0; i < iov_count; i++)
		if (desc[i])
			n_set++;
	if (n_set == 0) {
		*out = NULL;
		return 0;
	}
	if (n_set != iov_count)
		return -FI_EINVAL;
	*out = desc;
	return 0;
}

static inline uint32_t suet_ep_tx_flags(uint64_t fi_flags)
{
	uint32_t suet_flags = 0;

	if (fi_flags & FI_REMOTE_CQ_DATA)
		suet_flags |= SUET_REMOTE_CQ_DATA;
	if (fi_flags & FI_INJECT)
		suet_flags |= SUET_INJECT;
	if (fi_flags & FI_COMPLETION)
		return suet_flags;

	return suet_flags | SUET_NO_TX_COMP;
}

static inline uint32_t suet_ep_rx_flags(uint64_t fi_flags)
{
	uint32_t suet_flags = 0;

	if (fi_flags & FI_MULTI_RECV)
		suet_flags |= SUET_MULTI_RECV;
	if (fi_flags & FI_COMPLETION)
		return suet_flags;

	return suet_flags | SUET_NO_RX_COMP;
}

void suet_ses_unexp_msg_list_cleanup(struct dlist_entry *list);

void suet_info_from_dgram(struct fi_info *info, uint64_t caps,
			  uint64_t domain_caps, size_t max_mtu,
			  size_t prefix_size);
int suet_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
		void *context);

int suet_av_create(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		   struct fid_av **av, void *context);

int suet_query_atomic(struct fid_domain *domain, enum fi_datatype datatype,
		      enum fi_op op, struct fi_atomic_attr *attr,
		      uint64_t flags);

int suet_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **dom, void *context);
uint64_t suet_domain_now_ms(struct suet_domain *domain);
void suet_domain_progress(struct suet_domain *domain);
int suet_domain_broadcast_cq_err(struct suet_domain *domain, int err,
				 bool is_tx);

int suet_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		 struct fid_cq **cq_fid, void *context);
int suet_cntr_open(struct fid_domain *domain, struct fi_cntr_attr *attr,
		   struct fid_cntr **cntr_fid, void *context);

int suet_pds_init(struct suet_domain *domain);
void suet_pds_cleanup(struct suet_domain *domain);
void suet_pds_progress(struct suet_domain *domain);
int suet_pds_drain(struct suet_domain *domain, bool blocking);

int suet_ses_init(struct suet_domain *domain);
void suet_ses_cleanup(struct suet_domain *domain);
void suet_ses_ep_cleanup(struct suet_ep *ep);
int suet_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct fid_ep **ep, void *context);
ssize_t suet_ep_generic_recvmsg(struct suet_ep *suet_ep,
				const struct iovec *iov, size_t iov_count,
				fi_addr_t addr, uint64_t tag, uint64_t ignore,
				void *context, uint32_t op, uint32_t suet_flags,
				uint64_t flags);
ssize_t suet_ep_generic_sendmsg(struct suet_ep *suet_ep,
				const struct iovec *iov, void **desc,
				size_t iov_count, fi_addr_t addr, uint64_t tag,
				uint64_t data, void *context, uint32_t op,
				uint32_t suet_flags);
ssize_t suet_ep_generic_inject(struct suet_ep *suet_ep, const struct iovec *iov,
			       size_t iov_count, fi_addr_t addr, uint64_t tag,
			       uint64_t data, uint32_t op, uint32_t suet_flags);
#endif