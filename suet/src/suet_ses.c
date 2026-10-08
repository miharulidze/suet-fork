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

/*
 * SES (Semantic Sublayer) protocol code: TX post, RX dispatch, entry
 * completion, atomic exec, message matching.
 */

#include "suet_ses.h"
#include "suet.h"

static void suet_ses_entry_init_fn(struct ofi_bufpool_region *region, void *buf)
{
	struct suet_ses_resources *ses = region->pool->attr.context;

	if (region->pool == ses->tx_entry_pool) {
		struct suet_ses_tx_entry *entry = buf;
		entry->tx_id = (uint16_t) ofi_buf_index(entry);
	} else {
		struct suet_ses_rx_entry *entry = buf;
		entry->rx_id = (uint16_t) ofi_buf_index(entry);
	}
}

static int suet_ses_entry_pool_create(struct suet_ses_resources *ses,
				      size_t entry_size, size_t chunk_cnt,
				      struct ofi_bufpool **pool)
{
	struct ofi_bufpool_attr attr = {
		.size = entry_size,
		.alignment = SUET_BUF_POOL_ALIGNMENT,
		.max_cnt = (size_t) ((uint16_t) (~0)),
		.chunk_cnt = chunk_cnt,
		.alloc_fn = NULL,
		.free_fn = NULL,
		.init_fn = suet_ses_entry_init_fn,
		.context = ses,
		.flags = OFI_BUFPOOL_INDEXED | OFI_BUFPOOL_NO_TRACK |
			 OFI_BUFPOOL_HUGEPAGES,
	};
	int ret;

	ret = ofi_bufpool_create_attr(&attr, pool);
	if (ret)
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"Unable to create SES entry pool\n");
	return ret;
}

void suet_ses_cleanup(struct suet_domain *domain)
{
	if (domain->ses.tx_entry_pool)
		ofi_bufpool_destroy(domain->ses.tx_entry_pool);
	if (domain->ses.rx_entry_pool)
		ofi_bufpool_destroy(domain->ses.rx_entry_pool);
	if (domain->ses.unexp_msg_pool)
		ofi_bufpool_destroy(domain->ses.unexp_msg_pool);
	memset(&domain->ses, 0, sizeof(domain->ses));
	dlist_init(&domain->ses.rx_ctx_list);
}

int suet_ses_init(struct suet_domain *domain)
{
	size_t max_ses_size = suet_pds_max_ses_size(domain);
	int ret;

	if (max_ses_size <=
	    sizeof(struct ses_req_hdr) + sizeof(struct ses_msg_amo_hdr))
		return -FI_EMSGSIZE;
	domain->max_inline_msg = max_ses_size;
	domain->max_inline_rma = max_ses_size;
	domain->max_pkt_sz = max_ses_size - sizeof(struct ses_req_hdr);
	domain->max_inline_atom =
		domain->max_pkt_sz - sizeof(struct ses_msg_amo_hdr);

	dlist_init(&domain->ses.rx_ctx_list);
	ret = suet_ses_entry_pool_create(
		&domain->ses, sizeof(struct suet_ses_tx_entry),
		1ULL << SUET_MAX_TX_BITS, &domain->ses.tx_entry_pool);
	if (ret)
		goto err;

	ret = suet_ses_entry_pool_create(
		&domain->ses, sizeof(struct suet_ses_rx_entry),
		1ULL << SUET_MAX_RX_BITS, &domain->ses.rx_entry_pool);
	if (ret)
		goto err;

	ret = ofi_bufpool_create(
		&domain->ses.unexp_msg_pool, sizeof(struct suet_ses_unexp_msg),
		SUET_BUF_POOL_ALIGNMENT, suet_env.unexp_msg_pool_size, 0, 0);
	if (ret)
		goto err;
	return 0;
err:
	suet_ses_cleanup(domain);
	return ret;
}

static struct suet_ses_tx_entry *suet_ses_tx_entry_alloc(struct suet_ep *ep)
{
	struct suet_domain *domain = suet_ep_domain(ep);
	struct suet_ses_tx_entry *tx_entry;

	tx_entry = ofi_ibuf_alloc(domain->ses.tx_entry_pool);
	if (!tx_entry)
		return NULL;

	tx_entry->ep = ep;

	return tx_entry;
}

static struct suet_ses_rx_entry *suet_ses_rx_entry_alloc(struct suet_ep *ep)
{
	struct suet_domain *domain = suet_ep_domain(ep);
	struct suet_ses_rx_entry *rx_entry;

	rx_entry = ofi_ibuf_alloc(domain->ses.rx_entry_pool);
	if (!rx_entry)
		return NULL;

	rx_entry->ep = ep;
	return rx_entry;
}

/*
 * Carve a [offset, offset+len) window out of tx_entry->iov[] into
 * out_iov/out_desc for a single zero-copy segment.
 */
