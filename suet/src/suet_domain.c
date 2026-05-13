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

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "suet.h"

static struct fi_ops_domain suet_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = suet_av_create,
	.cq_open = suet_cq_open,
	.endpoint = suet_endpoint,
	.scalable_ep = fi_no_scalable_ep,
	.cntr_open = suet_cntr_open,
	.poll_open = fi_poll_create,
	.stx_ctx = fi_no_stx_context,
	.srx_ctx = fi_no_srx_context,
	.query_atomic = suet_query_atomic,
	.query_collective = fi_no_query_collective,
};

struct suet_pkt_entry *suet_domain_get_tx_pkt(struct suet_domain *domain)
{
	struct suet_pkt_entry *pkt_entry;

	pkt_entry = ofi_buf_alloc(domain->tx_pkt_entry_pool->pool);

	if (!pkt_entry)
		return NULL;

	pkt_entry->flags = 0;
	pkt_entry->zc_pld_iov_count = 0;

	return pkt_entry;
}

static inline int
suet_domain_buf_region_alloc_fn(struct ofi_bufpool_region *region)
{
	struct suet_buf_pool *pool = region->pool->attr.context;
	struct fid_mr *mr;
	int ret;

	if (!pool->suet_domain->do_local_mr) {
		region->context = NULL;
		return 0;
	}

	ret = fi_mr_reg(pool->suet_domain->fep_domain, region->mem_region,
			region->pool->region_size, FI_SEND | FI_RECV, 0, 0,
			OFI_MR_NOCACHE, &mr, NULL);

	region->context = mr;
	return ret;
}

static inline void
suet_domain_buf_region_free_fn(struct ofi_bufpool_region *region)
{
	struct suet_buf_pool *pool = region->pool->attr.context;

	if (pool->suet_domain->do_local_mr)
		fi_close(region->context);
}

static void suet_domain_free_pkt_entry_pools(struct suet_domain *domain)
{
	if (domain->tx_pkt_entry_pool) {
		if (domain->tx_pkt_entry_pool->pool)
			ofi_bufpool_destroy(domain->tx_pkt_entry_pool->pool);
		free(domain->tx_pkt_entry_pool);
		domain->tx_pkt_entry_pool = NULL;
	}

	if (domain->rx_pkt_entry_pool) {
		if (domain->rx_pkt_entry_pool->pool)
			ofi_bufpool_destroy(domain->rx_pkt_entry_pool->pool);
		free(domain->rx_pkt_entry_pool);
		domain->rx_pkt_entry_pool = NULL;
	}
}

static void suet_domain_pkt_entry_init_fn(struct ofi_bufpool_region *region,
					  void *buf)
{
	struct suet_pkt_entry *pkt_entry = (struct suet_pkt_entry *) buf;
	struct suet_buf_pool *pool =
		(struct suet_buf_pool *) region->pool->attr.context;

	if (pool->suet_domain->do_local_mr)
		pkt_entry->pkt_buf_desc =
			fi_mr_desc((struct fid_mr *) region->context);
	else
		pkt_entry->pkt_buf_desc = NULL;

	pkt_entry->pkt_buf_mr = (struct fid_mr *) region->context;
	if (pool->type == SUET_BUF_POOL_RX) {
		suet_domain_pkt_set_post_rx_prefix_ptr(
			pkt_entry, pool->suet_domain->rx_prefix_size);
	} else {
		suet_domain_pkt_set_post_tx_prefix_ptr(
			pkt_entry, pool->suet_domain->tx_prefix_size);
		pkt_entry->zc_pld_iov[0].iov_base =
			suet_domain_pkt_get_prefix_ptr(pkt_entry);
		pkt_entry->zc_pld_iov[0].iov_len =
			pool->suet_domain->tx_prefix_size +
			sizeof(struct ses_msg_data_pkt);
		pkt_entry->zc_pld_desc[0] = pkt_entry->pkt_buf_desc;
	}
}

