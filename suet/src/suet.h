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

#define SUET_MAX_TX_BITS 10
#define SUET_MAX_RX_BITS 10

#define SUET_CQ_MAX_BATCH	64
#define SUET_MR_KEY_MAX_RETRIES 1024
#define SUET_IOV_LIMIT		4
#define SUET_BUF_POOL_ALIGNMENT 16
#define SUET_TX_POOL_CHUNK_CNT	1024
#define SUET_RX_POOL_CHUNK_CNT	1024
#define SUET_MAX_PENDING	128
#define SUET_ADDR_INVALID	0

#define SUET_PKT_IN_USE (1 << 0)
#define SUET_PKT_ACKED	(1 << 1)

#define SUET_REMOTE_CQ_DATA	(1 << 0)
#define SUET_NO_TX_COMP		(1 << 1)
#define SUET_NO_RX_COMP		(1 << 2)
#define SUET_INJECT		(1 << 3)
#define SUET_TAG_HDR		(1 << 4)
#define SUET_MULTI_RECV		(1 << 6)
#define SUET_TX_ENTRY_PDS_OWNED (1 << 7)

#define SUET_IDX_OFFSET(x) (x + 1)

struct suet_env {
	int spin_count;
	int retry;
	int max_peers;
	int max_unacked;
	int max_eps;
	int rescan;
	int print_domain_counters;
	int max_tx_cq_errors;
	int max_rx_cq_errors;
	int cq_read_batch_size;
	int max_pkt_retry;
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
	struct fid_mr *dg_mr;
	struct suet_domain *domain;
	ofi_mutex_t amo_lock;
};

int suet_mr_reg_impl(struct suet_domain *domain, struct fi_mr_attr *attr,
		     uint64_t flags, bool gen_key, struct fid_mr **mr);
int suet_mr_regv_internal(struct suet_domain *domain, const struct iovec *iov,
			  size_t count, size_t reg_limit, uint64_t access,
			  struct fid_mr **mr);
void suet_mr_closev_internal(struct fid_mr **mr, size_t count);

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
	struct fid_fabric *dg_fabric;
};

struct suet_domain; /* UET Fabric endpoint (FEP) */
struct suet_ep; /* UET Resource Index (RI) */

enum suet_pool_type {
	SUET_BUF_POOL_RX,
	SUET_BUF_POOL_TX,
};

struct suet_buf_pool {
	enum suet_pool_type type;
	struct ofi_bufpool *pool;
	struct suet_domain *suet_domain;
};

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

	uint16_t pid_on_fep;
	struct fid_domain *fep_domain;
	struct fid_ep *dg_ep;
	struct fid_cq *dg_tx_cq;
	struct fid_cq *dg_rx_cq;
	int cq_read_batch_size;
	struct fi_cq_msg_entry tx_cq_entries[SUET_CQ_MAX_BATCH];
	struct fi_cq_msg_entry rx_cq_entries[SUET_CQ_MAX_BATCH];
	fi_addr_t cq_rx_dg_av_addrs[SUET_CQ_MAX_BATCH];
	struct ofi_genlock fep_lock;

	struct fid_av *dg_av;
	struct ofi_genlock av_lock;
	struct ofi_rbmap rbmap;
	struct indexer peer_idx_to_dg_av_addr;
	size_t dg_addrlen;

	size_t tx_prefix_size;
	size_t rx_prefix_size;
	int do_local_mr;
	size_t zc_mr_reg_threshold;
	uint64_t mr_key;

	struct suet_buf_pool *tx_pkt_entry_pool;
	struct suet_buf_pool *rx_pkt_entry_pool;
	struct dlist_entry rx_pkt_list;

	struct suet_buf_pool tx_entry_pool;
	struct suet_buf_pool rx_entry_pool;

	ssize_t max_mtu_sz;
	ssize_t max_inline_msg;
	ssize_t max_inline_rma;
	ssize_t max_inline_atom;
	ssize_t max_seg_sz;

	uint32_t ep_counter;
	uint16_t next_ri;
	struct suet_ep **ep_table;

	uint16_t next_pdcid;
	uint32_t psn_seed;
	struct ofi_bufpool *ipdc_pool;
	struct ofi_bufpool *tpdc_pool;
	struct ofi_bufpool *gtd_del_resp_pool;
	struct ofi_bufpool *unexp_msg_pool;
	struct dlist_entry active_ipdc_list;
	struct dlist_entry active_tpdc_list;
	struct index_map local_pdcid_to_ipdc_idm;
	struct index_map local_pdcid_to_tpdc_idm;
	struct suet_ipdc *ipdc_by_dg_av_addr_ht;
	struct suet_tpdc *tpdc_by_syn_key_ht;

	struct suet_domain_perfconters counters;
};