static inline size_t suet_tx_iov_find_slice(struct suet_ses_tx_entry *tx_entry,
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

static inline int suet_ses_verify_mr_iov(struct suet_ep *ep,
					 const struct ses_req_hdr *ses_hdr,
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

static inline const struct suet_ses_rx_packet *
suet_ses_unexp_msg_get_som_pkt(struct suet_ses_unexp_msg *unexp_msg)
{
	struct suet_ses_pkt_entry *entry = container_of(
		unexp_msg->pkt_list.next, struct suet_ses_pkt_entry, entry);

	return &entry->pkt;
}

static void suet_ses_unexp_msg_append_pkt(struct suet_ses_unexp_msg *unexp_msg,
					  void *pkt_ctx,
					  const struct suet_ses_rx_packet *pkt)
{
	struct suet_ses_pkt_entry *entry = pkt_ctx;

	entry->pkt = *pkt;
	dlist_insert_tail(&entry->entry, &unexp_msg->pkt_list);
}

static const struct suet_ses_rx_packet *
suet_ses_unexp_msg_pop_pkt(struct suet_ses_unexp_msg *unexp_msg)
{
	struct suet_ses_pkt_entry *entry;

	dlist_pop_front(&unexp_msg->pkt_list, struct suet_ses_pkt_entry, entry,
			entry);
	dlist_init(&entry->entry);
	return &entry->pkt;
}

static inline int suet_match_addr(int addr, int match_addr)
{
	return (addr == SUET_ADDR_INVALID || addr == match_addr);
}

static inline int suet_match_tag(uint64_t tag, uint64_t ignore,
				 uint64_t match_tag)
{
	return ((tag | ignore) == (match_tag | ignore));
}

static void suet_ses_drain_rx_entry_list(struct dlist_entry *list, int err);
static void suet_ses_tx_entry_free(struct suet_ses_tx_entry *tx_entry);

static void suet_ses_rx_put(struct suet_ses_rx_ctx *rx)
{
	if (--rx->refs)
		return;
	dlist_remove(&rx->entry);
	free(rx);
}

void *suet_ses_rx_open(struct suet_domain *domain, void *pds_ctx, int peer_idx)
{
	struct suet_ses_rx_ctx *rx = calloc(1, sizeof(*rx));

	if (!rx)
		return NULL;
	rx->domain = domain;
	rx->pds_ctx = pds_ctx;
	rx->peer_idx = peer_idx;
	rx->refs = 1;
	dlist_init(&rx->rx_list);
	dlist_insert_tail(&rx->entry, &domain->ses.rx_ctx_list);
	return rx;
}

void suet_ses_rx_close(void *ses_ctx)
{
	struct suet_ses_rx_ctx *rx = ses_ctx;

	rx->pds_ctx = NULL;
	rx->curr_unexp = NULL;
	suet_ses_drain_rx_entry_list(&rx->rx_list, FI_ECANCELED);
	suet_ses_rx_put(rx);
}

bool suet_ses_rx_busy(void *ses_ctx)
{
	struct suet_ses_rx_ctx *rx = ses_ctx;

	return !dlist_empty(&rx->rx_list);
}

static void suet_ses_unexp_msg_free(struct suet_ses_unexp_msg *unexp_msg)
{
	struct suet_ses_rx_ctx *rx = unexp_msg->ses_ctx;

	if (rx->curr_unexp == unexp_msg)
		rx->curr_unexp = NULL;
	suet_pds_response_cancel(unexp_msg->gtd_del_resp);
	dlist_remove(&unexp_msg->entry);
	ofi_buf_free(unexp_msg);
	suet_ses_rx_put(rx);
}

static void suet_ses_unexp_msg_cleanup(struct suet_ses_unexp_msg *unexp_msg)
{
	const struct suet_ses_rx_packet *pkt;

	while (!dlist_empty(&unexp_msg->pkt_list)) {
		pkt = suet_ses_unexp_msg_pop_pkt(unexp_msg);
		suet_pds_rx_release(pkt->handle);
	}
	suet_ses_unexp_msg_free(unexp_msg);
}

void suet_ses_unexp_msg_list_cleanup(struct dlist_entry *list)
{
	struct suet_ses_unexp_msg *unexp_msg;

	while (!dlist_empty(list)) {
		dlist_pop_front(list, struct suet_ses_unexp_msg, unexp_msg,
				entry);
		suet_ses_unexp_msg_cleanup(unexp_msg);
	}
}

void suet_ses_ep_cleanup(struct suet_ep *ep)
{
	struct suet_ses_rx_ctx *rx;
	struct suet_ses_tx_entry *tx_entry;
	struct suet_ses_rx_entry *rx_entry;
	struct dlist_entry *tmp;

	dlist_foreach_container_safe (&ep->tx_list, struct suet_ses_tx_entry,
				      tx_entry, entry, tmp)
		suet_ses_tx_entry_free(tx_entry);
	dlist_foreach_container (&suet_ep_domain(ep)->ses.rx_ctx_list,
				 struct suet_ses_rx_ctx, rx, entry) {
		dlist_foreach_container_safe (&rx->rx_list,
					      struct suet_ses_rx_entry,
					      rx_entry, entry, tmp) {
			if (rx_entry->ep == ep)
				suet_ses_rx_entry_free(rx_entry);
		}
	}
}

static void suet_ses_copy_payload(struct suet_ses_rx_entry *rx_entry,
				  const struct suet_ses_rx_packet *pkt,
				  uint64_t copy_offset)
{
	size_t payload_size = pkt->payload_len;

	if (payload_size > 0) {
		uint64_t done = ofi_copy_to_iov(
			rx_entry->iov, rx_entry->iov_count, copy_offset,
			(void *) pkt->payload, payload_size);
		rx_entry->bytes_copied += done;
	}
}

static void suet_ses_drain_rx_entry_list(struct dlist_entry *list, int err)
{
	struct fi_cq_err_entry err_entry;
	struct suet_ses_rx_entry *rx_entry;
	struct util_cq *util_cq;
	int ret;

	while (!dlist_empty(list)) {
		dlist_pop_front(list, struct suet_ses_rx_entry, rx_entry,
				entry);
		memset(&err_entry, 0, sizeof(err_entry));
		err_entry.op_context = rx_entry->cq_entry.op_context;
		err_entry.flags = rx_entry->cq_entry.flags;
		err_entry.err = err;
		err_entry.prov_errno = 0;
		util_cq = &suet_ep_rx_cq(rx_entry->ep)->util_cq;
		ret = ofi_cq_write_error(util_cq, &err_entry);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
				"could not write error entry\n");
		suet_ses_rx_entry_free(rx_entry);
	}
}

static void suet_ses_tx_entry_free(struct suet_ses_tx_entry *tx_entry)
{
	suet_pds_tx_cancel(tx_entry->pds_ctx);
	tx_entry->pds_ctx = NULL;
	if (tx_entry->zc_internal_mrs[0])
		suet_mr_closev_internal(tx_entry->zc_internal_mrs,
					tx_entry->iov_count);
	dlist_remove(&tx_entry->entry);
	ofi_ibuf_free(tx_entry);
}

static void suet_ses_tx_entry_complete(struct suet_ses_tx_entry *tx_entry)
{
	struct suet_ep *ep = tx_entry->ep;
	struct suet_cq *tx_cq = suet_ep_tx_cq(ep);

	if (!(tx_entry->flags & SUET_NO_TX_COMP))
		tx_cq->write_fn(tx_cq, &tx_entry->cq_entry);

	ofi_ep_tx_cntr_inc_func(&ep->util_ep, (uint8_t) tx_entry->op);

	suet_ses_tx_entry_free(tx_entry);
}

struct suet_ses_tx_entry *
suet_ses_tx_entry_init_common(struct suet_ep *ep, fi_addr_t addr, uint32_t op,
			      const struct iovec *iov, void **desc,
			      size_t iov_count, uint64_t tag, uint64_t data,
			      uint32_t suet_flags, void *context)
{
	struct suet_ses_tx_entry *tx_entry;
	struct suet_domain *domain = suet_ep_domain(ep);
	int peer_idx;
	size_t i;

	peer_idx = suet_av_peer_idx_from_usr_av_addr(suet_ep_av(ep), addr);
	if (!peer_idx)
		return NULL;

	tx_entry = suet_ses_tx_entry_alloc(ep);
	if (!tx_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "could not get tx entry\n");
		return NULL;
	}

	tx_entry->pds_ctx = suet_pds_tx_alloc(domain, peer_idx, tx_entry);
	if (!tx_entry->pds_ctx) {
		ofi_ibuf_free(tx_entry);
		return NULL;
	}
	tx_entry->op = op;
	tx_entry->flags = suet_flags;
	tx_entry->iov_count = (uint8_t) iov_count;
	memcpy(&tx_entry->iov[0], iov, sizeof(*iov) * iov_count);
	memset(tx_entry->zc_desc, 0, sizeof(tx_entry->zc_desc));
	memset(tx_entry->zc_internal_mrs, 0, sizeof(tx_entry->zc_internal_mrs));

	tx_entry->cq_entry.op_context = context;
	tx_entry->cq_entry.len = ofi_total_iov_len(iov, iov_count);
	tx_entry->cq_entry.buf = iov[0].iov_base;
	tx_entry->cq_entry.flags = ofi_tx_cq_flags(op);
	tx_entry->cq_entry.tag = tag;
	tx_entry->cq_entry.data = data;

	if (op == ofi_op_msg || op == ofi_op_tagged || op == ofi_op_write) {
		if (desc) {
			for (i = 0; i < iov_count; i++) {
				tx_entry->zc_desc[i] = suet_mr_desc(desc[i]);
			}
		} else if (tx_entry->cq_entry.len >
			   domain->zc_mr_reg_threshold) {
			/* Large unregistered send: do on-demand internal
			 * registration. Failure falls back to memcpy path. */
			int mr_ret = suet_mr_regv_internal(
				domain, iov, iov_count, tx_entry->cq_entry.len,
				FI_SEND, tx_entry->zc_internal_mrs,
				tx_entry->zc_desc);
			if (mr_ret) {
				FI_WARN(&suet_prov, FI_LOG_EP_DATA,
					"on-demand MR reg failed (%d); using "
					"copy path\n",
					mr_ret);
				memset(tx_entry->zc_internal_mrs, 0,
				       sizeof(tx_entry->zc_internal_mrs));
			}
		}
	}

	/* stamp constant SES fields */
	ses_req_ctrl_init(&tx_entry->cached_hdr.ses,
			  ofi_op_to_ses_req_opcode(op), 0, 0, 0, 0);
	ses_set_buffer_offset(&tx_entry->cached_hdr.ses, 0);
	ses_set_match_bits(&tx_entry->cached_hdr.ses, tag);

	dlist_insert_tail(&tx_entry->entry, &ep->tx_list);

	return tx_entry;
}