static int suet_domain_pkt_entry_pool_create(struct suet_domain *domain,
					     size_t chunk_cnt,
					     struct suet_buf_pool *pool,
					     enum suet_pool_type type)
{
	struct ofi_bufpool_attr attr = {
		.size = domain->max_mtu_sz + sizeof(struct suet_pkt_entry),
		.alignment = SUET_BUF_POOL_ALIGNMENT,
		.max_cnt = 0,
		.chunk_cnt = chunk_cnt,
		.alloc_fn = suet_domain_buf_region_alloc_fn,
		.free_fn = suet_domain_buf_region_free_fn,
		.init_fn = suet_domain_pkt_entry_init_fn,
		.context = pool,
		.flags = OFI_BUFPOOL_HUGEPAGES,
	};

	return suet_domain_buf_pool_create(domain, pool, attr, type);
}

static void suet_domain_x_entry_init_fn(struct ofi_bufpool_region *region,
					void *buf)
{
	struct suet_x_entry *entry = (struct suet_x_entry *) buf;
	struct suet_buf_pool *pool =
		(struct suet_buf_pool *) region->pool->attr.context;

	if (pool->type == SUET_BUF_POOL_TX)
		entry->tx_id = (uint16_t) ofi_buf_index(entry);
	else
		entry->rx_id = (uint16_t) ofi_buf_index(entry);
}

static int suet_domain_x_entry_pool_create(struct suet_domain *domain,
					   size_t chunk_cnt,
					   struct suet_buf_pool *pool,
					   enum suet_pool_type type)
{
	struct ofi_bufpool_attr attr = {
		.size = sizeof(struct suet_x_entry),
		.alignment = SUET_BUF_POOL_ALIGNMENT,
		.max_cnt = (size_t) ((uint16_t) (~0)),
		.chunk_cnt = chunk_cnt,
		.alloc_fn = NULL,
		.free_fn = NULL,
		.init_fn = suet_domain_x_entry_init_fn,
		.context = pool,
		.flags = OFI_BUFPOOL_INDEXED | OFI_BUFPOOL_NO_TRACK |
			 OFI_BUFPOOL_HUGEPAGES,
	};

	return suet_domain_buf_pool_create(domain, pool, attr, type);
}

static int suet_domain_init_x_entry_pools(struct suet_domain *domain)
{
	int ret;

	ret = suet_domain_x_entry_pool_create(domain, 1ULL << SUET_MAX_TX_BITS,
					      &domain->tx_entry_pool,
					      SUET_BUF_POOL_TX);
	if (ret)
		return ret;

	ret = suet_domain_x_entry_pool_create(domain, 1ULL << SUET_MAX_RX_BITS,
					      &domain->rx_entry_pool,
					      SUET_BUF_POOL_RX);
	if (ret) {
		ofi_bufpool_destroy(domain->tx_entry_pool.pool);
		domain->tx_entry_pool.pool = NULL;
		return ret;
	}
	return 0;
}

static void suet_domain_free_x_entry_pools(struct suet_domain *domain)
{
	if (domain->tx_entry_pool.pool) {
		ofi_bufpool_destroy(domain->tx_entry_pool.pool);
		domain->tx_entry_pool.pool = NULL;
	}
	if (domain->rx_entry_pool.pool) {
		ofi_bufpool_destroy(domain->rx_entry_pool.pool);
		domain->rx_entry_pool.pool = NULL;
	}
}