static inline uint16_t suet_domain_get_pid_on_fep()
{
	return (uint16_t) (getpid() & SUET_PID_ON_FEP_MASK);
}

static inline uint16_t suet_domain_allocate_pdcid(struct suet_domain *domain)
{
	uint16_t pdcid = domain->next_pdcid;

	/* Spec 3.5.8.2: "PDCID=0 is reserved and MUST not be used by a PDC." */
	if (++domain->next_pdcid == 0)
		domain->next_pdcid = 1;
	return pdcid;
}

/*
 * Spec-compatible PDC state (Section 3.5.8.2 / 3.5.8.3 / Figure 3-43, 3-44).
 *
 * Minimal subset without TSS and target-initiated close.
 * The initiator drives PDC lifecycle;
 * the target only ever observes ESTABLISHED -> CLOSED on receipt of a Close
 * Command CP.
 */
enum suet_pdc_state {
	SUET_PDC_OPENING, /* initiator: SYN attached to first request, no ACK
			     yet */
	SUET_PDC_ESTABLISHED, /* normal data flow */
	SUET_PDC_QUIESCE, /* initiator: closing requested, draining tx/in-flight
			   */
	SUET_PDC_CLOSE_ACK_WAIT, /* initiator: Close Cmd CP sent, awaiting Close
				    ACK */
	SUET_PDC_CLOSED, /* terminal, free pending */
};

/*
 * Initiator-side PDC - created on first send to a remote FEP.
 * Tracks TX reliability state: PSN sequence, in_flight packets, tx_list.
 * Currently, only one per remote FEP, shared by all EPs on this domain.
 */
struct suet_ipdc {
	struct dlist_entry entry;
	UT_hash_handle
		ipdc_dg_av_addr_handle; /* handle for ipdc_by_dg_av_addr_ht */
	fi_addr_t dg_av_addr; /* DG-layer address of remote (uthash key) */
	uint16_t local_pdcid;
	uint16_t tpdcid; /* learned from first ACK.spdcid */
	uint32_t start_psn;
	uint32_t tx_seq_no;
	uint32_t last_rx_cack_psn; /* last PSN acked by target (cack_psn) */
	uint32_t last_tx_clear_psn; /* CLEAR_PSN this initiator transmits
				     * Eager-clear: advanced with CACK_PSN when
				     * the SES response has been delivered. */
	uint32_t close_psn;
	int retry_cnt;
	uint16_t in_flight_cnt;
	bool teardown_pending; /* close requested; defer QUIESCE until
				  ESTABLISHED */
	enum suet_pdc_state state;
	struct dlist_entry tx_list; /* tx_entries pending send */
	struct dlist_entry
		in_flight_pkts; /* sent, awaiting NIC completion + remote ACK */
};

/*
 * Target-side PDC — created on first SYN from a remote FEP.
 * Tracks RX reliability state: expected PSN, OOO buffer.
 * Currently, only one per remote FEP, shared by all EPs on this domain.
 */