static int suet_ses_match_rx_entry(struct dlist_entry *item, const void *arg)
{
	struct suet_ses_msg_match_attr *attr =
		(struct suet_ses_msg_match_attr *) arg;
	struct suet_ses_rx_entry *rx_entry;

	rx_entry = container_of(item, struct suet_ses_rx_entry, entry);

	return suet_match_addr(rx_entry->peer_idx, attr->peer_idx);
}

static int suet_ses_match_tagged_rx_entry(struct dlist_entry *item,
					  const void *arg)
{
	struct suet_ses_msg_match_attr *attr =
		(struct suet_ses_msg_match_attr *) arg;
	struct suet_ses_rx_entry *rx_entry;

	rx_entry = container_of(item, struct suet_ses_rx_entry, entry);

	return suet_match_addr(rx_entry->peer_idx, attr->peer_idx) &&
	       suet_match_tag(rx_entry->cq_entry.tag, rx_entry->ignore,
			      attr->tag);
}

void suet_ses_rx_entry_free(struct suet_ses_rx_entry *rx_entry)
{
	dlist_remove(&rx_entry->entry);
	ofi_ibuf_free(rx_entry);
}

static void suet_ses_rx_entry_complete(struct suet_ses_rx_entry *rx_entry)
{
	struct suet_ep *ep = rx_entry->ep;
	struct fi_cq_err_entry err_entry;
	struct suet_cq *rx_cq = suet_ep_rx_cq(ep);
	int ret;

	if (rx_entry->bytes_copied != rx_entry->cq_entry.len) {
		memset(&err_entry, 0, sizeof(err_entry));
		err_entry.op_context = rx_entry->cq_entry.op_context;
		err_entry.flags = rx_entry->cq_entry.flags;
		err_entry.len = rx_entry->bytes_copied;
		err_entry.err = FI_ETRUNC;
		err_entry.prov_errno = 0;
		ret = ofi_cq_write_error(&rx_cq->util_cq, &err_entry);
		if (ret) {
			FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
				"could not write error entry\n");
			return;
		}
		goto out;
	}

	if (rx_entry->cq_entry.flags & FI_REMOTE_CQ_DATA ||
	    (!(rx_entry->flags & SUET_NO_RX_COMP) &&
	     rx_entry->cq_entry.flags & FI_RECV))
		rx_cq->write_fn(rx_cq, &rx_entry->cq_entry);

	ofi_ep_rx_cntr_inc_func(&ep->util_ep, (uint8_t) rx_entry->op);

out:
	suet_ses_rx_entry_free(rx_entry);
}

