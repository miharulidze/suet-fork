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
 * SES (Semantic Sublayer) protocol code: TX post, RX dispatch, x_entry
 * completion, atomic exec, message matching.
 */

#include "suet.h"

void suet_ses_drain_x_entry_list(struct dlist_entry *list, bool is_tx, int err)
{
	struct fi_cq_err_entry err_entry;
	struct suet_x_entry *x_entry;
	struct util_cq *util_cq;
	int ret;

	while (!dlist_empty(list)) {
		dlist_pop_front(list, struct suet_x_entry, x_entry, entry);
		memset(&err_entry, 0, sizeof(err_entry));
		err_entry.op_context = x_entry->cq_entry.op_context;
		err_entry.flags = x_entry->cq_entry.flags;
		err_entry.err = err;
		err_entry.prov_errno = 0;
		util_cq = is_tx ? &suet_ep_tx_cq(x_entry->ep)->util_cq :
				  &suet_ep_rx_cq(x_entry->ep)->util_cq;
		ret = ofi_cq_write_error(util_cq, &err_entry);
		if (ret)
			FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
				"could not write error entry\n");
		if (is_tx)
			suet_ses_tx_entry_free(x_entry);
		else
			suet_ses_rx_entry_free(x_entry);
	}
}

void suet_ses_tx_entry_free(struct suet_x_entry *tx_entry)
{
	if (tx_entry->zc_internal_mrs[0])
		suet_mr_closev_internal(tx_entry->zc_internal_mrs,
					tx_entry->iov_count);
	dlist_remove(&tx_entry->entry);
	ofi_ibuf_free(tx_entry);
}

void suet_ses_tx_entry_complete(struct suet_x_entry *tx_entry)
{
	struct suet_ep *ep = tx_entry->ep;
	struct suet_cq *tx_cq = suet_ep_tx_cq(ep);

	if (!(tx_entry->flags & SUET_NO_TX_COMP))
		tx_cq->write_fn(tx_cq, &tx_entry->cq_entry);

	ofi_ep_tx_cntr_inc_func(&ep->util_ep, (uint8_t) tx_entry->op);

	suet_ses_tx_entry_free(tx_entry);
}

struct suet_x_entry *
suet_ses_tx_entry_init_common(struct suet_ep *ep, fi_addr_t addr, uint32_t op,
			      const struct iovec *iov, void **desc,
			      size_t iov_count, uint64_t tag, uint64_t data,
			      uint32_t suet_flags, void *context)
{
	struct suet_x_entry *tx_entry;
	struct suet_domain *domain = suet_ep_domain(ep);
	int peer_idx;
	fi_addr_t dg_av_addr;
	struct suet_ipdc *ipdc;
	size_t i;

	peer_idx = suet_av_peer_idx_from_usr_av_addr(suet_ep_av(ep), addr);
	if (!peer_idx)
		return NULL;

	dg_av_addr = suet_domain_dg_av_get_addr_by_peer_idx(domain, peer_idx);

	ipdc = suet_pds_assign_ipdc(domain, dg_av_addr);
	if (!ipdc)
		return NULL;

	tx_entry = suet_ep_get_tx_entry(ep, op);
	if (!tx_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "could not get tx entry\n");
		return NULL;
	}

	tx_entry->op = op;
	tx_entry->flags = suet_flags;
	tx_entry->bytes_copied = 0;
	tx_entry->offset = 0;
	tx_entry->next_rel_psn = 0;
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
				struct suet_mr *smr = desc[i];
				tx_entry->zc_desc[i] = fi_mr_desc(smr->dg_mr);
			}
		} else if (tx_entry->cq_entry.len >
			   domain->zc_mr_reg_threshold) {
			/* Large unregistered send: do on-demand internal
			 * registration. Failure falls back to memcpy path. */
			int mr_ret = suet_mr_regv_internal(
				domain, iov, iov_count, tx_entry->cq_entry.len,
				FI_SEND, tx_entry->zc_internal_mrs);
			if (mr_ret) {
				FI_WARN(&suet_prov, FI_LOG_EP_DATA,
					"on-demand MR reg failed (%d); using "
					"copy path\n",
					mr_ret);
				memset(tx_entry->zc_internal_mrs, 0,
				       sizeof(tx_entry->zc_internal_mrs));
			} else {
				for (i = 0; i < iov_count; i++) {
					tx_entry->zc_desc[i] = fi_mr_desc(
						tx_entry->zc_internal_mrs[i]);
				}
			}
		}
	}

	/* stamp constant PDS fields */
	memset(&tx_entry->cached_hdr.data.pds, 0,
	       sizeof(tx_entry->cached_hdr.data.pds));
	pds_prologue_set_type(&tx_entry->cached_hdr.data.pds, PDS_ROD_REQ);
	pds_prologue_set_next_hdr(&tx_entry->cached_hdr.data.pds,
				  UET_HDR_REQUEST_STD);

	/* stamp constant SES fields */
	ses_req_ctrl_init(&tx_entry->cached_hdr.data.ses,
			  ofi_op_to_ses_req_opcode(op), 0, 0, 0, 0);
	ses_set_buffer_offset(&tx_entry->cached_hdr.data.ses, 0);
	ses_set_match_bits(&tx_entry->cached_hdr.data.ses, tag);

	dlist_insert_tail(&tx_entry->entry, &ipdc->tx_list);
	tx_entry->ipdc = ipdc;

	return tx_entry;
}