struct suet_tpdc {
	struct dlist_entry entry;
	UT_hash_handle tpdc_syn_key_handle; /* handle for tpdc_by_syn_key_ht */
	/* --- tpdc_syn_key_handle: key start --- */
	fi_addr_t dg_av_addr; /* DG-layer address of remote */
	uint16_t ipdcid; /* from SYN.spdcid */
	/* --- tpdc_syn_key_handle: key end --- */
	uint16_t local_pdcid;
	int peer_idx; /* AV peer index for directed-recv matching */
	uint32_t expected_rx_psn; /* next expected PSN */
	uint32_t last_tx_cack_psn; /* last cack_psn we sent (= expected_rx_psn-1
				    * at the moment the ACK was emitted) */
	uint32_t last_rx_clear_psn; /* highest CLEAR_PSN received in
				     * forward direction */
	uint32_t pkts_since_last_ack; /* requests delivered since last ACK sent;
				       * drives ACK coalescing */
	enum suet_pdc_state state;
	uint16_t curr_rx_id; /* current in-progress rx_entry id */
	struct suet_unexp_msg
		*curr_unexp; /* current unexpected multi-seg msg */
	struct dlist_entry rx_list; /* in-progress rx_entries */
	struct dlist_entry rma_rx_list; /* in-progress rma/atomic rx_entries */
	struct dlist_entry ooo_pkts;
	struct dlist_entry
		gtd_del_list; /* saved SES responses awaiting CLEAR_PSN;
			       * entries are ordered by ascending PSN. */
};

struct suet_ut_tpdc_syn_key {
	fi_addr_t dg_av_addr;
	uint16_t ipdcid;
} __attribute__((packed));

static inline struct suet_ipdc *
suet_pds_ipdc_get_by_local_pdcid(struct suet_domain *domain, uint16_t lpdcid)
{
	return ofi_idm_lookup(&domain->local_pdcid_to_ipdc_idm, (int) lpdcid);
}

static inline struct suet_tpdc *
suet_pds_tpdc_get_by_local_pdcid(struct suet_domain *domain, uint16_t lpdcid)
{
	return ofi_idm_lookup(&domain->local_pdcid_to_tpdc_idm, (int) lpdcid);
}

static inline struct suet_ipdc *
suet_pds_ipdc_get_by_dg_av_addr(struct suet_domain *domain,
				fi_addr_t dg_av_addr)
{
	struct suet_ipdc *ipdc = NULL;

	HASH_FIND(ipdc_dg_av_addr_handle, domain->ipdc_by_dg_av_addr_ht,
		  &dg_av_addr, sizeof(dg_av_addr), ipdc);
	return ipdc;
}

static inline struct suet_tpdc *
suet_pds_tpdc_get_by_syn_key(struct suet_domain *domain, fi_addr_t dg_av_addr,
			     uint16_t ipdcid)
{
	struct suet_ut_tpdc_syn_key key = {dg_av_addr, ipdcid};
	struct suet_tpdc *tpdc = NULL;

	HASH_FIND(tpdc_syn_key_handle, domain->tpdc_by_syn_key_ht, &key,
		  sizeof(struct suet_ut_tpdc_syn_key), tpdc);
	return tpdc;
}

/*
 * SES response returned from SES dispatch to PDS layer.
 * PDS uses this to decide ACK timing and populate the ACK's ses_resp_hdr.
 */
struct suet_ses_to_pds_resp {
	bool x_entry_owned; /* packet owned by SES (caller must NOT free it) */
	bool som_or_eom; /* true if SOM or EOM packet */
	bool gtd_del; /* SES response needs guaranteed delivery
		       * (Spec 3.5.11.4.4 / SES 3.4.3.3): the response
		       * is non-default and must be retained until the
		       * peer's CLEAR_PSN advances past its PSN. */
	uint8_t ses_opcode; /* ses_resp_opcode */
	uint8_t ses_rc; /* ses_return_code */
	uint8_t list; /* ses_list_type (UET_EXPECTED / UET_OVERFLOW) */
	uint8_t pds_nack_code; /* PDS NACK code; 0 == no NACK, (possibly) emit
				* ACK instead. Set by save/dispatch when the
				* response cannot be honored (e.g.
				* gtd_del_resp_pool exhausted ->
				* PDS_NACK_CODE_NO_RESOURCE). */
	uint16_t message_id; /* echoed back from the SES request */
	uint32_t modified_length; /* request_length echoed for the response */
	uint32_t psn; /* PSN of the request that produced this
		       * response; used to index gtd_del_list. For an
		       * unexp-msg reservation this is the SOM PSN -- the
		       * lowest PSN of the buffered message. */
	uint16_t num_pkts; /* When this slot is an unexpected-message
			    * reservation linked at SOM intake, num_pkts > 1
			    * means dup-PSN lookup must treat
			    * [psn, psn + num_pkts) as the covered range
			    * (whole message). For non-placeholder saved
			    * responses num_pkts is 1. */
	struct suet_tpdc *tpdc; /* owning tpdc when persisted */
	struct dlist_entry entry; /* link in tpdc->gtd_del_list */
};