static int suet_domain_init_pkt_entry_pools(struct suet_domain *domain)
{
	int ret;

	domain->tx_pkt_entry_pool =
		calloc(1, sizeof(*domain->tx_pkt_entry_pool));
	if (!domain->tx_pkt_entry_pool)
		return -FI_ENOMEM;

	domain->rx_pkt_entry_pool =
		calloc(1, sizeof(*domain->rx_pkt_entry_pool));
	if (!domain->rx_pkt_entry_pool) {
		free(domain->tx_pkt_entry_pool);
		domain->tx_pkt_entry_pool = NULL;
		return -FI_ENOMEM;
	}

	ret = suet_domain_pkt_entry_pool_create(domain, SUET_TX_POOL_CHUNK_CNT,
						domain->tx_pkt_entry_pool,
						SUET_BUF_POOL_TX);
	if (ret)
		goto err;

	ret = suet_domain_pkt_entry_pool_create(domain, SUET_RX_POOL_CHUNK_CNT,
						domain->rx_pkt_entry_pool,
						SUET_BUF_POOL_RX);
	if (ret)
		goto err;

	return 0;
err:
	suet_domain_free_pkt_entry_pools(domain);
	return ret;
}

ssize_t suet_domain_dg_ep_send_pkt(struct suet_domain *domain,
				   struct suet_pkt_entry *pkt_entry)
{
	ssize_t ret;
	pkt_entry->timestamp = ofi_gettime_ms();

	if (pkt_entry->zc_pld_iov_count) {
		ret = fi_sendv(domain->dg_ep, pkt_entry->zc_pld_iov,
			       pkt_entry->zc_pld_desc,
			       pkt_entry->zc_pld_iov_count + 1,
			       pkt_entry->dg_av_addr, &pkt_entry->context);
	} else {
		ret = fi_send(domain->dg_ep,
			      (const void *) suet_domain_pkt_get_prefix_ptr(
				      pkt_entry),
			      pkt_entry->pkt_size, pkt_entry->pkt_buf_desc,
			      pkt_entry->dg_av_addr, &pkt_entry->context);
	}
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"error sending packet: %d (%s)\n", (int) ret,
			fi_strerror((int) -ret));
		return ret;
	}

	pkt_entry->flags |= SUET_PKT_IN_USE;

	return 0;
}

ssize_t suet_domain_dg_ep_recv_pkt(struct suet_domain *domain)
{
	struct suet_pkt_entry *pkt_entry;
	ssize_t ret;

	pkt_entry = ofi_buf_alloc(domain->rx_pkt_entry_pool->pool);
	if (!pkt_entry)
		return -FI_ENOMEM;

	ret = fi_recv(domain->dg_ep, suet_domain_pkt_get_prefix_ptr(pkt_entry),
		      domain->max_mtu_sz, pkt_entry->pkt_buf_desc,
		      FI_ADDR_UNSPEC, &pkt_entry->context);
	if (ret) {
		suet_domain_pkt_entry_free(pkt_entry);
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "failed to repost\n");
		return ret;
	}

	dlist_insert_tail(&pkt_entry->d_entry, &domain->rx_pkt_list);

	return 0;
}

/*
 * Broadcast a synthetic fi_cq_err_entry to every EP currently bound to
 * this domain. Returns the number of CQs the error was successfully
 * written to.
 *
 * Used as the primary surface for unrecoverable dgram-level CQ errors
 * that have lost their per-op association by the time they reach us
 * (e.g. an errored pre-posted RX buffer carries no EP context). Per
 * libfabric idiom (cf. prov/efa/src/efa_base_ep.c, prov/tcp/src/xnet_ep.c),
 * abort() is the fallback only when no surface is reachable.
 */
static int suet_domain_broadcast_cq_err(struct suet_domain *domain, int err,
					bool is_tx)
{
	struct suet_ep *ep;
	struct util_cq *util_cq;
	struct fi_cq_err_entry err_entry = {
		.err = err,
		.prov_errno = err,
	};
	int written = 0;
	int i;

	if (!domain->ep_table)
		return 0;

	for (i = 0; i < suet_env.max_eps; i++) {
		ep = domain->ep_table[i];
		if (!ep)
			continue;
		util_cq = is_tx ? &suet_ep_tx_cq(ep)->util_cq :
				  &suet_ep_rx_cq(ep)->util_cq;
		if (ofi_cq_write_error(util_cq, &err_entry) == 0)
			written++;
	}
	return written;
}