static int suet_ses_match_rx_entry(struct dlist_entry *item, const void *arg)
{
	struct suet_match_attr *attr = (struct suet_match_attr *) arg;
	struct suet_x_entry *rx_entry;

	rx_entry = container_of(item, struct suet_x_entry, entry);

	return suet_match_addr(rx_entry->peer_idx, attr->peer_idx);
}

static int suet_ses_match_tagged_rx_entry(struct dlist_entry *item,
					  const void *arg)
{
	struct suet_match_attr *attr = (struct suet_match_attr *) arg;
	struct suet_x_entry *rx_entry;

	rx_entry = container_of(item, struct suet_x_entry, entry);

	return suet_match_addr(rx_entry->peer_idx, attr->peer_idx) &&
	       suet_match_tag(rx_entry->cq_entry.tag, rx_entry->ignore,
			      attr->tag);
}

void suet_ses_rx_entry_free(struct suet_x_entry *rx_entry)
{
	dlist_remove(&rx_entry->entry);
	ofi_ibuf_free(rx_entry);
}

void suet_ses_rx_entry_complete(struct suet_x_entry *rx_entry)
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

struct suet_x_entry *
suet_ses_rx_entry_init(struct suet_ep *ep, const struct iovec *iov,
		       size_t iov_count, uint64_t tag, uint64_t ignore,
		       void *context, int peer_idx, uint32_t op, uint32_t flags)
{
	struct suet_x_entry *rx_entry;

	rx_entry = suet_ep_get_rx_entry(ep, op);
	if (!rx_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "could not get rx entry\n");
		return NULL;
	}

	rx_entry->peer_idx = peer_idx;
	rx_entry->flags = flags;
	rx_entry->bytes_copied = 0;
	rx_entry->offset = 0;
	rx_entry->next_rel_psn = 0;
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
	struct suet_match_attr *attr = (struct suet_match_attr *) arg;
	struct suet_unexp_msg *unexp_msg =
		container_of(item, struct suet_unexp_msg, entry);
	struct ses_msg_data_pkt *pkt =
		(struct ses_msg_data_pkt *) suet_ses_unexp_msg_get_som_pkt(
			unexp_msg)
			->pkt;

	if (!suet_match_addr(attr->peer_idx, unexp_msg->peer_idx))
		return 0;

	if (!ses_req_opcode_is_tagged(ses_req_ctrl_get_opcode(&pkt->ses)))
		return 1;

	return suet_match_tag(attr->tag, attr->ignore,
			      ses_get_match_bits(&pkt->ses));
}