/*
 * Spec 3.4.3.3: a non-default SES response (anything other than DEFAULT or
 * NO_RESPONSE) MUST be guaranteed-delivered. NO_RESPONSE is by definition the
 * "no semantic answer yet" and is NEVER saved. The actual GD
 * answer is generated later from the matched-after-buffer path.
 *
 * TODO: support UET_RESPONSE_W_DATA
 */
static inline bool
suet_ses_resp_is_gtd_del(const struct suet_ses_to_pds_resp *r)
{
	if (r->ses_opcode == UET_DEFAULT_RESPONSE ||
	    r->ses_opcode == UET_NO_RESPONSE)
		return false;
	return true;
}

struct suet_av {
	struct util_av util_av;
	struct fid_av *dg_av;
	struct indexer usr_av_addr_to_peer_idx;
	struct index_map peer_idx_to_usr_av_addr;
};

/*
 * Temporarily unwrap suet_av_addr addresses in an fi_info so the core
 * datagram provider sees its raw addresses.  Call
 * suet_av_info_wrap_raw_dg_addrs() afterwards to put the original pointers
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
suet_av_info_unwrap_raw_dg_addrs(struct fi_info *info,
				 struct suet_av_addr_tmp_storage *save)
{
	save->src_addr = info->src_addr;
	save->src_addrlen = info->src_addrlen;
	save->dest_addr = info->dest_addr;
	save->dest_addrlen = info->dest_addrlen;

	if (info->src_addr &&
	    info->src_addrlen >= sizeof(struct suet_av_addr)) {
		struct suet_av_addr *suet_a = info->src_addr;
		info->src_addr = suet_a->raw_dg_addr;
		info->src_addrlen = suet_a->raw_dg_addrlen;
	}
	if (info->dest_addr &&
	    info->dest_addrlen >= sizeof(struct suet_av_addr)) {
		struct suet_av_addr *suet_a = info->dest_addr;
		info->dest_addr = suet_a->raw_dg_addr;
		info->dest_addrlen = suet_a->raw_dg_addrlen;
	}
}

static inline void
suet_av_info_wrap_raw_dg_addrs(struct fi_info *info,
			       const struct suet_av_addr_tmp_storage *save)
{
	info->src_addr = save->src_addr;
	info->src_addrlen = save->src_addrlen;
	info->dest_addr = save->dest_addr;
	info->dest_addrlen = save->dest_addrlen;
}

static inline int suet_av_peer_idx_from_usr_av_addr(struct suet_av *av,
						    fi_addr_t fi_addr)
{
	return (int) (intptr_t) ofi_idx_lookup(&av->usr_av_addr_to_peer_idx,
					       SUET_IDX_OFFSET((int) fi_addr));
}

static inline fi_addr_t
suet_domain_dg_av_get_addr_by_peer_idx(struct suet_domain *domain, int peer_idx)
{
	return (fi_addr_t) (intptr_t) ofi_idx_lookup(
		&domain->peer_idx_to_dg_av_addr, (int) peer_idx);
}

struct suet_cq;
typedef int (*suet_cq_write_fn)(struct suet_cq *cq,
				struct fi_cq_tagged_entry *cq_entry);
struct suet_cq {
	struct util_cq util_cq;
	suet_cq_write_fn write_fn;
};

struct suet_ep {
	struct util_ep util_ep;

	uint16_t resource_index; /* assigned RI for this EP (12-bit) */
	uint16_t pid_on_fep; /* PIDonFEP from domain */

	size_t rx_size;
	size_t tx_size;
	size_t min_multi_recv_size;
	uint32_t tx_flags;
	uint32_t rx_flags;

	struct dlist_entry unexp_list;
	struct dlist_entry unexp_tag_list;
	struct dlist_entry rx_list;
	struct dlist_entry rx_tag_list;
	struct dlist_entry ctrl_pkts;
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