struct suet_ses_rx_entry *
suet_ses_rx_entry_init(struct suet_ep *ep, const struct iovec *iov,
		       size_t iov_count, uint64_t tag, uint64_t ignore,
		       void *context, int peer_idx, uint32_t op, uint32_t flags)
{
	struct suet_ses_rx_entry *rx_entry;

	rx_entry = suet_ses_rx_entry_alloc(ep);
	if (!rx_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "could not get rx entry\n");
		return NULL;
	}

	rx_entry->peer_idx = peer_idx;
	rx_entry->flags = flags;
	rx_entry->bytes_copied = 0;
	rx_entry->pkts_received = 0;
	rx_entry->iov_count = (uint8_t) iov_count;
	rx_entry->op = op;
	rx_entry->ignore = ignore;

	memcpy(rx_entry->iov, iov, sizeof(*rx_entry->iov) * iov_count);

	rx_entry->cq_entry.op_context = context;
	rx_entry->cq_entry.len = ofi_total_iov_len(iov, iov_count);
	rx_entry->cq_entry.buf = iov_count ? iov[0].iov_base : NULL;
	rx_entry->cq_entry.tag = tag;

	rx_entry->cq_entry.flags = ofi_rx_cq_flags(op);
	dlist_init(&rx_entry->entry);

	return rx_entry;
}

static int suet_ses_match_unexp_msg(struct dlist_entry *item, const void *arg)
{
	struct suet_ses_msg_match_attr *attr =
		(struct suet_ses_msg_match_attr *) arg;
	struct suet_ses_unexp_msg *unexp_msg =
		container_of(item, struct suet_ses_unexp_msg, entry);
	const struct suet_ses_rx_packet *pkt =
		suet_ses_unexp_msg_get_som_pkt(unexp_msg);

	if (!suet_match_addr(attr->peer_idx, unexp_msg->peer_idx))
		return 0;

	if (!ses_req_opcode_is_tagged(ses_req_ctrl_get_opcode(pkt->hdr)))
		return 1;

	return suet_match_tag(attr->tag, attr->ignore,
			      ses_get_match_bits(pkt->hdr));
}

static struct suet_ses_unexp_msg *
suet_ses_check_unexp_list(struct dlist_entry *list, int peer_idx, uint64_t tag,
			  uint64_t ignore)
{
	struct suet_ses_msg_match_attr attr;
	struct dlist_entry *match;

	attr.peer_idx = peer_idx;
	attr.tag = tag;
	attr.ignore = ignore;

	match = dlist_find_first_match(list, &suet_ses_match_unexp_msg, &attr);
	if (!match)
		return NULL;

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL, "Matched to unexp msg entry\n");

	return container_of(match, struct suet_ses_unexp_msg, entry);
}

static void suet_ses_req_som_unpack_generic_op_metadata(
	struct suet_ses_rx_ctx *rx, struct suet_ses_rx_entry *rx_entry,
	const struct suet_ses_rx_packet *pkt)
{
	const struct ses_req_hdr *ses_hdr = pkt->hdr;
	uint8_t ses_opcode = ses_req_ctrl_get_opcode(ses_hdr);

	rx->curr_rx_id = rx_entry->rx_id;

	if (ses_req_ctrl_has_hd(ses_hdr)) {
		rx_entry->cq_entry.flags |= FI_REMOTE_CQ_DATA;
		rx_entry->cq_entry.data = ses_hd_get_completion_data(ses_hdr);
	}

	rx_entry->peer_idx = rx->peer_idx;

	if (ses_req_opcode_is_tagged(ses_opcode))
		rx_entry->cq_entry.tag = ses_get_match_bits(ses_hdr);

	if (ses_get_request_length(ses_hdr) <=
	    (uint32_t) suet_ep_domain(rx_entry->ep)->max_pkt_sz) {
		rx_entry->num_pkts = 1;
		return;
	}

	rx_entry->num_pkts =
		ofi_div_ceil(ses_get_request_length(ses_hdr),
			     suet_ep_domain(rx_entry->ep)->max_pkt_sz);
}

void suet_ses_complete_unexp_msg(struct suet_ep *ep,
				 struct suet_ses_rx_entry *rx_entry,
				 struct suet_ses_unexp_msg *unexp_msg)
{
	const struct suet_ses_rx_packet *pkt =
		suet_ses_unexp_msg_get_som_pkt(unexp_msg);
	const struct suet_ses_rx_packet *pkt_entry;
	struct suet_domain *suet_domain = suet_ep_domain(ep);
	struct suet_ses_rx_ctx *rx;
	uint16_t curr_id;
	uint64_t request_length = ses_get_request_length(pkt->hdr);
	int is_single_pkt =
		request_length <= (uint64_t) suet_domain->max_pkt_sz;
	uint64_t num_segs = 0;
	struct suet_ses_resp resp = {
		.ses_opcode = UET_RESPONSE,
		.ses_rc = RC_OK,
		.list = UET_OVERFLOW,
		.message_id = ses_get_message_id(pkt->hdr),
		.modified_length = (uint32_t) request_length,
	};
	struct suet_pds_ses_resp_entry *response;

	rx = unexp_msg->ses_ctx;
	curr_id = rx->curr_rx_id;

	suet_ses_req_som_unpack_generic_op_metadata(rx, rx_entry, pkt);

	while (!dlist_empty(&unexp_msg->pkt_list)) {
		uint64_t offset;

		pkt_entry = suet_ses_unexp_msg_pop_pkt(unexp_msg);
		offset = ses_req_ctrl_is_som(pkt_entry->hdr) ?
				 0 :
				 ses_hd_get_message_offset(pkt_entry->hdr);
		suet_ses_copy_payload(rx_entry, pkt_entry, offset);
		rx_entry->pkts_received++;
		suet_pds_rx_release(pkt_entry->handle);
		num_segs++;
	}

	if (rx_entry->pkts_received >= rx_entry->num_pkts)
		suet_ses_rx_entry_complete(rx_entry);

	if (rx->curr_unexp) {
		if (is_single_pkt ||
		    num_segs == ofi_div_ceil(request_length,
					     suet_domain->max_pkt_sz))
			rx->curr_rx_id = curr_id;
		else
			rx->curr_unexp = NULL;
	}

	response = unexp_msg->gtd_del_resp;
	unexp_msg->gtd_del_resp = NULL;
	suet_ses_unexp_msg_free(unexp_msg);
	suet_pds_response_complete(response, &resp);
}