struct suet_unexp_msg *suet_ses_check_unexp_list(struct dlist_entry *list,
						 int peer_idx, uint64_t tag,
						 uint64_t ignore)
{
	struct suet_match_attr attr;
	struct dlist_entry *match;

	attr.peer_idx = peer_idx;
	attr.tag = tag;
	attr.ignore = ignore;

	match = dlist_find_first_match(list, &suet_ses_match_unexp_msg, &attr);
	if (!match)
		return NULL;

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL, "Matched to unexp msg entry\n");

	return container_of(match, struct suet_unexp_msg, entry);
}

static void
suet_ses_req_som_unpack_generic_op_metadata(struct suet_tpdc *tpdc,
					    struct suet_x_entry *rx_entry,
					    struct ses_msg_data_pkt *pkt)
{
	struct pds_req_hdr *pds = &pkt->pds;
	struct ses_req_hdr *ses_hdr = &pkt->ses;
	uint8_t ses_opcode = ses_req_ctrl_get_opcode(ses_hdr);

	tpdc->curr_rx_id = rx_entry->rx_id;

	if (ses_req_ctrl_has_hd(ses_hdr)) {
		rx_entry->cq_entry.flags |= FI_REMOTE_CQ_DATA;
		rx_entry->cq_entry.data = ses_hd_get_completion_data(ses_hdr);
	}

	rx_entry->peer_idx = tpdc->peer_idx;

	if (ses_req_opcode_is_tagged(ses_opcode))
		rx_entry->cq_entry.tag = ses_get_match_bits(ses_hdr);

	rx_entry->start_psn = pds_req_get_psn(pds);

	if (ses_get_request_length(ses_hdr) <=
	    (uint32_t) suet_ep_domain(rx_entry->ep)->max_seg_sz) {
		rx_entry->num_pkts = 1;
		return;
	}

	rx_entry->tx_id = ses_get_message_id(ses_hdr);
	rx_entry->num_pkts =
		ofi_div_ceil(ses_get_request_length(ses_hdr),
			     suet_ep_domain(rx_entry->ep)->max_seg_sz);
}

void suet_ses_complete_unexp_msg(struct suet_ep *ep,
				 struct suet_x_entry *rx_entry,
				 struct suet_unexp_msg *unexp_msg)
{
	struct ses_msg_data_pkt *pkt =
		(struct ses_msg_data_pkt *) suet_ses_unexp_msg_get_som_pkt(
			unexp_msg)
			->pkt;
	struct suet_pkt_entry *pkt_entry;
	struct suet_domain *suet_domain = suet_ep_domain(ep);
	struct suet_tpdc *tpdc;
	uint16_t curr_id;
	uint64_t request_length = ses_get_request_length(&pkt->ses);
	int is_single_pkt =
		request_length <= (uint64_t) suet_domain->max_seg_sz;
	uint64_t num_segs = 0;
	struct suet_ses_to_pds_resp *resp;

	tpdc = suet_pds_tpdc_get_by_local_pdcid(suet_domain,
						pds_req_get_dpdcid(&pkt->pds));
	curr_id = tpdc->curr_rx_id;

	suet_ses_req_som_unpack_generic_op_metadata(tpdc, rx_entry, pkt);

	while (!dlist_empty(&unexp_msg->pkt_list)) {
		struct ses_msg_data_pkt *seg_pkt;
		uint64_t offset;

		dlist_pop_front(&unexp_msg->pkt_list, struct suet_pkt_entry,
				pkt_entry, d_entry);
		seg_pkt = (struct ses_msg_data_pkt *) pkt_entry->pkt;
		offset = ses_req_ctrl_is_som(&seg_pkt->ses) ?
				 0 :
				 ses_hd_get_message_offset(&seg_pkt->ses);
		suet_pds_copy_payload(ep, rx_entry, pkt_entry, offset);
		rx_entry->next_rel_psn++;
		suet_domain_pkt_entry_free(pkt_entry);
		num_segs++;
	}