/*
 * Handle an RX CQ error completion. Returns 0 if the error was
 * resolved via AV-fixup and the caller should proceed to process
 * cq_entry/dg_av_addr as a normal RX completion; negative otherwise.
 */
static int suet_domain_dg_rx_cq_handle_error(struct suet_domain *domain,
					     struct fi_cq_msg_entry *cq_entry,
					     fi_addr_t *dg_av_addr)
{
	struct fi_cq_err_entry err = {0};
	struct suet_pkt_entry *pkt_entry;
	ssize_t ret;

	ret = fi_cq_readerr(domain->dg_rx_cq, &err, 0);
	if (ret < 0) {
		FI_WARN(&suet_prov, FI_LOG_CQ, "fi_cq_readerr(rx) error: %s\n",
			fi_strerror((int) -ret));
		return ret;
	}

	pkt_entry =
		container_of(err.op_context, struct suet_pkt_entry, context);

	if (err.err == FI_EADDRNOTAVAIL && err.err_data &&
	    err.err_data_size > 0 &&
	    !suet_domain_dg_av_handle_addr_notavail(domain, &err, cq_entry,
						    dg_av_addr)) {
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

	suet_domain_pkt_entry_unlink_and_free(pkt_entry);
	suet_domain_dg_ep_recv_pkt(domain);
	return -FI_EAVAIL;
}

static void suet_domain_dg_tx_cq_handle_error(struct suet_domain *domain)
{
	struct fi_cq_err_entry err = {0};
	struct fi_cq_msg_entry cq_entry = {0};
	ssize_t ret;

	ret = fi_cq_readerr(domain->dg_tx_cq, &err, 0);
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

	/* do cleanup so the errored pkt_entry doesn't leak: for
	 * PDS_ACK/NACK it is freed; for PDS_ROD_REQ it has SUET_PKT_IN_USE
	 * cleared so the RTO path can retransmit it. Per-tx_entry CQ errors
	 * are written by suet_ses_drain_x_entry_list when the owning ipdc
	 * exhausts retries (suet_pds_free_ipdc_if_retry_exhausted). */
	cq_entry.op_context = err.op_context;
	cq_entry.flags = err.flags;
	suet_pds_process_tx_cqe(domain, &cq_entry);
}

/*
 * Poll the TX CQ once and dispatch up to suet_env.cq_read_batch_size
 * completions. Returns the number of entries processed, 0 if none.
 */
static int suet_domain_dg_tx_cq_poll(struct suet_domain *domain)
{
	ssize_t ret;
	int i;

	ret = fi_cq_read(domain->dg_tx_cq, domain->tx_cq_entries,
			 domain->cq_read_batch_size);
	if (ret < 0) {
		if (ret == -FI_EAVAIL)
			suet_domain_dg_tx_cq_handle_error(domain);
		else if (ret != -FI_EAGAIN)
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"fi_cq_read(tx) error: %s\n",
				fi_strerror((int) -ret));
		return 0;
	}

	for (i = 0; i < ret; i++)
		suet_pds_process_tx_cqe(domain, &domain->tx_cq_entries[i]);
	return (int) ret;
}

/*
 * Poll the RX CQ once and dispatch up to suet_env.cq_read_batch_size
 * completions. RX needs the source FEP datagram address (dg_av_addr) to
 * demultiplex incoming packets to the right FEP. Returns the
 * number of entries processed, 0 if none.
 */