int suet_ses_peek_recv(struct suet_ep *suet_ep, int peer_idx, uint64_t tag,
		       uint64_t ignore, void *context, uint64_t flags,
		       struct dlist_entry *unexp_list)
{
	struct suet_ses_unexp_msg *unexp_msg;
	const struct suet_ses_rx_packet *pkt;

	suet_domain_progress(suet_ep_domain(suet_ep));

	unexp_msg =
		suet_ses_check_unexp_list(unexp_list, peer_idx, tag, ignore);
	if (!unexp_msg) {
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL, "Message not found\n");
		return ofi_cq_write_error_peek(suet_ep->util_ep.rx_cq, tag,
					       context);
	}
	FI_DBG(&suet_prov, FI_LOG_EP_CTRL, "Message found\n");

	if (flags & FI_DISCARD)
		return suet_ses_discard_recv(suet_ep, context, unexp_msg);

	if (flags & FI_CLAIM) {
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "Marking message for CLAIM\n");
		((struct fi_context *) context)->internal[0] = unexp_msg;
		dlist_remove(&unexp_msg->entry);
	}

	pkt = suet_ses_unexp_msg_get_som_pkt(unexp_msg);
	return ofi_cq_write(suet_ep->util_ep.rx_cq, context,
			    FI_TAGGED | FI_RECV,
			    ses_get_request_length(pkt->hdr), NULL,
			    ses_req_ctrl_has_hd(pkt->hdr) ?
				    ses_hd_get_completion_data(pkt->hdr) :
				    0,
			    ses_get_match_bits(pkt->hdr));
}

static struct suet_ses_rx_entry *
suet_ses_progress_multi_recv(struct suet_ep *ep,
			     struct suet_ses_rx_entry *rx_entry,
			     size_t total_size)
{
	struct suet_ses_rx_entry *dup_entry;
	size_t left;
	uint32_t dup_id;

	left = rx_entry->iov[0].iov_len - total_size;

	if (left < ep->min_multi_recv_size) {
		rx_entry->cq_entry.flags |= FI_MULTI_RECV;
		return NULL;
	}

	dup_entry = suet_ses_rx_entry_alloc(ep);
	if (!dup_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "could not get rx entry\n");
		return NULL;
	}
	dup_id = dup_entry->rx_id;
	memcpy(dup_entry, rx_entry, sizeof(*rx_entry));
	dup_entry->rx_id = (uint16_t) dup_id;
	dup_entry->iov[0].iov_base = rx_entry->iov[0].iov_base;
	dup_entry->iov[0].iov_len = total_size;
	dup_entry->cq_entry.len = total_size;

	rx_entry->iov[0].iov_base =
		(char *) rx_entry->iov[0].iov_base + total_size;
	rx_entry->cq_entry.buf = rx_entry->iov[0].iov_base;
	rx_entry->iov[0].iov_len = left;
	rx_entry->cq_entry.len = left;

	return dup_entry;
}

int suet_ses_progress_unexp_list(struct suet_ep *ep,
				 struct dlist_entry *unexp_list,
				 struct dlist_entry *rx_list,
				 struct suet_ses_rx_entry *rx_entry)
{
	struct suet_ses_rx_entry *progress_entry, *dup_entry = NULL;
	struct suet_ses_unexp_msg *unexp_msg;
	const struct suet_ses_rx_packet *pkt;
	size_t total_size;

	while (!dlist_empty(unexp_list)) {
		unexp_msg = suet_ses_check_unexp_list(
			unexp_list, rx_entry->peer_idx, rx_entry->cq_entry.tag,
			rx_entry->ignore);
		if (!unexp_msg)
			return 0;

		pkt = suet_ses_unexp_msg_get_som_pkt(unexp_msg);
		total_size = ses_get_request_length(pkt->hdr);

		if (rx_entry->flags & SUET_MULTI_RECV)
			dup_entry = suet_ses_progress_multi_recv(ep, rx_entry,
								 total_size);

		progress_entry = dup_entry ? dup_entry : rx_entry;
		progress_entry->cq_entry.len =
			MIN(rx_entry->cq_entry.len, total_size);
		suet_ses_complete_unexp_msg(ep, progress_entry, unexp_msg);
		if (!dup_entry) {
			return 1;
		}
	}

	return 0;
}

int suet_ses_discard_recv(struct suet_ep *suet_ep, void *context,
			  struct suet_ses_unexp_msg *unexp_msg)
{
	const struct suet_ses_rx_packet *pkt =
		suet_ses_unexp_msg_get_som_pkt(unexp_msg);
	uint16_t gtd_msg_id = ses_get_message_id(pkt->hdr);
	uint32_t gtd_req_len = (uint32_t) ses_get_request_length(pkt->hdr);
	struct suet_pds_ses_resp_entry *response;
	struct suet_ses_resp resp = {
		.ses_opcode = UET_RESPONSE,
		.ses_rc = RC_OK,
		.list = UET_OVERFLOW,
		.message_id = gtd_msg_id,
		.modified_length = gtd_req_len,
	};
	int ret;

	/* FI_DISCARD is a libfabric-local concept; the UET-libfabric mapping
	 * (Table 2-28) lists FI_CLAIM/FI_DISCARD as not required, and neither
	 * PDS nor SES define a wire encoding for "delivered but dropped by
	 * the consumer". We finalize the placeholder GD response as a normal
	 * UET_RESPONSE/UET_OVERFLOW so the initiator's tx_entry can complete
	 * (it is gated on a non-NO_RESPONSE ACK).
	 *
	 * Limitation: if SOM was matched but EOM has not yet arrived, future
	 * continuations of the same message_id will re-buffer payload as a
	 * fresh unexpected message. The spec-conformant fix (not implemented)
	 * is a per-stream "cancelled message_id" set checked in the RX path
	 * that drops post-discard segments without buffering. */

	ret = ofi_cq_write(suet_ep->util_ep.rx_cq, context, FI_TAGGED | FI_RECV,
			   0, NULL,
			   ses_req_ctrl_has_hd(pkt->hdr) ?
				   ses_hd_get_completion_data(pkt->hdr) :
				   0,
			   ses_get_match_bits(pkt->hdr));