static inline int suet_domain_buf_pool_create(struct suet_domain *domain,
					      struct suet_buf_pool *pool,
					      struct ofi_bufpool_attr attr,
					      enum suet_pool_type type)
{
	int ret;
	pool->suet_domain = domain;
	pool->type = type;

	ret = ofi_bufpool_create_attr(&attr, &pool->pool);
	if (ret)
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"Unable to create buf pool\n");
	return ret;
}

struct suet_x_entry {
	struct dlist_entry entry;
	uint16_t tx_id;
	uint16_t rx_id;
	uint64_t bytes_copied;
	uint32_t next_rel_psn;
	uint32_t start_psn;
	uint64_t offset;
	uint32_t num_pkts;
	uint32_t op;

	uint32_t flags;
	uint64_t ignore;
	uint8_t iov_count;

	struct iovec iov[SUET_IOV_LIMIT];
	void *zc_desc[SUET_IOV_LIMIT];
	struct fid_mr *zc_internal_mrs[SUET_IOV_LIMIT];

	struct fi_cq_tagged_entry cq_entry;

	struct suet_ep *ep; /* owning EP (for CQ completion & pool free) */
	struct suet_ipdc *ipdc; /* ipdc this entry is queued on (TX path) */
	int peer_idx;

	size_t hdr_len;

	/* Must be last to make compiler happy */
	union {
		struct ses_msg_data_pkt data;
		struct ses_msg_amo_pkt amo;
	} cached_hdr;
};

/*
 * Carve a [offset, offset+len) window out of tx_entry->iov[] into
 * out_iov/out_desc for a single zero-copy segment.
 */
static inline size_t suet_tx_iov_find_slice(struct suet_x_entry *tx_entry,
					    size_t offset, size_t len,
					    struct iovec *out_iov,
					    void **out_desc)
{
	size_t i, n = 0, chunk_len;

	for (i = 0; i < tx_entry->iov_count && len; i++) {
		if (offset >= tx_entry->iov[i].iov_len) {
			offset -= tx_entry->iov[i].iov_len;
			continue;
		}
		chunk_len = MIN(len, tx_entry->iov[i].iov_len - offset);
		out_iov[n].iov_base =
			(char *) tx_entry->iov[i].iov_base + offset;
		out_iov[n].iov_len = chunk_len;
		out_desc[n] = tx_entry->zc_desc[i];
		n++;
		offset = 0;
		len -= chunk_len;
	}
	return n;
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

static inline int suet_ses_verify_mr_iov(struct suet_ep *ep,
					 struct ses_req_hdr *ses_hdr,
					 uint32_t type, struct iovec *iov)
{
	struct util_domain *util_domain = &suet_ep_domain(ep)->util_domain;
	uintptr_t addr = ses_get_buffer_offset(ses_hdr);
	int ret;

	ret = ofi_mr_verify(
		&util_domain->mr_map, ses_get_request_length(ses_hdr), &addr,
		ses_get_match_bits(ses_hdr), ofi_rx_mr_reg_flags(type, 0));
	iov->iov_base = (void *) addr;
	iov->iov_len = ses_get_request_length(ses_hdr);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "could not verify MR\n");
		return -FI_EACCES;
	}
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

struct suet_pkt_entry {
	struct dlist_entry d_entry;
	uint8_t flags;
	size_t pkt_size;
	uint64_t timestamp;
	struct fi_context context;
	struct fid_mr *pkt_buf_mr;
	void *pkt_buf_desc;
	fi_addr_t dg_av_addr; /* destination FEP address for fi_send */
	uint16_t local_pdcid; /* owning ipdc local_pdcid (send completion) */
	void *pkt;
	size_t zc_pld_iov_count;
	struct iovec zc_pld_iov[SUET_IOV_LIMIT + 1];
	void *zc_pld_desc[SUET_IOV_LIMIT + 1];
};