	if (rx_entry->next_rel_psn >= rx_entry->num_pkts)
		suet_ses_rx_entry_complete(rx_entry);

	if (tpdc->curr_unexp) {
		if (is_single_pkt ||
		    num_segs == ofi_div_ceil(request_length,
					     suet_domain->max_seg_sz))
			tpdc->curr_rx_id = curr_id;
		else
			tpdc->curr_unexp = NULL;
	}

	/* The reserved slot was linked into tpdc->gtd_del_list at SOM
	 * intake; finalize it in place (sender sees the deferred GD
	 * UET_RESPONSE/UET_OVERFLOW; slot stays linked, freed later by
	 * CLEAR_PSN advance). */
	resp = unexp_msg->gtd_del_resp;
	unexp_msg->gtd_del_resp = NULL;
	suet_ses_unexp_msg_free(unexp_msg);

	assert(resp->ses_opcode == UET_NO_RESPONSE);
	resp->ses_opcode = UET_RESPONSE;
	resp->ses_rc = RC_OK;
	resp->list = UET_OVERFLOW;
	resp->message_id = ses_get_message_id(&pkt->ses);
	resp->modified_length = (uint32_t) request_length;
	suet_pds_tpdc_send_ack(suet_domain, tpdc, resp->psn, UET_HDR_RESPONSE,
			       resp->message_id, resp->modified_length,
			       resp->ses_opcode, resp->ses_rc, resp->list);
}

int suet_ses_peek_recv(struct suet_ep *suet_ep, int peer_idx, uint64_t tag,
		       uint64_t ignore, void *context, uint64_t flags,
		       struct dlist_entry *unexp_list)
{
	struct suet_unexp_msg *unexp_msg;
	struct ses_msg_data_pkt *pkt;

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

	pkt = (struct ses_msg_data_pkt *) suet_ses_unexp_msg_get_som_pkt(
		      unexp_msg)
		      ->pkt;
	return ofi_cq_write(suet_ep->util_ep.rx_cq, context,
			    FI_TAGGED | FI_RECV,
			    ses_get_request_length(&pkt->ses), NULL,
			    ses_req_ctrl_has_hd(&pkt->ses) ?
				    ses_hd_get_completion_data(&pkt->ses) :
				    0,
			    ses_get_match_bits(&pkt->ses));
}

static struct suet_x_entry *
suet_ses_progress_multi_recv(struct suet_ep *ep, struct suet_x_entry *rx_entry,
			     size_t total_size)
{
	struct suet_x_entry *dup_entry;
	size_t left;
	uint32_t dup_id;

	left = rx_entry->iov[0].iov_len - total_size;

	if (left < ep->min_multi_recv_size) {
		rx_entry->cq_entry.flags |= FI_MULTI_RECV;
		return NULL;
	}

	dup_entry = suet_ep_get_rx_entry(ep, rx_entry->op);
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
				 struct suet_x_entry *rx_entry)
{
	struct suet_x_entry *progress_entry, *dup_entry = NULL;
	struct suet_unexp_msg *unexp_msg;
	struct ses_msg_data_pkt *pkt;
	size_t total_size;

	while (!dlist_empty(unexp_list)) {
		unexp_msg = suet_ses_check_unexp_list(
			unexp_list, rx_entry->peer_idx, rx_entry->cq_entry.tag,
			rx_entry->ignore);
		if (!unexp_msg)
			return 0;

		pkt = (struct ses_msg_data_pkt *)
			      suet_ses_unexp_msg_get_som_pkt(unexp_msg)
				      ->pkt;
		total_size = ses_get_request_length(&pkt->ses);

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
			  struct suet_unexp_msg *unexp_msg)
{
	struct ses_msg_data_pkt *pkt =
		(struct ses_msg_data_pkt *) suet_ses_unexp_msg_get_som_pkt(
			unexp_msg)
			->pkt;
	struct suet_domain *suet_domain = suet_ep_domain(suet_ep);
	struct suet_tpdc *tpdc;
	uint16_t gtd_msg_id = ses_get_message_id(&pkt->ses);
	uint32_t gtd_req_len = (uint32_t) ses_get_request_length(&pkt->ses);
	struct suet_ses_to_pds_resp *placeholder;
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
	 * is a per-tpdc "cancelled message_id" set checked in the RX path that
	 * drops post-discard segments without buffering. */