	response = unexp_msg->gtd_del_resp;
	unexp_msg->gtd_del_resp = NULL;
	suet_ses_unexp_msg_cleanup(unexp_msg);
	suet_pds_response_complete(response, &resp);

	return ret;
}

/*
 * Allocate an unexpected-message bookkeeping struct and link a
 * opaque response reservation with PDS, marked
 * UET_NO_RESPONSE. PDS records the current request PSN and the message
 * packet count as the replay range.
 *
 * Reserving at intake is the spec-clean way to admit a message we cannot
 * yet semantically answer (Spec 3.4.3.6.2.2 / 3.4.4.5): the receiver MUST
 * eventually emit a GD UET_RESPONSE/UET_OVERFLOW for it, and that response
 * MUST NOT fail to persist. If the pool is exhausted at intake, we refuse
 * to admit -- SES reports a local allocation failure and PDS
 * NACKs the request without advancing expected_rx_psn so the initiator
 * RTO-retransmits.
 */
static struct suet_ses_unexp_msg *
suet_ses_init_unexp_msg(struct suet_ep *ep, struct suet_ses_rx_ctx *rx,
			const struct suet_ses_rx_packet *som_pkt,
			struct suet_ses_rx_dispatch_result *resp)
{
	struct suet_domain *domain = suet_ep_domain(ep);
	struct suet_ses_unexp_msg *unexp_msg;
	struct suet_pds_ses_resp_entry *reserved_resp;
	uint32_t request_length = ses_get_request_length(som_pkt->hdr);
	uint32_t num_pkts = ofi_div_ceil(request_length, domain->max_pkt_sz);
	struct suet_ses_resp placeholder = {
		.ses_opcode = UET_NO_RESPONSE,
		.ses_rc = RC_OK,
		.list = UET_EXPECTED,
		.message_id = ses_get_message_id(som_pkt->hdr),
		.modified_length = request_length,
	};

	/* 0-byte messages still produce one SOM+EOM packet. */
	reserved_resp = suet_pds_response_reserve(
		domain, rx->pds_ctx, &placeholder,
		(uint16_t) (num_pkts == 0 ? 1 : num_pkts));
	if (!reserved_resp) {
		/* Spec 3.5.12.7: no GD response slot available. */
		resp->status = -FI_ENOMEM;
		return NULL;
	}

	unexp_msg = ofi_buf_alloc(domain->ses.unexp_msg_pool);
	if (!unexp_msg) {
		suet_pds_response_cancel(reserved_resp);
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"SES SOM: no rx entry match and unexpected "
			"alloc failed\n");
		resp->resp.ses_rc = RC_NO_MATCH;
		resp->resp.ses_opcode = UET_RESPONSE;
		return NULL;
	}

	dlist_init(&unexp_msg->pkt_list);
	unexp_msg->gtd_del_resp = reserved_resp;
	unexp_msg->ses_ctx = rx;
	rx->refs++;

	return unexp_msg;
}

static struct suet_ses_rx_entry *
suet_ses_match_msg_rx_entry(struct suet_ep *ep, struct suet_ses_rx_ctx *rx,
			    void *pkt_ctx, const struct suet_ses_rx_packet *pkt,
			    struct suet_ses_rx_dispatch_result *resp)
{
	struct suet_ses_rx_entry *rx_entry, *dup_entry;
	struct dlist_entry *rx_list;
	struct dlist_entry *unexp_list;
	struct dlist_entry *match;
	struct suet_ses_msg_match_attr attr;
	size_t total_size;

	attr.peer_idx = rx->peer_idx;

	if (ses_req_opcode_is_tagged(ses_req_ctrl_get_opcode(pkt->hdr))) {
		unexp_list = &ep->unexp_tag_list;
		attr.tag = ses_get_match_bits(pkt->hdr);
		rx_list = &ep->rx_tag_list;
		match = dlist_find_first_match(rx_list,
					       &suet_ses_match_tagged_rx_entry,
					       (void *) &attr);
	} else {
		unexp_list = &ep->unexp_list;
		attr.tag = 0;
		rx_list = &ep->rx_list;
		match = dlist_find_first_match(
			rx_list, &suet_ses_match_rx_entry, (void *) &attr);
	}

	if (!match) {
		assert(!rx->curr_unexp);
		rx->curr_unexp = suet_ses_init_unexp_msg(ep, rx, pkt, resp);
		if (rx->curr_unexp) {
			rx->curr_unexp->peer_idx = rx->peer_idx;
			suet_ses_unexp_msg_append_pkt(rx->curr_unexp, pkt_ctx,
						      pkt);
			dlist_insert_tail(&rx->curr_unexp->entry, unexp_list);
		}
		return NULL;
	}

	rx_entry = container_of(match, struct suet_ses_rx_entry, entry);
	total_size = ses_get_request_length(pkt->hdr);

	if (rx_entry->flags & SUET_MULTI_RECV) {
		dup_entry =
			suet_ses_progress_multi_recv(ep, rx_entry, total_size);
		if (!dup_entry)
			goto out;

		dlist_init(&dup_entry->entry);
		rx_entry = dup_entry;
		goto init;
	}

out:
	dlist_remove(&rx_entry->entry);
init:
	rx_entry->cq_entry.len = MIN(rx_entry->cq_entry.len, total_size);
	dlist_insert_tail(&rx_entry->entry, &rx->rx_list);
	return rx_entry;
}

static struct suet_ses_rx_entry *
suet_ses_match_rma_rx_entry(struct suet_ep *ep, struct suet_ses_rx_ctx *rx,
			    const struct suet_ses_rx_packet *pkt,
			    uint32_t ofi_op)
{
	struct suet_ses_rx_entry *rx_entry;
	const struct ses_req_hdr *ses_hdr = pkt->hdr;
	struct iovec iov[1];
	int ret;

	ret = suet_ses_verify_mr_iov(ep, ses_hdr, ofi_op, iov);
	if (ret)
		return NULL;

	rx_entry = suet_ses_rx_entry_init(ep, iov, 1, 0, 0, NULL, rx->peer_idx,
					  ofi_op, 0);
	if (!rx_entry)
		return NULL;

	dlist_insert_tail(&rx_entry->entry, &rx->rx_list);
	return rx_entry;
}