struct suet_unexp_msg {
	struct dlist_entry entry;
	struct dlist_entry pkt_list;
	int peer_idx; /* AV peer index for directed-recv matching */
	struct suet_ses_to_pds_resp *gtd_del_resp;
};

/* Return a packet entry to its buffer pool. */
static inline void suet_domain_pkt_entry_free(struct suet_pkt_entry *pkt_entry)
{
	ofi_buf_free(pkt_entry);
}

/* Unlink a packet entry from whichever dlist it is on (tx_list, in_flight_pkts,
 * ooo_pkts, rx_pkt_list, …) without freeing it. */
static inline void
suet_domain_pkt_entry_unlink(struct suet_pkt_entry *pkt_entry)
{
	dlist_remove(&pkt_entry->d_entry);
}

/* Combined unlink + free: the common end-of-life path for a packet entry. */
static inline void
suet_domain_pkt_entry_unlink_and_free(struct suet_pkt_entry *pkt_entry)
{
	suet_domain_pkt_entry_unlink(pkt_entry);
	suet_domain_pkt_entry_free(pkt_entry);
}

static inline struct suet_pkt_entry *
suet_ses_unexp_msg_get_som_pkt(struct suet_unexp_msg *unexp_msg)
{
	return container_of(unexp_msg->pkt_list.next, struct suet_pkt_entry,
			    d_entry);
}

static inline void suet_ses_unexp_msg_free(struct suet_unexp_msg *unexp_msg)
{
	dlist_remove(&unexp_msg->entry);
	ofi_buf_free(unexp_msg);
}

static inline void suet_ses_unexp_msg_cleanup(struct suet_unexp_msg *unexp_msg)
{
	struct suet_pkt_entry *pkt_entry;
	while (!dlist_empty(&unexp_msg->pkt_list)) {
		dlist_pop_front(&unexp_msg->pkt_list, struct suet_pkt_entry,
				pkt_entry, d_entry);
		suet_domain_pkt_entry_free(pkt_entry);
	}

	/* a non-NULL pointer here means the message was never matched, that is
	 * gtd-del response for it with SES_OVERFLOW was never issued. */
	if (unexp_msg->gtd_del_resp) {
		dlist_remove(&unexp_msg->gtd_del_resp->entry);
		ofi_buf_free(unexp_msg->gtd_del_resp);
	}
	suet_ses_unexp_msg_free(unexp_msg);
}

static inline void suet_ses_unexp_msg_list_cleanup(struct dlist_entry *list)
{
	struct suet_unexp_msg *unexp_msg;

	while (!dlist_empty(list)) {
		dlist_pop_front(list, struct suet_unexp_msg, unexp_msg, entry);
		suet_ses_unexp_msg_cleanup(unexp_msg);
	}
}

struct suet_match_attr {
	int peer_idx;
	uint64_t tag;
	uint64_t ignore;
};

static inline int suet_match_addr(int addr, int match_addr)
{
	return (addr == SUET_ADDR_INVALID || addr == match_addr);
}

static inline int suet_match_tag(uint64_t tag, uint64_t ignore,
				 uint64_t match_tag)
{
	return ((tag | ignore) == (match_tag | ignore));
}

static inline void
suet_domain_pkt_set_post_tx_prefix_ptr(struct suet_pkt_entry *pkt_entry,
				       size_t tx_prefix_size)
{
	pkt_entry->pkt = (void *) ((char *) pkt_entry + sizeof(*pkt_entry) +
				   tx_prefix_size);
}

static inline void
suet_domain_pkt_set_post_rx_prefix_ptr(struct suet_pkt_entry *pkt_entry,
				       size_t rx_prefix_size)
{
	pkt_entry->pkt = (void *) ((char *) pkt_entry + sizeof(*pkt_entry) +
				   rx_prefix_size);
}

