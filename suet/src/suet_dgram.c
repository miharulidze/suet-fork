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
#include "suet_pds_dgram_api.h"

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
	ssize_t ret;

	assert(!entry->in_use);
	if (pkt->zc_pld_iov_count) {
		/* PDS supplies the wire header in slot zero and payload in the
		 * rest. Use a local header view so retransmits never accumulate
		 * prefixes.
		 */
		struct iovec iov[SUET_IOV_LIMIT + 1];

		memcpy(iov, pkt->zc_pld_iov,
		       (pkt->zc_pld_iov_count + 1) * sizeof(*iov));
		iov[0].iov_base = (char *) pkt->pkt - prefix_size;
		iov[0].iov_len += prefix_size;
		pkt->zc_pld_desc[0] = entry->desc;
		ret = fi_sendv(domain->dgram.ep, iov, pkt->zc_pld_desc,
			       pkt->zc_pld_iov_count + 1, pkt->dgram_av_addr,
			       &entry->context);
	} else {
		ret = fi_send(domain->dgram.ep, (char *) pkt->pkt - prefix_size,
			      pkt->pkt_size + prefix_size, entry->desc,
			      pkt->dgram_av_addr, &entry->context);
	}
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
				   struct fi_cq_msg_entry *comp,
				   fi_addr_t src_addr)
{
	struct suet_dgram_pkt_entry *entry = container_of(
		comp->op_context, struct suet_dgram_pkt_entry, context);

	entry->in_use = false;
	dlist_remove_init(&entry->entry);
	if (comp->len < domain->dgram.rx_prefix_size) {
		suet_dgram_pkt_free(&entry->pkt);
		return;
	}
	entry->pkt.pkt_size = comp->len - domain->dgram.rx_prefix_size;
	suet_pds_receive(domain, &entry->pkt, src_addr);
}

/*
 * Handle an RX CQ error completion. Returns 0 if the error was
 * resolved via AV-fixup and the caller should proceed to process
 * cq_entry/dgram_av_addr as a normal RX completion; negative otherwise.
 */
static int suet_dgram_rx_cq_handle_error(struct suet_domain *domain,
					 struct fi_cq_msg_entry *cq_entry,
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

	ret = fi_cq_readfrom(domain->dgram.rx_cq, domain->dgram.rx_cq_entries,
			     domain->dgram.cq_read_batch_size,
			     domain->dgram.rx_cq_addrs);
	if (ret < 0) {
		if (ret == -FI_EAVAIL) {
			/* Error applies to a single completion; recover or
			 * bail. On recovery the handler populates
			 * rx_cq_entries[0]/rx_cq_addrs[0] with the real
			 * entry. */
			if (suet_dgram_rx_cq_handle_error(
				    domain, &domain->dgram.rx_cq_entries[0],
				    &domain->dgram.rx_cq_addrs[0]) < 0)
				return 0;
			ret = 1;
			goto process_cqes;
		}
		if (ret != -FI_EAGAIN)
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"fi_cq_readfrom(rx) error: %s\n",
				fi_strerror((int) -ret));
		return 0;
	}

process_cqes:
	for (i = 0; i < ret; i++)
		suet_dgram_ep_recv_pkt(domain);
	for (i = 0; i < ret; i++)
		suet_dgram_rx_complete(domain, &domain->dgram.rx_cq_entries[i],
				       domain->dgram.rx_cq_addrs[i]);

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
	struct suet_av_addr_tmp_storage addr_save = {0};
	struct fi_cq_attr cq_attr = {0};
	size_t min_cq_size;
	int ret, i;

	suet_domain->dgram.pds_pkt_size = pds_pkt_size;
	suet_domain->dgram.ses_pkt_size = ses_pkt_size;
	dlist_init(&suet_domain->dgram.rx_pkt_list);
	/* Unwrap suet_av_addr addresses so the core provider sees raw
	 * datagram addresses (needed for fi_endpoint below). */
	suet_av_info_unwrap_raw_dgram_addrs(info, &addr_save);

	ret = ofi_get_core_info(fabric->api_version, NULL, NULL, 0,
				&suet_util_prov, info, NULL, suet_info_to_core,
				&dgram_info);

	suet_av_info_wrap_raw_dgram_addrs(info, &addr_save);

	if (ret)
		goto err;

	ret = fi_domain(suet_fabric->dgram_fabric, dgram_info,
			&suet_domain->dgram.domain, context);
	if (ret)
		goto err;

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