static void suet_ses_execute_atomic_op(struct suet_ep *ep,
				       struct suet_ses_rx_entry *rx_entry,
				       const struct suet_ses_rx_packet *pkt)
{
	const struct ses_msg_amo_hdr *atom_hdr = (const void *) (pkt->hdr + 1);
	const void *src = pkt->payload;
	size_t data_size, len, cnt;
	enum fi_datatype datatype;
	enum fi_op atomic_op;

	(void) ep;

	datatype = ses_type_to_fi_datatype(atom_hdr->atomic_datatype);
	atomic_op = ses_amo_to_fi_op(atom_hdr->atomic_opcode);

	len = rx_entry->iov[0].iov_len;
	rx_entry->bytes_copied = len;

	data_size = ofi_datatype_size(datatype);
	if (!data_size) {
		FI_WARN(&suet_prov, FI_LOG_EP_DATA,
			"Invalid atomic datatype received\n");
		return;
	}
	cnt = len / data_size;

	ofi_atomic_write_handler(atomic_op, datatype, rx_entry->iov[0].iov_base,
				 src, cnt);
}

static struct suet_ses_rx_entry *
suet_ses_req_som_unpack_to_rx_entry(struct suet_ep *ep,
				    struct suet_ses_rx_ctx *rx, void *pkt_ctx,
				    const struct suet_ses_rx_packet *pkt,
				    struct suet_ses_rx_dispatch_result *resp)
{
	uint8_t ses_opcode = ses_req_ctrl_get_opcode(pkt->hdr);
	struct suet_ses_rx_entry *rx_entry;

	if (ses_req_opcode_is_send(ses_opcode) ||
	    ses_req_opcode_is_tagged(ses_opcode)) {
		rx_entry =
			suet_ses_match_msg_rx_entry(ep, rx, pkt_ctx, pkt, resp);
	} else if (ses_req_opcode_is_write(ses_opcode)) {
		rx_entry =
			suet_ses_match_rma_rx_entry(ep, rx, pkt, ofi_op_write);
	} else if (ses_opcode == UET_ATOMIC) {
		rx_entry =
			suet_ses_match_rma_rx_entry(ep, rx, pkt, ofi_op_atomic);
	} else {
		FI_WARN(&suet_prov, FI_LOG_CQ, "Unsupported SES opcode: %d\n",
			ses_opcode);
		return NULL;
	}

	if (rx_entry)
		suet_ses_req_som_unpack_generic_op_metadata(rx, rx_entry, pkt);

	return rx_entry;
}

static struct suet_ep *
suet_ses_lookup_ep_by_ri(struct suet_domain *domain,
			 const struct ses_req_hdr *ses_hdr)
{
	uint16_t ri = ses_get_ri(ses_hdr);
	if (OFI_UNLIKELY(ri >= suet_env.max_eps || !domain->ep_table[ri])) {
		FI_WARN(&suet_prov, FI_LOG_CQ, "No EP for RI=%d\n", ri);
		return NULL;
	}
	return domain->ep_table[ri];
}

static void suet_ses_init_ses_hdr(struct ses_req_hdr *ses_hdr,
				  struct suet_ep *ep,
				  struct suet_ses_tx_entry *tx_entry,
				  int is_som, int is_eom)
{
	ses_req_init(ses_hdr, ses_req_ctrl_get_opcode(ses_hdr), is_som, is_eom,
		     1, tx_entry->flags & SUET_REMOTE_CQ_DATA, tx_entry->tx_id,
		     ses_get_buffer_offset(ses_hdr),
		     ses_get_match_bits(ses_hdr), tx_entry->cq_entry.data,
		     (uint32_t) tx_entry->cq_entry.len, ep->pid_on_fep,
		     ep->resource_index);
}

void suet_ses_init_rma_iov(const struct fi_rma_iov *rma_iov,
			   struct suet_ses_tx_entry *tx_entry)
{
	ses_set_buffer_offset(&tx_entry->cached_hdr.ses, rma_iov[0].addr);
	ses_set_match_bits(&tx_entry->cached_hdr.ses, rma_iov[0].key);
}
bool suet_ses_rx_parse(struct suet_domain *domain, uint8_t hdr_type,
		       const void *data, size_t len,
		       struct suet_ses_rx_packet *pkt)
{
	const struct ses_req_hdr *hdr = data;
	size_t hdr_len = sizeof(*hdr);

	if (hdr_type != UET_HDR_REQUEST_STD || len < hdr_len)
		return false;
	if (ses_req_ctrl_get_opcode(hdr) == UET_ATOMIC) {
		hdr_len += sizeof(struct ses_msg_amo_hdr);
		if (len < hdr_len ||
		    len - hdr_len < ses_get_request_length(hdr))
			return false;
	}
	if (!suet_ses_lookup_ep_by_ri(domain, hdr))
		return false;
	pkt->hdr = hdr;
	pkt->payload = (const char *) data + hdr_len;
	pkt->payload_len = len - hdr_len;
	return true;
}

void suet_ses_default_response(const struct ses_req_hdr *hdr,
			       struct suet_ses_resp *resp)
{
	resp->ses_opcode = UET_DEFAULT_RESPONSE;
	resp->ses_rc = RC_OK;
	resp->list = UET_EXPECTED;
	resp->message_id = ses_get_message_id(hdr);
	resp->modified_length = ses_get_request_length(hdr);
}

static bool suet_ses_response_is_gtd(const struct suet_ses_resp *resp)
{
	return resp->ses_opcode != UET_DEFAULT_RESPONSE &&
	       resp->ses_opcode != UET_NO_RESPONSE;
}

void suet_ses_receive(void *ses_ctx, void *pkt_ctx,
		      const struct suet_ses_rx_packet *pkt,
		      struct suet_ses_rx_dispatch_result *resp)
{
	struct suet_ses_rx_ctx *rx = ses_ctx;
	struct suet_domain *domain = rx->domain;
	struct suet_ep *ep = suet_ses_lookup_ep_by_ri(domain, pkt->hdr);
	struct suet_ses_rx_entry *rx_entry = NULL;
	bool is_som = ses_req_ctrl_is_som(pkt->hdr);