	ret = ofi_cq_write(suet_ep->util_ep.rx_cq, context, FI_TAGGED | FI_RECV,
			   0, NULL,
			   ses_req_ctrl_has_hd(&pkt->ses) ?
				   ses_hd_get_completion_data(&pkt->ses) :
				   0,
			   ses_get_match_bits(&pkt->ses));

	tpdc = suet_pds_tpdc_get_by_local_pdcid(suet_domain,
						pds_req_get_dpdcid(&pkt->pds));
	/* The peer's PDC may have been torn down between PEEK and DISCARD
	 * (Close Cmd). In that case the placeholder was already freed via
	 * gtd_del_list cleanup; just complete the user's CQ entry and bail. */
	if (!tpdc) {
		suet_ses_unexp_msg_cleanup(unexp_msg);
		return ret;
	}

	/* If FI_DISCARD races against a multi-segment receive (SOM matched,
	 * EOM not yet arrived), tpdc->curr_unexp still points at this
	 * unexp_msg. Clear it before freeing to avoid a use-after-free when
	 * the next continuation arrives. */
	if (tpdc->curr_unexp == unexp_msg)
		tpdc->curr_unexp = NULL;

	/* Detach the placeholder before freeing unexp_msg bookkeeping; the
	 * placeholder itself stays linked on tpdc->gtd_del_list (finalize
	 * mutates it in place; CLEAR_PSN advance will free it). */
	placeholder = unexp_msg->gtd_del_resp;
	unexp_msg->gtd_del_resp = NULL;
	suet_ses_unexp_msg_cleanup(unexp_msg);

	assert(placeholder->ses_opcode == UET_NO_RESPONSE);
	placeholder->ses_opcode = UET_RESPONSE;
	placeholder->ses_rc = RC_OK;
	placeholder->list = UET_OVERFLOW;
	placeholder->message_id = gtd_msg_id;
	placeholder->modified_length = gtd_req_len;
	suet_pds_tpdc_send_ack(suet_domain, tpdc, placeholder->psn,
			       UET_HDR_RESPONSE, placeholder->message_id,
			       placeholder->modified_length,
			       placeholder->ses_opcode, placeholder->ses_rc,
			       placeholder->list);

	return ret;
}

/*
 * Allocate an unexpected-message bookkeeping struct and link a
 * reserved response slot into tpdc->gtd_del_list, marked
 * UET_NO_RESPONSE and covering the full PSN range
 * [SOM_psn, SOM_psn + num_pkts) of the message.
 *
 * Reserving at intake is the spec-clean way to admit a message we cannot
 * yet semantically answer (Spec 3.4.3.6.2.2 / 3.4.4.5): the receiver MUST
 * eventually emit a GD UET_RESPONSE/UET_OVERFLOW for it, and that response
 * MUST NOT fail to persist. If the pool is exhausted at intake, we refuse
 * to admit -- caller sees nack_code=NO_RESOURCE on resp and the dispatcher
 * NACKs the request without advancing expected_rx_psn so the initiator
 * RTO-retransmits.
 */
static struct suet_unexp_msg *
suet_ses_init_unexp_msg(struct suet_ep *ep, struct suet_tpdc *tpdc,
			struct ses_msg_data_pkt *som_pkt,
			struct suet_ses_to_pds_resp *resp)
{
	struct suet_domain *domain = suet_ep_domain(ep);
	struct suet_unexp_msg *unexp_msg;
	struct suet_ses_to_pds_resp *reserved_resp;
	uint32_t request_length = ses_get_request_length(&som_pkt->ses);
	uint32_t som_psn = pds_req_get_psn(&som_pkt->pds);
	uint32_t num_pkts = ofi_div_ceil(request_length, domain->max_seg_sz);
	struct suet_ses_to_pds_resp placeholder = {
		.ses_opcode = UET_NO_RESPONSE,
		.ses_rc = RC_OK,
		.list = UET_EXPECTED,
		.message_id = ses_get_message_id(&som_pkt->ses),
		.modified_length = request_length,
		.psn = som_psn,
		/* 0-byte messages still produce one SOM+EOM packet. */
		.num_pkts = (uint16_t) (num_pkts == 0 ? 1 : num_pkts),
	};