static int suet_domain_dg_rx_cq_poll(struct suet_domain *domain)
{
	ssize_t ret;
	int i;

	ret = fi_cq_readfrom(domain->dg_rx_cq, domain->rx_cq_entries,
			     domain->cq_read_batch_size,
			     domain->cq_rx_dg_av_addrs);
	if (ret < 0) {
		if (ret == -FI_EAVAIL) {
			/* Error applies to a single completion; recover or
			 * bail. On recovery the handler populates
			 * rx_cq_entries[0]/cq_rx_dg_av_addrs[0] with the real
			 * entry. */
			if (suet_domain_dg_rx_cq_handle_error(
				    domain, &domain->rx_cq_entries[0],
				    &domain->cq_rx_dg_av_addrs[0]) < 0)
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
		suet_domain_dg_ep_recv_pkt(domain);
	for (i = 0; i < ret; i++)
		suet_pds_process_rx_cqe(domain, &domain->rx_cq_entries[i],
					domain->cq_rx_dg_av_addrs[i]);

	return (int) ret;
}

void suet_domain_progress(struct suet_domain *domain)
{
	struct suet_ipdc *ipdc;
	struct dlist_entry *tmp;
	int tx_got, rx_got;
	int i;

	for (i = 0; !suet_env.spin_count || i < suet_env.spin_count; i++) {
		tx_got = suet_domain_dg_tx_cq_poll(domain);
		rx_got = suet_domain_dg_rx_cq_poll(domain);
		if (!tx_got && !rx_got) {
			break;
		}
	}

	dlist_foreach_container_safe (&domain->active_ipdc_list,
				      struct suet_ipdc, ipdc, entry, tmp) {
		if (suet_env.retry) {
			suet_pds_ipdc_progress_tx_pkt_list(domain, ipdc);
			if (suet_pds_free_ipdc_if_retry_exhausted(domain, ipdc))
				continue;
			if (dlist_empty(&ipdc->in_flight_pkts))
				suet_pds_ipdc_progress_tx_list(ipdc);
		}
		suet_pds_ipdc_progress_teardown(domain, ipdc);
	}
}

static void suet_domain_teardown_pdcs(struct suet_domain *domain)
{
	struct suet_ipdc *ipdc;
	struct dlist_entry *tmp;

	dlist_foreach_container_safe (&domain->active_ipdc_list,
				      struct suet_ipdc, ipdc, entry, tmp)
		suet_pds_ipdc_begin_teardown(ipdc);

	while (!dlist_empty(&domain->active_ipdc_list) ||
	       !dlist_empty(&domain->active_tpdc_list)) {
		suet_domain_progress(domain);
	}
}

static int suet_domain_close(fid_t fid)
{
	int ret;
	struct suet_domain *suet_domain;
	struct suet_pkt_entry *pkt_entry;

	suet_domain = container_of(fid, struct suet_domain,
				   util_domain.domain_fid.fid);

	suet_domain_teardown_pdcs(suet_domain);

	if (suet_env.print_domain_counters)
		FI_INFO(&suet_prov, FI_LOG_DOMAIN,
			"[SUET domain counters]: nacks_tx=%lu nacks_rx=%lu "
			"stale_acks_rx=%lu error_acks_rx=%lu tx_cq_errors=%lu "
			"rx_cq_errors=%lu pdc_closes_ok=%lu "
			"pdc_close_in_err=%lu pdc_closes_rx=%lu "
			"dup_drops=%lu ses_err_completions=%lu\n",
			(unsigned long) suet_domain->counters.nacks_tx,
			(unsigned long) suet_domain->counters.nacks_rx,
			(unsigned long) suet_domain->counters.stale_acks_rx,
			(unsigned long) suet_domain->counters.error_acks_rx,
			(unsigned long) suet_domain->counters.tx_cq_errors,
			(unsigned long) suet_domain->counters.rx_cq_errors,
			(unsigned long) suet_domain->counters.pdc_closes_ok,
			(unsigned long) suet_domain->counters.pdc_close_in_err,
			(unsigned long) suet_domain->counters.pdc_closes_rx,
			(unsigned long) suet_domain->counters.dup_drops,
			(unsigned long) suet_domain->counters.ses_err_completions);
	/* Drain posted RX buffers */
	while (!dlist_empty(&suet_domain->rx_pkt_list)) {
		pkt_entry = container_of(suet_domain->rx_pkt_list.next,
					 struct suet_pkt_entry, d_entry);
		suet_domain_pkt_entry_unlink_and_free(pkt_entry);
	}

	suet_domain_free_x_entry_pools(suet_domain);
	suet_domain_free_pkt_entry_pools(suet_domain);

	ofi_idm_reset(&suet_domain->local_pdcid_to_ipdc_idm, NULL);
	ofi_idm_reset(&suet_domain->local_pdcid_to_tpdc_idm, NULL);
	HASH_CLEAR(ipdc_dg_av_addr_handle, suet_domain->ipdc_by_dg_av_addr_ht);
	HASH_CLEAR(tpdc_syn_key_handle, suet_domain->tpdc_by_syn_key_ht);

	if (suet_domain->ipdc_pool)
		ofi_bufpool_destroy(suet_domain->ipdc_pool);
	if (suet_domain->tpdc_pool)
		ofi_bufpool_destroy(suet_domain->tpdc_pool);
	if (suet_domain->gtd_del_resp_pool)
		ofi_bufpool_destroy(suet_domain->gtd_del_resp_pool);
	if (suet_domain->unexp_msg_pool)
		ofi_bufpool_destroy(suet_domain->unexp_msg_pool);

	if (suet_domain->dg_ep) {
		ret = fi_close(&suet_domain->dg_ep->fid);
		if (ret)
			return ret;
	}

	suet_domain_dg_av_cleanup(suet_domain);

	if (suet_domain->dg_rx_cq) {
		ret = fi_close(&suet_domain->dg_rx_cq->fid);
		if (ret) {
			return ret;
		}
	}

	if (suet_domain->dg_tx_cq) {
		ret = fi_close(&suet_domain->dg_tx_cq->fid);
		if (ret) {
			return ret;
		}
	}

	ret = fi_close(&suet_domain->fep_domain->fid);
	if (ret)
		return ret;

	ret = ofi_domain_close(&suet_domain->util_domain);
	if (ret)
		return ret;

	ofi_genlock_destroy(&suet_domain->fep_lock);
	free(suet_domain->ep_table);
	free(suet_domain);
	return 0;
}

static struct fi_ops suet_domain_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = suet_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int suet_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **domain, void *context)
{
	int ret, i;
	struct fi_info *dg_info;
	struct suet_domain *suet_domain;
	struct suet_fabric *suet_fabric;
	struct suet_av_addr_tmp_storage addr_save = {0};
	struct fi_cq_attr cq_attr = {0};
	size_t min_cq_size;

	suet_fabric = container_of(fabric, struct suet_fabric,
				   util_fabric.fabric_fid);

	suet_domain = calloc(1, sizeof(*suet_domain));
	if (!suet_domain)
		return -FI_ENOMEM;

	/* Unwrap suet_av_addr addresses so the core provider sees raw
	 * datagram addresses (needed for fi_endpoint below). */
	suet_av_info_unwrap_raw_dg_addrs(info, &addr_save);

	ret = ofi_get_core_info(fabric->api_version, NULL, NULL, 0,
				&suet_util_prov, info, NULL, suet_info_to_core,
				&dg_info);

	suet_av_info_wrap_raw_dg_addrs(info, &addr_save);

	if (ret)
		goto err1;

	ret = fi_domain(suet_fabric->dg_fabric, dg_info,
			&suet_domain->fep_domain, context);
	if (ret)
		goto err2;

	suet_domain->max_mtu_sz =
		MIN(dg_info->ep_attr->max_msg_size, PDS_MAX_MTU_SIZE);
	suet_domain->max_inline_msg = suet_domain->max_mtu_sz -
				      sizeof(struct pds_req_hdr) -
				      dg_info->ep_attr->msg_prefix_size;
	suet_domain->max_inline_rma = suet_domain->max_inline_msg;
	suet_domain->max_inline_atom =
		suet_domain->max_inline_rma - sizeof(struct ses_msg_amo_hdr);
	suet_domain->max_seg_sz = suet_domain->max_mtu_sz -
				  sizeof(struct ses_msg_data_pkt) -
				  dg_info->ep_attr->msg_prefix_size;

	/* Shared FEP: create the datagram endpoint on the domain */
	suet_domain->do_local_mr = ofi_mr_local(dg_info);
	suet_domain->tx_prefix_size =
		dg_info->tx_attr->mode & FI_MSG_PREFIX ?
			dg_info->ep_attr->msg_prefix_size :
			0;
	suet_domain->rx_prefix_size =
		dg_info->rx_attr->mode & FI_MSG_PREFIX ?
			dg_info->ep_attr->msg_prefix_size :
			0;

	ret = fi_endpoint(suet_domain->fep_domain, dg_info, &suet_domain->dg_ep,
			  suet_domain);
	if (ret)
		goto err3;

	ret = suet_domain_dg_av_init(suet_domain);
	if (ret)
		goto err4;

	ret = fi_ep_bind(suet_domain->dg_ep, &suet_domain->dg_av->fid, 0);
	if (ret)
		goto err5;

	cq_attr.format = FI_CQ_FORMAT_MSG;
	cq_attr.wait_obj = FI_WAIT_NONE;

	cq_attr.size = dg_info->tx_attr->size;
	ret = fi_cq_open(suet_domain->fep_domain, &cq_attr,
			 &suet_domain->dg_tx_cq, suet_domain);
	if (ret)
		goto err5;

	ret = fi_ep_bind(suet_domain->dg_ep, &suet_domain->dg_tx_cq->fid,
			 FI_TRANSMIT);
	if (ret)
		goto err6;

	cq_attr.size = dg_info->rx_attr->size;
	ret = fi_cq_open(suet_domain->fep_domain, &cq_attr,
			 &suet_domain->dg_rx_cq, suet_domain);
	if (ret)
		goto err6;

	ret = fi_ep_bind(suet_domain->dg_ep, &suet_domain->dg_rx_cq->fid,
			 FI_RECV);
	if (ret)
		goto err6_rx;

	suet_domain->cq_read_batch_size = suet_env.cq_read_batch_size;
	min_cq_size = MIN(dg_info->tx_attr->size, dg_info->rx_attr->size);
	if ((size_t) suet_domain->cq_read_batch_size > min_cq_size) {
		FI_INFO(&suet_prov, FI_LOG_DOMAIN,
			"clamping cq_read_batch_size from %d to available CQ "
			"size "
			"%zu\n",
			suet_domain->cq_read_batch_size, min_cq_size);
		suet_domain->cq_read_batch_size = (int) min_cq_size;
	}

	ret = fi_enable(suet_domain->dg_ep);
	if (ret)
		goto err6_rx;

	dlist_init(&suet_domain->rx_pkt_list);

	ret = suet_domain_init_pkt_entry_pools(suet_domain);
	if (ret)
		goto err6;

	ret = suet_domain_init_x_entry_pools(suet_domain);
	if (ret) {
		suet_domain_free_pkt_entry_pools(suet_domain);
		goto err6;
	}

	for (i = 0; i < dg_info->rx_attr->size; i++)
		if (suet_domain_dg_ep_recv_pkt(suet_domain))
			break;

	ret = ofi_domain_init(fabric, info, &suet_domain->util_domain, context,
			      OFI_LOCK_MUTEX);
	if (ret)
		goto err7;

	ret = ofi_genlock_init(&suet_domain->fep_lock,
			       suet_domain->util_domain.threading !=
					       FI_THREAD_SAFE ?
				       OFI_LOCK_NOOP :
				       OFI_LOCK_MUTEX);
	if (ret)
		goto err8;

	suet_domain->ep_table =
		calloc(suet_env.max_eps, sizeof(*suet_domain->ep_table));
	if (!suet_domain->ep_table) {
		ret = -FI_ENOMEM;
		goto err9;
	}

	suet_domain->pid_on_fep = suet_domain_get_pid_on_fep();
	suet_domain->next_ri = 0;

	ret = ofi_bufpool_create(
		&suet_domain->ipdc_pool, sizeof(struct suet_ipdc),
		SUET_BUF_POOL_ALIGNMENT, 0, suet_env.max_peers, 0);
	if (ret)
		goto err9;

	ret = ofi_bufpool_create(
		&suet_domain->tpdc_pool, sizeof(struct suet_tpdc),
		SUET_BUF_POOL_ALIGNMENT, 0, suet_env.max_peers, 0);
	if (ret)
		goto err10;

	ret = ofi_bufpool_create(&suet_domain->gtd_del_resp_pool,
				 sizeof(struct suet_ses_to_pds_resp),
				 SUET_BUF_POOL_ALIGNMENT,
				 suet_env.max_gtd_del_resp_pool_size, 0, 0);
	if (ret)
		goto err11;

	ret = ofi_bufpool_create(
		&suet_domain->unexp_msg_pool, sizeof(struct suet_unexp_msg),
		SUET_BUF_POOL_ALIGNMENT, suet_env.unexp_msg_pool_size, 0, 0);
	if (ret)
		goto err12;

	memset(&suet_domain->local_pdcid_to_ipdc_idm, 0,
	       sizeof(suet_domain->local_pdcid_to_ipdc_idm));
	memset(&suet_domain->local_pdcid_to_tpdc_idm, 0,
	       sizeof(suet_domain->local_pdcid_to_tpdc_idm));
	suet_domain->ipdc_by_dg_av_addr_ht = NULL;
	suet_domain->tpdc_by_syn_key_ht = NULL;
	dlist_init(&suet_domain->active_ipdc_list);
	dlist_init(&suet_domain->active_tpdc_list);
	suet_domain->next_pdcid = 1; /* PDS Spec 3.5.8.2: PDCID 0 is reserved */
	suet_domain->psn_seed = ofi_generate_seed();

	suet_domain->zc_mr_reg_threshold = suet_env.zc_mr_reg_threshold;
	suet_domain->mr_key = 0;

	*domain = &suet_domain->util_domain.domain_fid;
	(*domain)->fid.ops = &suet_domain_fi_ops;
	(*domain)->ops = &suet_domain_ops;
	(*domain)->mr = &suet_domain_mr_ops;
	fi_freeinfo(dg_info);
	return 0;

err12:
	ofi_bufpool_destroy(suet_domain->gtd_del_resp_pool);
err11:
	ofi_bufpool_destroy(suet_domain->tpdc_pool);
err10:
	ofi_bufpool_destroy(suet_domain->ipdc_pool);
err9:
	ofi_genlock_destroy(&suet_domain->fep_lock);
err8:
	if (ofi_domain_close(&suet_domain->util_domain))
		FI_WARN(&suet_prov, FI_LOG_DOMAIN, "ofi_domain_close failed");
err7:
	suet_domain_free_x_entry_pools(suet_domain);
	suet_domain_free_pkt_entry_pools(suet_domain);
err6_rx:
	fi_close(&suet_domain->dg_rx_cq->fid);
err6:
	fi_close(&suet_domain->dg_tx_cq->fid);
err5:
	suet_domain_dg_av_cleanup(suet_domain);
err4:
	fi_close(&suet_domain->dg_ep->fid);
err3:
	fi_close(&suet_domain->fep_domain->fid);
err2:
	fi_freeinfo(dg_info);
err1:
	free(suet_domain);
	return ret;
}