static inline void *
suet_domain_pkt_get_prefix_ptr(struct suet_pkt_entry *pkt_entry)
{
	return (void *) ((char *) pkt_entry + sizeof(*pkt_entry));
}

int suet_info_to_core(uint32_t version, const struct fi_info *suet_info,
		      const struct fi_info *base_info,
		      struct fi_info *core_info);
int suet_info_to_suet(uint32_t version, const struct fi_info *core_info,
		      const struct fi_info *base_info, struct fi_info *info);
int suet_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
		void *context);

int suet_av_create(struct fid_domain *domain_fid, struct fi_av_attr *attr,
		   struct fid_av **av, void *context);

int suet_query_atomic(struct fid_domain *domain, enum fi_datatype datatype,
		      enum fi_op op, struct fi_atomic_attr *attr,
		      uint64_t flags);

int suet_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **dom, void *context);
int suet_domain_dg_av_init(struct suet_domain *domain);
void suet_domain_dg_av_cleanup(struct suet_domain *domain);
int suet_domain_dg_av_insert_raw_addr(struct suet_domain *domain,
				      const void *addr, int *peer_idx,
				      uint64_t flags, void *context);
int suet_domain_dg_av_get_peer_idx_by_addr(struct suet_domain *domain,
					   fi_addr_t dg_av_addr);
int suet_domain_dg_av_handle_addr_notavail(struct suet_domain *domain,
					   struct fi_cq_err_entry *err,
					   struct fi_cq_msg_entry *comp,
					   fi_addr_t *dg_av_addr);
ssize_t suet_domain_dg_ep_recv_pkt(struct suet_domain *domain);
ssize_t suet_domain_dg_ep_send_pkt(struct suet_domain *domain,
				   struct suet_pkt_entry *pkt_entry);
void suet_domain_progress(struct suet_domain *domain);
struct suet_pkt_entry *suet_domain_get_tx_pkt(struct suet_domain *domain);

int suet_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		 struct fid_cq **cq_fid, void *context);
int suet_cntr_open(struct fid_domain *domain, struct fi_cntr_attr *attr,
		   struct fid_cntr **cntr_fid, void *context);
void suet_cq_report_error(struct suet_cq *cq,
			  struct fi_cq_err_entry *err_entry);
void suet_cq_report_tx_comp(struct suet_cq *cq, struct suet_x_entry *tx_entry);

void suet_pds_tpdc_send_ack(struct suet_domain *domain, struct suet_tpdc *tpdc,
			    uint32_t trigger_psn, uint8_t next_hdr,
			    uint16_t message_id, uint32_t modified_length,
			    uint8_t resp_opcode, uint8_t resp_rc,
			    uint8_t resp_list);
struct suet_ses_to_pds_resp *
suet_pds_tpdc_save_gtd_del_resp(struct suet_domain *domain,
				struct suet_tpdc *tpdc,
				const struct suet_ses_to_pds_resp *resp);
struct suet_pkt_entry *suet_pds_ipdc_generate_new_req_pkt(
	struct suet_domain *domain, struct suet_x_entry *tx_entry,
	struct suet_ipdc *ipdc, size_t hdr_len, uint32_t psn, uint16_t flags);
struct suet_ipdc *suet_pds_assign_ipdc(struct suet_domain *domain,
				       fi_addr_t dg_av_addr);
void suet_pds_ipdc_begin_teardown(struct suet_ipdc *ipdc);
void suet_pds_ipdc_progress_teardown(struct suet_domain *domain,
				     struct suet_ipdc *ipdc);

void suet_pds_send_tx_entry(struct suet_x_entry *tx_entry);
void suet_ses_init_ses_hdr(struct ses_req_hdr *ses_hdr, struct suet_ep *ep,
			   struct suet_x_entry *tx_entry, int is_som,
			   int is_eom);