	reserved_resp =
		suet_pds_tpdc_save_gtd_del_resp(domain, tpdc, &placeholder);
	if (!reserved_resp) {
		/* Spec 3.5.12.7: no GD response slot available. */
		resp->pds_nack_code = PDS_NACK_CODE_NO_GTD_DEL_AVAIL;
		return NULL;
	}

	unexp_msg = ofi_buf_alloc(domain->unexp_msg_pool);
	if (!unexp_msg) {
		dlist_remove(&reserved_resp->entry);
		ofi_buf_free(reserved_resp);
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"SES SOM: no rx entry match and unexpected "
			"alloc failed\n");
		resp->ses_rc = RC_NO_MATCH;
		resp->ses_opcode = UET_RESPONSE;
		return NULL;
	}

	dlist_init(&unexp_msg->pkt_list);
	unexp_msg->gtd_del_resp = reserved_resp;

	return unexp_msg;
}

static struct suet_x_entry *
suet_ses_match_msg_rx_entry(struct suet_ep *ep, struct suet_tpdc *tpdc,
			    struct suet_pkt_entry *pkt_entry,
			    struct ses_msg_data_pkt *pkt,
			    struct suet_ses_to_pds_resp *resp)
{
	struct suet_x_entry *rx_entry, *dup_entry;
	struct dlist_entry *rx_list;
	struct dlist_entry *unexp_list;
	struct dlist_entry *match;
	struct suet_match_attr attr;
	size_t total_size;

	attr.peer_idx = tpdc->peer_idx;

	if (ses_req_opcode_is_tagged(ses_req_ctrl_get_opcode(&pkt->ses))) {
		unexp_list = &ep->unexp_tag_list;
		attr.tag = ses_get_match_bits(&pkt->ses);
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
		assert(!tpdc->curr_unexp);
		tpdc->curr_unexp = suet_ses_init_unexp_msg(ep, tpdc, pkt, resp);
		if (tpdc->curr_unexp) {
			tpdc->curr_unexp->peer_idx = tpdc->peer_idx;
			dlist_insert_tail(&pkt_entry->d_entry,
					  &tpdc->curr_unexp->pkt_list);
			dlist_insert_tail(&tpdc->curr_unexp->entry, unexp_list);
		}
		return NULL;
	}

	rx_entry = container_of(match, struct suet_x_entry, entry);
	total_size = ses_get_request_length(&pkt->ses);

	if (rx_entry->flags & SUET_MULTI_RECV) {
		dup_entry =
			suet_ses_progress_multi_recv(ep, rx_entry, total_size);
		if (!dup_entry)
			goto out;

		dup_entry->start_psn = pds_req_get_psn(&pkt->pds);
		dlist_init(&dup_entry->entry);
		rx_entry = dup_entry;
		goto init;
	}

out:
	dlist_remove(&rx_entry->entry);
init:
	rx_entry->cq_entry.len = MIN(rx_entry->cq_entry.len, total_size);
	dlist_insert_tail(&rx_entry->entry, &tpdc->rx_list);
	return rx_entry;
}