	memset(resp, 0, sizeof(*resp));
	suet_ses_default_response(pkt->hdr, &resp->resp);
	resp->ack_now = is_som || ses_req_ctrl_is_eom(pkt->hdr);
	if (!ep)
		return;
	if (is_som) {
		uint8_t ses_opcode = ses_req_ctrl_get_opcode(pkt->hdr);

		resp->resp.ses_rc = RC_OK;
		rx_entry = suet_ses_req_som_unpack_to_rx_entry(ep, rx, pkt_ctx,
							       pkt, resp);
		if (!rx_entry) {
			if (!resp->status && rx->curr_unexp) {
				resp->resp.ses_opcode = UET_NO_RESPONSE;
				resp->pkt_retained = true;
			}
		} else if (ses_opcode == UET_ATOMIC) {
			suet_ses_execute_atomic_op(ep, rx_entry, pkt);
			rx_entry->pkts_received++;
		} else {
			suet_ses_copy_payload(rx_entry, pkt, 0);
			rx_entry->pkts_received++;
		}
	} else if (rx->curr_unexp) {
		suet_ses_unexp_msg_append_pkt(rx->curr_unexp, pkt_ctx, pkt);
		resp->resp.ses_opcode = UET_NO_RESPONSE;
		resp->pkt_retained = true;
		resp->resp.ses_rc = RC_OK;
	} else {
		rx_entry = ofi_bufpool_get_ibuf(domain->ses.rx_entry_pool,
						rx->curr_rx_id);
		suet_ses_copy_payload(rx_entry, pkt,
				      ses_hd_get_message_offset(pkt->hdr));
		rx_entry->pkts_received++;
		resp->resp.ses_rc = RC_OK;
	}

	resp->ack_now |= resp->resp.ses_rc != RC_OK;
	resp->gtd_del = suet_ses_response_is_gtd(&resp->resp);
	resp->accepted = !resp->status && resp->resp.ses_rc == RC_OK;
	resp->completion = rx_entry;
}

void suet_ses_rx_commit(void *ses_ctx,
			const struct suet_ses_rx_dispatch_result *resp)
{
	struct suet_ses_rx_ctx *rx = ses_ctx;
	struct suet_ses_rx_entry *entry = resp->completion;

	/* For unexpected messages, EOM is recognized by the retained packet. */
	if (rx->curr_unexp && !dlist_empty(&rx->curr_unexp->pkt_list)) {
		struct suet_ses_pkt_entry *last =
			container_of(rx->curr_unexp->pkt_list.prev,
				     struct suet_ses_pkt_entry, entry);
		const struct suet_ses_rx_packet *pkt = &last->pkt;
		if (ses_req_ctrl_is_eom(pkt->hdr))
			rx->curr_unexp = NULL;
	}
	if (entry && entry->pkts_received >= entry->num_pkts)
		suet_ses_rx_entry_complete(entry);
}

void suet_ses_submit(struct suet_ses_tx_entry *entry)
{
	suet_ses_init_ses_hdr(&entry->cached_hdr.ses, entry->ep, entry, 1, 1);
	suet_pds_tx_submit(entry->pds_ctx, entry->num_pkts);
}

int suet_ses_prepare_tx(void *context, uint32_t segment, void *hdr,
			size_t hdr_capacity, struct suet_ses_tx_segment *tx)
{
	struct suet_ses_tx_entry *entry = context;
	struct suet_domain *domain = suet_ep_domain(entry->ep);
	struct ses_req_hdr *ses = hdr;
	size_t offset = (size_t) segment * domain->max_pkt_sz;
	size_t len;

	if (segment >= entry->num_pkts || offset > entry->cq_entry.len)
		return -FI_EINVAL;
	len = MIN(domain->max_pkt_sz, entry->cq_entry.len - offset);
	if (entry->hdr_len > hdr_capacity ||
	    entry->iov_count > tx->iov_capacity)
		return -FI_ETOOSMALL;

	memcpy(hdr, &entry->cached_hdr, entry->hdr_len);
	ses_req_ctrl_set_som_eom(ses, segment == 0,
				 offset + len >= entry->cq_entry.len);
	if (segment)
		ses_hd_set_cont(ses, (uint32_t) offset, (uint16_t) len);
	tx->iov_count =
		suet_tx_iov_find_slice(entry, offset, len, tx->iov, tx->desc);
	tx->hdr_len = entry->hdr_len;
	tx->hdr_type = UET_HDR_REQUEST_STD;
	tx->payload_len = len;
	tx->zero_copy = entry->zc_desc[0] != NULL;
	return 0;
}

void suet_ses_tx_done(void *context, int err, int prov_errno)
{
	struct suet_ses_tx_entry *entry = context;
	struct fi_cq_err_entry cq_err = {0};

	entry->pds_ctx = NULL;
	if (!err) {
		suet_ses_tx_entry_complete(entry);
		return;
	}
	cq_err.op_context = entry->cq_entry.op_context;
	cq_err.flags = entry->cq_entry.flags;
	cq_err.err = err;
	cq_err.prov_errno = prov_errno;
	if (ofi_cq_write_error(&suet_ep_tx_cq(entry->ep)->util_cq, &cq_err))
		FI_WARN(&suet_prov, FI_LOG_CQ, "could not write error entry\n");
	suet_ses_tx_entry_free(entry);
}

bool suet_ses_tx_response(void *context, const struct ses_resp_hdr *resp)
{
	struct suet_ses_tx_entry *entry = context;
	struct suet_domain *domain = suet_ep_domain(entry->ep);

	if (entry->tx_id != ses_resp_get_message_id(resp))
		return false;
	if (ses_resp_ctrl_get_rc(resp) != RC_OK) {
		domain->counters.ses_err_completions++;
		domain->counters.tx_cq_errors++;
		suet_pds_tx_cancel(entry->pds_ctx);
		suet_ses_tx_done(entry, FI_EINVAL, ses_resp_ctrl_get_rc(resp));
	}
	return true;
}

bool suet_ses_response_is_error(const struct ses_resp_hdr *resp)
{
	return ses_resp_ctrl_get_rc(resp) != RC_OK;
}