void suet_ses_tx_entry_complete(struct suet_x_entry *tx_entry);
struct suet_x_entry *
suet_ses_req_som_unpack_to_rx_entry(struct suet_ep *ep, struct suet_tpdc *tpdc,
				    struct suet_pkt_entry *pkt_entry,
				    struct ses_msg_data_pkt *pkt,
				    struct suet_ses_to_pds_resp *resp);
void suet_ses_execute_atomic_op(struct suet_ep *ep,
				struct suet_x_entry *rx_entry,
				struct ses_msg_amo_pkt *pkt);
struct suet_ep *suet_ses_lookup_ep_by_ri(struct suet_domain *domain,
					 struct ses_req_hdr *ses_hdr);
void suet_ses_unexp_msg_list_cleanup(struct dlist_entry *list);
void suet_ses_init_rma_iov(const struct fi_rma_iov *rma_iov,
			   struct suet_x_entry *tx_entry);
struct suet_x_entry *
suet_ses_tx_entry_init_common(struct suet_ep *ep, fi_addr_t addr, uint32_t op,
			      const struct iovec *iov, void **desc,
			      size_t iov_count, uint64_t tag, uint64_t data,
			      uint32_t flags, void *context);
int suet_ses_peek_recv(struct suet_ep *suet_ep, int peer_idx, uint64_t tag,
		       uint64_t ignore, void *context, uint64_t flags,
		       struct dlist_entry *unexp_list);
int suet_ses_discard_recv(struct suet_ep *suet_ep, void *context,
			  struct suet_unexp_msg *unexp_msg);
void suet_ses_drain_x_entry_list(struct dlist_entry *list, bool is_tx, int err);
int suet_ses_progress_unexp_list(struct suet_ep *ep,
				 struct dlist_entry *unexp_list,
				 struct dlist_entry *rx_list,
				 struct suet_x_entry *rx_entry);
struct suet_unexp_msg *suet_ses_check_unexp_list(struct dlist_entry *list,
						 int peer_idx, uint64_t tag,
						 uint64_t ignore);
struct suet_x_entry *suet_ses_rx_entry_init(struct suet_ep *ep,
					    const struct iovec *iov,
					    size_t iov_count, uint64_t tag,
					    uint64_t ignore, void *context,
					    int peer_idx, uint32_t op,
					    uint32_t flags);
void suet_ses_tx_entry_free(struct suet_x_entry *tx_entry);
void suet_ses_rx_entry_free(struct suet_x_entry *rx_entry);
void suet_ses_rx_entry_complete(struct suet_x_entry *rx_entry);
void suet_ses_complete_unexp_msg(struct suet_ep *ep,
				 struct suet_x_entry *rx_entry,
				 struct suet_unexp_msg *unexp_msg);
void suet_pds_process_rx_cqe(struct suet_domain *domain,
			     struct fi_cq_msg_entry *comp,
			     fi_addr_t dg_av_addr);
void suet_pds_process_tx_cqe(struct suet_domain *domain,
			     struct fi_cq_msg_entry *comp);
void suet_pds_ipdc_send_tracked_pkt(struct suet_domain *domain,
				    struct suet_ipdc *ipdc,
				    struct suet_pkt_entry *pkt_entry);
void suet_pds_ipdc_progress_tx_list(struct suet_ipdc *ipdc);
void suet_pds_ipdc_send_clear_cmd(struct suet_domain *domain,
				  struct suet_ipdc *ipdc);
void suet_pds_ipdc_progress_tx_pkt_list(struct suet_domain *domain,
					struct suet_ipdc *ipdc);
bool suet_pds_free_ipdc_if_retry_exhausted(struct suet_domain *domain,
					   struct suet_ipdc *ipdc);
void suet_pds_copy_payload(struct suet_ep *ep, struct suet_x_entry *rx_entry,
			   struct suet_pkt_entry *pkt_entry,
			   uint64_t copy_offset);

int suet_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct fid_ep **ep, void *context);
struct suet_x_entry *suet_ep_get_tx_entry(struct suet_ep *ep, uint32_t op);
struct suet_x_entry *suet_ep_get_rx_entry(struct suet_ep *ep, uint32_t op);
void suet_ep_progress(struct util_ep *util_ep);
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