static struct suet_x_entry *
suet_ses_match_rma_rx_entry(struct suet_ep *ep, struct ses_msg_data_pkt *pkt,
			    uint32_t ofi_op)
{
	struct suet_x_entry *rx_entry;
	struct suet_domain *suet_domain = suet_ep_domain(ep);
	struct pds_req_hdr *pds = &pkt->pds;
	struct ses_req_hdr *ses_hdr = &pkt->ses;
	struct suet_tpdc *tpdc;
	struct iovec iov[1];
	int ret;

	tpdc = suet_pds_tpdc_get_by_local_pdcid(suet_domain,
						pds_req_get_dpdcid(pds));
	if (!tpdc)
		return NULL;

	ret = suet_ses_verify_mr_iov(ep, ses_hdr, ofi_op, iov);
	if (ret)
		return NULL;

	rx_entry = suet_ses_rx_entry_init(ep, iov, 1, 0, 0, NULL,
					  tpdc->peer_idx, ofi_op, 0);
	if (!rx_entry)
		return NULL;

	rx_entry->start_psn = pds_req_get_psn(pds);

	dlist_insert_tail(&rx_entry->entry, &tpdc->rx_list);
	return rx_entry;
}

void suet_ses_execute_atomic_op(struct suet_ep *ep,
				       struct suet_x_entry *rx_entry,
				       struct ses_msg_amo_pkt *pkt)
{
	struct ses_msg_amo_hdr *atom_hdr = &pkt->amo;
	char *src = pkt->msg;
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

struct suet_x_entry *
suet_ses_req_som_unpack_to_rx_entry(struct suet_ep *ep, struct suet_tpdc *tpdc,
				    struct suet_pkt_entry *pkt_entry,
				    struct ses_msg_data_pkt *pkt,
				    struct suet_ses_to_pds_resp *resp)
{
	uint8_t ses_opcode = ses_req_ctrl_get_opcode(&pkt->ses);
	struct suet_x_entry *rx_entry;

	if (ses_req_opcode_is_send(ses_opcode) ||
	    ses_req_opcode_is_tagged(ses_opcode)) {
		rx_entry = suet_ses_match_msg_rx_entry(ep, tpdc, pkt_entry, pkt,
						       resp);
	} else if (ses_req_opcode_is_write(ses_opcode)) {
		rx_entry = suet_ses_match_rma_rx_entry(ep, pkt, ofi_op_write);
	} else if (ses_opcode == UET_ATOMIC) {
		rx_entry = suet_ses_match_rma_rx_entry(ep, pkt, ofi_op_atomic);
	} else {
		FI_WARN(&suet_prov, FI_LOG_CQ, "Unsupported SES opcode: %d\n",
			ses_opcode);
		return NULL;
	}

	if (rx_entry)
		suet_ses_req_som_unpack_generic_op_metadata(tpdc, rx_entry,
							    pkt);

	return rx_entry;
}

struct suet_ep *suet_ses_lookup_ep_by_ri(struct suet_domain *domain,
					 struct ses_req_hdr *ses_hdr)
{
	uint16_t ri = ses_get_ri(ses_hdr);
	if (OFI_UNLIKELY(ri >= suet_env.max_eps || !domain->ep_table[ri])) {
		FI_WARN(&suet_prov, FI_LOG_CQ, "No EP for RI=%d\n", ri);
		return NULL;
	}
	return domain->ep_table[ri];
}

void suet_ses_init_ses_hdr(struct ses_req_hdr *ses_hdr,
			   struct suet_ep *ep,
			   struct suet_x_entry *tx_entry, int is_som,
				  int is_eom)
{
	ses_req_init(ses_hdr, ses_req_ctrl_get_opcode(ses_hdr), is_som, is_eom,
		     1, tx_entry->flags & SUET_REMOTE_CQ_DATA, tx_entry->tx_id,
		     ses_get_buffer_offset(ses_hdr),
		     ses_get_match_bits(ses_hdr), tx_entry->cq_entry.data,
		     (uint32_t) tx_entry->cq_entry.len, ep->pid_on_fep,
		     ep->resource_index);
}


void suet_ses_init_rma_iov(const struct fi_rma_iov *rma_iov,
			   struct suet_x_entry *tx_entry)
{
	ses_set_buffer_offset(&tx_entry->cached_hdr.data.ses, rma_iov[0].addr);
	ses_set_match_bits(&tx_entry->cached_hdr.data.ses, rma_iov[0].key);
}