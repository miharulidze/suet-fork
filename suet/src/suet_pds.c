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
 * PDS (Packet Delivery Sublayer) reliability code: ipdc/tpdc lifecycle,
 * ACK/NACK/CTRL handlers, retransmit, OOO buffering, PDC teardown.
 */

#include "suet.h"

void suet_pds_copy_payload(struct suet_ep *ep, struct suet_x_entry *rx_entry,
			   struct suet_pkt_entry *pkt_entry,
			   uint64_t copy_offset)
{
	struct ses_msg_data_pkt *pkt =
		(struct ses_msg_data_pkt *) pkt_entry->pkt;
	size_t payload_size = pkt_entry->pkt_size - sizeof(*pkt) -
			      suet_ep_domain(ep)->rx_prefix_size;

	if (payload_size > 0) {
		uint64_t done =
			ofi_copy_to_iov(rx_entry->iov, rx_entry->iov_count,
					copy_offset, pkt->msg, payload_size);
		rx_entry->bytes_copied += done;
	}
}

void suet_pds_ipdc_progress_tx_list(struct suet_ipdc *ipdc)
{
	struct dlist_entry *tmp_entry;
	struct suet_x_entry *tx_entry;
	struct suet_pkt_entry *head_pkt_entry;
	uint32_t head_psn;

	/* head_psn = PSN of the lowest unacked (in-flight) packet. When the
	 * in-flight list is empty, that is conceptually one past the highest
	 * cumulatively-acked PSN (last_rx_cack_psn per Spec Table 3-35). */
	if (!dlist_empty(&ipdc->in_flight_pkts)) {
		head_pkt_entry = container_of(ipdc->in_flight_pkts.next,
					      struct suet_pkt_entry, d_entry);
		head_psn = pds_req_get_psn(
			&((struct ses_msg_data_pkt *) head_pkt_entry->pkt)
				 ->pds);
	} else {
		head_psn = ipdc->last_rx_cack_psn + 1;
	}

	dlist_foreach_container_safe (&ipdc->tx_list, struct suet_x_entry,
				      tx_entry, entry, tmp_entry) {
		/* check that all packets for this message are posted */
		if (tx_entry->next_rel_psn >= tx_entry->num_pkts) {
			/* if yes, check if we got all ACKs for this message */
			if (pds_psn_before(tx_entry->start_psn +
						   (tx_entry->num_pkts - 1),
					   head_psn)) {
				suet_ses_tx_entry_complete(tx_entry);
			}
			continue;
		}

		suet_pds_send_tx_entry(tx_entry);
		/* even if suet_pds_send_tx_entry internally failed due to
		 exhausted window or lack of free packets, no completion
		 ordering (comp_order == 0) allows us to try to progress
		 and complete subsequent messages */
	}

	/* Spec 3.5.11.4.4: CLEAR_PSN is the high-water-mark of PSNs whose
	 * SES responses have been delivered locally. Advance it to the
	 * cumulatively-acked horizon. */
	if (pds_psn_before(ipdc->last_tx_clear_psn, ipdc->last_rx_cack_psn))
		ipdc->last_tx_clear_psn = ipdc->last_rx_cack_psn;
}

static void
suet_pds_tpdc_send_ack_if_needed(struct suet_domain *domain,
				 struct suet_tpdc *tpdc,
				 struct ses_msg_data_pkt *pkt,
				 const struct suet_ses_to_pds_resp *ses_resp)
{
	uint32_t ack_interval = (uint32_t) suet_env.max_unacked / 2;
	if (ack_interval == 0)
		ack_interval = 1;
	if (ses_resp->ses_rc != RC_OK || ses_resp->som_or_eom ||
	    (pds_prologue_get_flags(&pkt->pds) & PDS_FLAG_AR) ||
	    tpdc->pkts_since_last_ack >= ack_interval) {
		suet_pds_tpdc_send_ack(
			domain, tpdc, pds_req_get_psn(&pkt->pds),
			UET_HDR_RESPONSE, ses_get_message_id(&pkt->ses),
			ses_get_request_length(&pkt->ses), ses_resp->ses_opcode,
			ses_resp->ses_rc, ses_resp->list);
	}
}

/*
 * Advance the receiver's CLEAR_PSN to new_clear and free any saved
 * guaranteed-delivery responses now covered by it.
 *
 * Spec 3.5.11.4.4: CLEAR_PSN MUST be monotonically increasing. Once it
 * moves past a saved gtd_del response's PSN, the destination may release
 * that response (Spec 3.5.16.3). All wire-format decoding and validation
 * (e.g. recovering CLEAR_PSN from a Request's pds.psn + sign-extended
 * pds.clear_psn_offset, or extracting it from a Clear Cmd CP payload) is
 * the caller's responsibility.
 */
static void suet_pds_tpdc_advance_rx_clear_psn(struct suet_tpdc *tpdc,
					       uint32_t new_clear)
{
	struct suet_ses_to_pds_resp *gtd_resp;
	struct dlist_entry *item, *next;

	if (!pds_psn_before(tpdc->last_rx_clear_psn, new_clear))
		return;
	tpdc->last_rx_clear_psn = new_clear;

	/* Free every saved slot whose PSN is covered by new_clear, EXCEPT
	 * placeholders for unexpected messages that have not yet been
	 * matched (still UET_NO_RESPONSE). The sender's CLEAR_PSN advance
	 * is driven by its cumulative ACK, which counts UET_NO_RESPONSE
	 * ACKs; without this guard the placeholder gets freed under our
	 * feet and unexp_msg->gtd_del_resp dangles. The placeholder is
	 * mutated to UET_RESPONSE/UET_OVERFLOW when the recv matches; a
	 * later CLEAR_PSN advance will free it then. */
	dlist_foreach_safe (&tpdc->gtd_del_list, item, next) {
		gtd_resp =
			container_of(item, struct suet_ses_to_pds_resp, entry);
		if (pds_psn_before(new_clear, gtd_resp->psn))
			break;
		if (gtd_resp->ses_opcode == UET_NO_RESPONSE)
			continue;
		dlist_remove(&gtd_resp->entry);
		ofi_buf_free(gtd_resp);
	}
}

/*
 * Spec 3.5.11.4.4 / 3.5.12.5.5: persist a SES response so the destination
 * can replay it on retransmit until CLEAR_PSN moves past its PSN. Caller MUST
 * set resp->num_pkts >= 1 (range covered by this slot in dup-PSN replay).
 *
 * Returns the saved slot, or NULL on pool exhaustion. Caller decides how to
 * surface the failure (typically: set pds_nack_code so the unified emit path
 * sends a NACK and the dispatcher leaves expected_rx_psn unchanged for RTO
 * retry).
 */
struct suet_ses_to_pds_resp *
suet_pds_tpdc_save_gtd_del_resp(struct suet_domain *domain,
				struct suet_tpdc *tpdc,
				const struct suet_ses_to_pds_resp *resp)
{
	struct suet_ses_to_pds_resp *saved;

	assert(resp->num_pkts >= 1);

	saved = ofi_buf_alloc(domain->gtd_del_resp_pool);
	if (!saved) {
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"gtd_del_resp_pool exhausted: cannot save psn=%u\n",
			resp->psn);
		return NULL;
	}
	*saved = *resp;
	saved->x_entry_owned = false;
	saved->gtd_del = true;
	saved->tpdc = tpdc;
	dlist_init(&saved->entry);
	dlist_insert_tail(&saved->entry, &tpdc->gtd_del_list);
	return saved;
}

static void suet_pds_send_nack(struct suet_domain *domain, fi_addr_t dg_av_addr,
			       uint8_t nack_code, uint32_t nack_psn,
			       uint16_t spdcid, uint16_t dpdcid)
{
	struct suet_pkt_entry *pkt_entry;
	struct pds_nack_hdr *nack;

	pkt_entry = suet_domain_get_tx_pkt(domain);
	if (!pkt_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "Unable to send nack\n");
		return;
	}

	nack = (struct pds_nack_hdr *) pkt_entry->pkt;
	pkt_entry->pkt_size = sizeof(*nack) + domain->tx_prefix_size;
	pkt_entry->dg_av_addr = dg_av_addr;

	pds_nack_init(nack, nack_code, nack_psn, spdcid, dpdcid);

	if (suet_domain_dg_ep_send_pkt(domain, pkt_entry)) {
		suet_domain_pkt_entry_free(pkt_entry);
	} else {
		domain->counters.nacks_tx++;
	}
}

/*
 * Unified PDS response: send a PDS NACK if the dispatch encoded a failure into
 * resp->nack_code, otherwise apply normal ACK coalescing logic.
 */
static void suet_pds_tpdc_send_response(struct suet_domain *domain,
					struct suet_tpdc *tpdc,
					struct ses_msg_data_pkt *pkt,
					const struct suet_ses_to_pds_resp *resp)
{
	if (resp->pds_nack_code)
		suet_pds_send_nack(domain, tpdc->dg_av_addr,
				   resp->pds_nack_code,
				   pds_req_get_psn(&pkt->pds),
				   tpdc->local_pdcid, tpdc->ipdcid);
	else
		suet_pds_tpdc_send_ack_if_needed(domain, tpdc, pkt, resp);
}

static int
suet_pds_dispatch_req_to_ses_in_order(struct suet_ep *ep,
				      struct suet_tpdc *tpdc,
				      struct suet_pkt_entry *pkt_entry)
{
	struct ses_msg_data_pkt *pkt =
		(struct ses_msg_data_pkt *) pkt_entry->pkt;
	struct suet_domain *domain = container_of(
		ep->util_ep.domain, struct suet_domain, util_domain);
	struct suet_ses_to_pds_resp resp = {
		.x_entry_owned = false,
		.ses_opcode = UET_DEFAULT_RESPONSE,
		.ses_rc = RC_OK,
		.list = UET_EXPECTED,
		.psn = pds_req_get_psn(&pkt->pds),
		.message_id = ses_get_message_id(&pkt->ses),
		.modified_length = ses_get_request_length(&pkt->ses),
	};
	struct suet_x_entry *x_entry = NULL;
	uint32_t new_clear_psn = pds_req_get_clear_psn(&pkt->pds);
	bool is_som = ses_req_ctrl_is_som(&pkt->ses);
	bool is_eom = ses_req_ctrl_is_eom(&pkt->ses);
	resp.som_or_eom = is_som || is_eom;

	suet_pds_tpdc_advance_rx_clear_psn(tpdc, new_clear_psn);

	if (is_som) {
		uint8_t ses_opcode = ses_req_ctrl_get_opcode(&pkt->ses);

		resp.ses_rc = RC_OK;
		x_entry = suet_ses_req_som_unpack_to_rx_entry(
			ep, tpdc, pkt_entry, pkt, &resp);
		if (!x_entry) {
			if (!resp.pds_nack_code && tpdc->curr_unexp) {
				resp.ses_opcode = UET_NO_RESPONSE;
				resp.x_entry_owned = true;
			}
		} else if (ses_opcode == UET_ATOMIC) {
			suet_ses_execute_atomic_op(
				ep, x_entry, (struct ses_msg_amo_pkt *) pkt);
			x_entry->next_rel_psn++;
		} else {
			suet_pds_copy_payload(ep, x_entry, pkt_entry, 0);
			x_entry->next_rel_psn++;
		}
	} else if (tpdc->curr_unexp) {
		dlist_insert_tail(&pkt_entry->d_entry,
				  &tpdc->curr_unexp->pkt_list);
		resp.ses_opcode = UET_NO_RESPONSE;
		resp.x_entry_owned = true;
		resp.ses_rc = RC_OK;
	} else {
		x_entry = ofi_bufpool_get_ibuf(domain->rx_entry_pool.pool,
					       tpdc->curr_rx_id);
		suet_pds_copy_payload(ep, x_entry, pkt_entry,
				      ses_hd_get_message_offset(&pkt->ses));
		x_entry->next_rel_psn++;
		resp.ses_rc = RC_OK;
	}

	/* Bail before any further SES processing if the dispatch encoded a
	 * PDS-level failure (e.g. gtd_del_resp_pool exhausted in
	 * suet_ses_init_unexp_msg). x_entry is NULL and curr_unexp is NULL on
	 * this path, so process_rx_eom would crash. expected_rx_psn stays
	 * put so the initiator RTO-retransmits. */
	if (resp.pds_nack_code)
		goto send_resp;

	if (suet_ses_resp_is_gtd_del(&resp)) {
		/* In-order expected path: each saved slot covers exactly
		 * the single PSN of the request being responded to. */
		resp.num_pkts = 1;
		if (!suet_pds_tpdc_save_gtd_del_resp(domain, tpdc, &resp)) {
			/* Spec 3.5.12.7: GD slot pool exhausted. */
			resp.pds_nack_code = PDS_NACK_CODE_NO_GTD_DEL_AVAIL;
			goto send_resp;
		}
	}

	if (resp.ses_rc != RC_OK)
		goto send_resp;

	if (is_eom && tpdc->curr_unexp)
		tpdc->curr_unexp = NULL;

	if (x_entry && x_entry->next_rel_psn >= x_entry->num_pkts)
		suet_ses_rx_entry_complete(x_entry);

	tpdc->expected_rx_psn++;
	tpdc->pkts_since_last_ack++;

send_resp:
	suet_pds_tpdc_send_response(domain, tpdc, pkt, &resp);
	if (!resp.x_entry_owned)
		suet_domain_pkt_entry_free(pkt_entry);
	return resp.pds_nack_code ? -FI_ENOMEM : 0;
}

static void suet_pds_progress_ooo_rx_pkts(struct suet_domain *domain,
					  struct suet_tpdc *tpdc)
{
	struct suet_pkt_entry *pkt_entry;
	struct pds_req_hdr *pds;
	struct ses_msg_data_pkt *ses_pkt;
	struct suet_ep *ep;

	while (!dlist_empty(&tpdc->ooo_pkts)) {
		pkt_entry = container_of(tpdc->ooo_pkts.next,
					 struct suet_pkt_entry, d_entry);
		pds = &((struct ses_msg_data_pkt *) pkt_entry->pkt)->pds;
		if (pds_req_get_psn(pds) != tpdc->expected_rx_psn)
			return;

		/* Remove from ooo_pkts before SES processing so d_entry
		 * can be reused for bookkeeping pkt in unexpected path. */
		suet_domain_pkt_entry_unlink(pkt_entry);

		ses_pkt = (struct ses_msg_data_pkt *) pkt_entry->pkt;
		ep = suet_ses_lookup_ep_by_ri(domain, &ses_pkt->ses);
		if (!ep) {
			suet_domain_pkt_entry_free(pkt_entry);
			continue;
		}

		if (suet_pds_dispatch_req_to_ses_in_order(ep, tpdc,
							  pkt_entry)) {
			/* Save failed; expected_rx_psn was NOT advanced. Stop
			 * draining -- subsequent OOO pkts would just stack up
			 * behind this unmade resp_slot. */
			return;
		}
	}
}

static int suet_pds_compare_pkt_psn_fn(struct dlist_entry *item,
				       const void *arg)
{
	struct suet_pkt_entry *list_entry, *new_entry;
	struct pds_req_hdr *list_hdr, *new_hdr;

	list_entry = container_of(item, struct suet_pkt_entry, d_entry);
	list_hdr = &((struct ses_msg_data_pkt *) list_entry->pkt)->pds;

	new_entry = container_of((struct dlist_entry *) arg,
				 struct suet_pkt_entry, d_entry);
	new_hdr = &((struct ses_msg_data_pkt *) new_entry->pkt)->pds;

	/* Wrap-safe ordering (Spec 3.5.12 serial-number arithmetic). The
	 * in-flight window is bounded by max_unacked, so the unsigned form
	 * is correct in practice; pds_psn_before keeps it correct under any
	 * future bound. */
	return pds_psn_before(pds_req_get_psn(list_hdr),
			      pds_req_get_psn(new_hdr));
}

static void suet_pds_dispatch_req_to_ses_ooo(struct suet_domain *domain,
					     struct suet_tpdc *tpdc,
					     struct suet_pkt_entry *pkt_entry,
					     struct pds_req_hdr *pds_hdr,
					     struct ses_req_hdr *ses_hdr)
{
	if (!suet_env.retry) {
		/* This works only with *lossless* out-of-order transport! */
		dlist_insert_order(&tpdc->ooo_pkts,
				   &suet_pds_compare_pkt_psn_fn,
				   &pkt_entry->d_entry);
		return;
	}

	/* Duplicate of an already-delivered packet: our previous
	 * cumulative ACK must have been lost (or the sender RTO'd
	 * before it arrived). If we have a saved guaranteed-delivery
	 * response for this PSN (Spec 3.5.11.4.4), replay it -- the
	 * initiator may not yet have observed the original GD ACK.
	 * Otherwise resend the current cumulative ACK with DEFAULT
	 * (presumption rule) so the sender can advance its window.
	 *
	 * Spec 3.5.12.5: a duplicate that does NOT carry pds.flags.retx
	 * is just network reordering -- silently drop it without replying
	 * (event-counted), because the sender is not asking for a
	 * response. Only RETX-marked duplicates indicate the sender
	 * actually re-issued the request and is waiting for the answer. */
	if (pds_psn_before(pds_req_get_psn(pds_hdr), tpdc->expected_rx_psn)) {
		struct suet_ses_to_pds_resp *gtd_del_resp;
		uint32_t dup_psn = pds_req_get_psn(pds_hdr);
		bool replayed = false;

		if (!(pds_prologue_get_flags(pds_hdr) & PDS_FLAG_RETX)) {
			domain->counters.dup_drops++;
			goto free_pkt;
		}

		dlist_foreach_container (&tpdc->gtd_del_list,
					 struct suet_ses_to_pds_resp,
					 gtd_del_resp, entry) {
			if (!pds_psn_before(dup_psn, gtd_del_resp->psn) &&
			    pds_psn_before(dup_psn,
					   gtd_del_resp->psn +
						   gtd_del_resp->num_pkts)) {
				suet_pds_tpdc_send_ack(
					domain, tpdc, dup_psn, UET_HDR_RESPONSE,
					gtd_del_resp->message_id,
					gtd_del_resp->modified_length,
					gtd_del_resp->ses_opcode,
					gtd_del_resp->ses_rc,
					gtd_del_resp->list);
				replayed = true;
				break;
			}
		}
		if (!replayed) {
			suet_pds_tpdc_send_ack(
				domain, tpdc, dup_psn, UET_HDR_RESPONSE,
				ses_get_message_id(ses_hdr),
				ses_get_request_length(ses_hdr),
				UET_DEFAULT_RESPONSE, RC_OK, UET_EXPECTED);
		}
		goto free_pkt;
	}

	/* TODO: abstract away into reliability algorithm callback: GBN,
	 * SR, FEC, etc. */

	/* Go-back-N: send NACK to tell the sender the PSN we actually
	 * need.  Per spec (Section 3.5.8.2, step 4.d.i) the receiver
	 * issues a NACK with pds.nack_code = PDS_NACK_CODE_ROD_OOO
	 * carrying the out-of-order PSN. */
	suet_pds_send_nack(domain, tpdc->dg_av_addr, PDS_NACK_CODE_ROD_OOO,
			   pds_req_get_psn(pds_hdr), tpdc->local_pdcid,
			   tpdc->ipdcid);
free_pkt:
	suet_domain_pkt_entry_free(pkt_entry);
}

static void suet_pds_process_rx_req(struct suet_domain *domain,
				    struct suet_tpdc *tpdc,
				    struct suet_pkt_entry *pkt_entry)
{
	struct ses_msg_data_pkt *pkt =
		(struct ses_msg_data_pkt *) pkt_entry->pkt;
	struct pds_req_hdr *pds_hdr = &pkt->pds;
	struct ses_req_hdr *ses_hdr = &pkt->ses;
	struct suet_ep *ep;
	if (pkt_entry->pkt_size <
	    sizeof(struct ses_msg_data_pkt) + domain->rx_prefix_size) {
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"Cannot process packet smaller than minimum header "
			"size\n");
		suet_domain_pkt_entry_free(pkt_entry);
		return;
	}

	ep = suet_ses_lookup_ep_by_ri(domain, ses_hdr);
	if (!ep) {
		suet_domain_pkt_entry_free(pkt_entry);
		return;
	}

	/* Out-of-order */
	if (pds_req_get_psn(pds_hdr) != tpdc->expected_rx_psn) {
		suet_pds_dispatch_req_to_ses_ooo(domain, tpdc, pkt_entry,
						 pds_hdr, ses_hdr);
		return;
	}

	/* In-order; if successful, current packet might fill the gap */
	if (!suet_pds_dispatch_req_to_ses_in_order(ep, tpdc, pkt_entry) &&
	    !suet_env.retry && !dlist_empty(&tpdc->ooo_pkts)) {
		suet_pds_progress_ooo_rx_pkts(domain, tpdc);
	}
}

/*
 * tx_entries currently being progressed on this ipdc are linked into
 * ipdc->tx_list (see suet_pds_ipdc_progress_tx_list); they are removed
 * only on terminal completion. Match by tx_id (== SES message_id we put on
 * the wire in suet_ses_init_ses_hdr).
 */
static struct suet_x_entry *
suet_pds_ipdc_find_tx_entry_by_msg_id(struct suet_ipdc *ipdc, uint16_t msg_id)
{
	struct suet_x_entry *tx_entry;

	dlist_foreach_container (&ipdc->tx_list, struct suet_x_entry, tx_entry,
				 entry) {
		if (tx_entry->tx_id == msg_id)
			return tx_entry;
	}
	return NULL;
}

/*
 * Spec 3.4.3 / Table 3-19: a non-RC_OK SES response indicates the target
 * could not semantically complete the operation. The response is guaranteed-
 * delivered, so the initiator MUST surface a fatal CQ error completion to
 * the user instead of relying on transport retry/timeout. Drops the matching
 * tx_entry from ipdc->tx_list and frees it.
 */
static void suet_pds_handle_ses_error_resp(struct suet_domain *domain,
					   struct suet_ipdc *ipdc,
					   const struct ses_resp_hdr *resp_hdr)
{
	uint8_t rc = ses_resp_ctrl_get_rc(resp_hdr);
	uint16_t msg_id = ses_resp_get_message_id(resp_hdr);
	struct suet_x_entry *tx_entry;
	struct fi_cq_err_entry err_entry;
	struct util_cq *util_cq;

	tx_entry = suet_pds_ipdc_find_tx_entry_by_msg_id(ipdc, msg_id);
	if (!tx_entry) {
		/* tx_entry already completed (e.g. spurious / duplicate
		 * GD-replayed error after we moved on). Nothing to do. */
		return;
	}

	memset(&err_entry, 0, sizeof(err_entry));
	err_entry.op_context = tx_entry->cq_entry.op_context;
	err_entry.flags = tx_entry->cq_entry.flags;
	err_entry.err = FI_EINVAL;
	err_entry.prov_errno = rc;

	util_cq = &suet_ep_tx_cq(tx_entry->ep)->util_cq;
	if (ofi_cq_write_error(util_cq, &err_entry)) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"could not write SES error completion\n");
	}

	domain->counters.ses_err_completions++;
	domain->counters.tx_cq_errors++;
	suet_ses_tx_entry_free(tx_entry);
}

static void suet_pds_free_ipdc(struct suet_domain *domain,
			       struct suet_ipdc *ipdc)
{
	struct suet_pkt_entry *pkt_entry;

	suet_ses_drain_x_entry_list(&ipdc->tx_list, true, FI_ECONNREFUSED);

	while (!dlist_empty(&ipdc->in_flight_pkts)) {
		dlist_pop_front(&ipdc->in_flight_pkts, struct suet_pkt_entry,
				pkt_entry, d_entry);
		if (pkt_entry->flags & SUET_PKT_IN_USE) {
			/* NIC still owns the buffer; we cannot free it yet.
			 * Mark it ACKED and self-loop its d_entry so the TX
			 * completion handler takes the ACKED branch (where
			 * the ipdc lookup will fail) and the dlist_remove in
			 * suet_domain_pkt_entry_unlink_and_free is a safe
			 * no-op. */
			pkt_entry->flags |= SUET_PKT_ACKED;
			dlist_init(&pkt_entry->d_entry);
		} else {
			suet_domain_pkt_entry_free(pkt_entry);
		}
		ipdc->in_flight_cnt--;
	}

	dlist_remove(&ipdc->entry);
	HASH_DELETE(ipdc_dg_av_addr_handle, domain->ipdc_by_dg_av_addr_ht,
		    ipdc);
	ofi_idm_clear(&domain->local_pdcid_to_ipdc_idm,
		      (int) ipdc->local_pdcid);

	ipdc->state = SUET_PDC_CLOSED;
	ofi_buf_free(ipdc);
}

void suet_pds_ipdc_process_ack(struct suet_domain *domain,
			       struct suet_pkt_entry *ack_entry)
{
	struct ses_msg_ack_pkt *ack = (struct ses_msg_ack_pkt *) ack_entry->pkt;
	uint32_t cack_psn = pds_ack_get_cack_psn(&ack->pds);
	bool has_ses_resp = pds_ack_get_next_hdr(&ack->pds) == UET_HDR_RESPONSE;
	struct dlist_entry *next_unacked_d_entry;
	struct suet_pkt_entry *cur_unacked_pkt_entry;
	struct pds_req_hdr *cur_unacked_hdr;
	struct suet_ipdc *ipdc;

	ipdc = suet_pds_ipdc_get_by_local_pdcid(domain,
						pds_ack_get_dpdcid(&ack->pds));
	if (!ipdc) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"ACK for unknown PDCID=%d\n",
			pds_ack_get_dpdcid(&ack->pds));
		goto out;
	}

	/* Learn remote pdcid from first ACK */
	if (ipdc->state == SUET_PDC_OPENING) {
		ipdc->tpdcid = pds_ack_get_spdcid(&ack->pds);
		ipdc->state = SUET_PDC_ESTABLISHED;
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "PDC established for source PDCID=%d\n", ipdc->tpdcid);
	}

	/* Spec 3.5.8.3: Close ACK frees the ipdc and ends the lifecycle.
	 * Identified by being in CLOSE_ACK_WAIT with a cack covering close_psn.
	 * Done before the SES error / cumulative-ACK walk because there is no
	 * SES response on a Close ACK and we must not feed the closing tx_list
	 * back into progress. */
	if (ipdc->state == SUET_PDC_CLOSE_ACK_WAIT &&
	    pds_psn_after_eq(cack_psn, ipdc->close_psn)) {
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "Close ACK received for ipdc pdcid=%u close_psn=%u\n",
		       ipdc->local_pdcid, ipdc->close_psn);
		domain->counters.pdc_closes_ok++;
		suet_pds_free_ipdc(domain, ipdc);
		goto out;
	}

	/* TODO: abstract away into reliability algorithm callback: GBN, SR,
	 * FEC, etc. */

	/* SES error ACK (e.g. RC_NO_MATCH) or stale/duplicate ACK:
	 * don't advance the sender window. Still check for RTO'ed in-flight
	 * packets so we retransmit promptly instead of waiting for the next
	 * domain progress cycle.
	 *
	 * Spec 3.4.3 / Table 3-19: a non-RC_OK SES response is a fatal
	 * semantic failure. Surface it as a CQ error completion on the
	 * matching tx_entry instead of letting RTO loop until the retry
	 * budget is exhausted. */
	if (has_ses_resp && ses_resp_ctrl_get_rc(&ack->ses) != RC_OK) {
		domain->counters.error_acks_rx++;
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"Received ACK with SES error: opcode=%d rc=%d "
			"cack_psn=%u dpdcid=%d\n",
			ses_resp_ctrl_get_opcode(&ack->ses),
			ses_resp_ctrl_get_rc(&ack->ses), cack_psn,
			pds_ack_get_dpdcid(&ack->pds));
		suet_pds_handle_ses_error_resp(domain, ipdc, &ack->ses);
		goto retry_check;
	}
	if (pds_psn_after_eq(ipdc->last_rx_cack_psn, cack_psn)) {
		domain->counters.stale_acks_rx++;
		goto retry_check;
	}

	assert(!dlist_empty(&(ipdc->in_flight_pkts)));
	assert(ipdc->in_flight_cnt);

	ipdc->last_rx_cack_psn = cack_psn;
	ipdc->retry_cnt = 0;

	cur_unacked_pkt_entry = container_of(ipdc->in_flight_pkts.next,
					     struct suet_pkt_entry, d_entry);

	while (&cur_unacked_pkt_entry->d_entry != &ipdc->in_flight_pkts) {
		cur_unacked_hdr = &((struct ses_msg_data_pkt *)
					    cur_unacked_pkt_entry->pkt)
					   ->pds;
		/* Spec Table 3-35: cack_psn covers all PSNs up to and
		 * including itself. Stop when packet PSN > cack_psn. */
		if (pds_psn_before(cack_psn, pds_req_get_psn(cur_unacked_hdr)))
			break;

		next_unacked_d_entry = cur_unacked_pkt_entry->d_entry.next;

		if (cur_unacked_pkt_entry->flags & SUET_PKT_IN_USE) {
			cur_unacked_pkt_entry->flags |= SUET_PKT_ACKED;
		} else {
			ipdc->in_flight_cnt--;
			suet_domain_pkt_entry_unlink_and_free(
				cur_unacked_pkt_entry);
		}

		cur_unacked_pkt_entry = container_of(
			next_unacked_d_entry, struct suet_pkt_entry, d_entry);
	}

	suet_pds_ipdc_progress_tx_list(ipdc);

	/* Spec 3.5.16.3.1: if the target asked for a clear via REQ_CLEAR and
	 * we have nothing else to send (no queued tx_entries, no unacked PSNs),
	 * emit a Clear Cmd CP carrying the just-advanced last_tx_clear_psn so
	 * the target can free saved gtd_del responses. If tx_list / in_flight
	 * are non-empty, the next outbound PDS Request piggybacks CLEAR_PSN
	 * for free (Spec 3.5.10.4) and Clear Cmd MUST NOT be emitted.
	 * Probe / Close ACK paths set req=0 by construction. */
	if (pds_ack_get_req(&ack->pds) == PDS_ACK_REQ_CLEAR &&
	    dlist_empty(&ipdc->tx_list) && ipdc->in_flight_cnt == 0 &&
	    ipdc->state == SUET_PDC_ESTABLISHED)
		suet_pds_ipdc_send_clear_cmd(domain, ipdc);
	goto out;

retry_check:
	suet_pds_ipdc_progress_tx_pkt_list(domain, ipdc);
	(void) suet_pds_free_ipdc_if_retry_exhausted(domain, ipdc);
out:
	suet_domain_pkt_entry_free(ack_entry);
}

void suet_pds_ipdc_process_nack(struct suet_domain *domain,
				struct suet_pkt_entry *nack_entry)
{
	struct pds_nack_hdr *nack = (struct pds_nack_hdr *) nack_entry->pkt;
	struct suet_ipdc *ipdc;

	ipdc = suet_pds_ipdc_get_by_local_pdcid(domain,
						pds_nack_get_dpdcid(nack));
	if (!ipdc) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"NACK for unknown PDCID=%d\n",
			pds_nack_get_dpdcid(nack));
		goto out;
	}

	domain->counters.nacks_rx++;

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Received NACK: code=%d nack_psn=%u dpdcid=%d\n",
	       pds_nack_get_code(nack), pds_nack_get_psn(nack),
	       pds_nack_get_dpdcid(nack));
out:
	suet_domain_pkt_entry_free(nack_entry);
}

void suet_pds_process_tx_cqe(struct suet_domain *domain,
			     struct fi_cq_msg_entry *comp)
{
	struct suet_pkt_entry *pkt_entry =
		container_of(comp->op_context, struct suet_pkt_entry, context);
	struct suet_ipdc *ipdc;
	struct pds_req_hdr *pds =
		&((struct ses_msg_data_pkt *) pkt_entry->pkt)->pds;
	uint16_t lpdcid;

	FI_DBG(&suet_prov, FI_LOG_EP_DATA, "got send completion (type: %s)\n",
	       pds_pkt_type_name(pds_prologue_get_type(pds)));

	switch (pds_prologue_get_type(pds)) {
	case PDS_ACK:
	case PDS_NACK:
		suet_domain_pkt_entry_free(pkt_entry);
		break;
	case PDS_CP:
		/* Spec 3.5.16.3.1: Clear Cmd CP is never linked into
		 * in_flight_pkts and the receiver does not ACK it. Free
		 * directly on TX completion. Close Cmd CP IS linked and IS
		 * ACKed; it falls through to PDS_ROD_REQ. */
		if (pds_ctrl_get_ctl_type((struct pds_ctrl_hdr *) pds) ==
		    PDS_CTL_CLEAR_CMD) {
			suet_domain_pkt_entry_free(pkt_entry);
			break;
		}
		/* fallthrough */
	case PDS_ROD_REQ:
		if (pkt_entry->flags & SUET_PKT_ACKED) {
			lpdcid = pkt_entry->local_pdcid;
			ipdc = suet_pds_ipdc_get_by_local_pdcid(domain, lpdcid);
			suet_domain_pkt_entry_unlink_and_free(pkt_entry);
			if (ipdc) {
				ipdc->in_flight_cnt--;
				/* try to send more if window allows and we have
				 * outstanding data */
				suet_pds_ipdc_progress_tx_list(ipdc);
			} else {
				/* ipdc was freed (e.g. Close ACK).
				 * Buffer is already gone. */
			}
		} else {
			pkt_entry->flags &= ~SUET_PKT_IN_USE;
		}
		break;
	default:
		FI_WARN(&suet_prov, FI_LOG_EP_DATA,
			"Unknown send completion pkt type: %d\n",
			pds_prologue_get_type(pds));
		break;
	}
}

static void suet_pds_free_tpdc(struct suet_domain *domain,
			       struct suet_tpdc *tpdc)
{
	struct suet_pkt_entry *pkt_entry;
	struct dlist_entry *tmp;

	if (!suet_env.retry) {
		dlist_foreach_container_safe (&tpdc->ooo_pkts,
					      struct suet_pkt_entry, pkt_entry,
					      d_entry, tmp) {
			suet_domain_pkt_entry_unlink_and_free(pkt_entry);
		}
	}

	/* In-progress receives matched onto this tpdc. Complete each with
	 * FI_ECANCELED and release the rx_entry; otherwise their list
	 * linkage would dangle into the freed tpdc. */
	suet_ses_drain_x_entry_list(&tpdc->rx_list, false, FI_ECANCELED);
	suet_ses_drain_x_entry_list(&tpdc->rma_rx_list, false, FI_ECANCELED);

	/* Drain any saved guaranteed-delivery responses; the peer is gone
	 * so retained state is no longer needed. */
	while (!dlist_empty(&tpdc->gtd_del_list)) {
		struct suet_ses_to_pds_resp *resp;
		dlist_pop_front(&tpdc->gtd_del_list,
				struct suet_ses_to_pds_resp, resp, entry);
		ofi_buf_free(resp);
	}

	dlist_remove(&tpdc->entry);
	HASH_DELETE(tpdc_syn_key_handle, domain->tpdc_by_syn_key_ht, tpdc);
	ofi_idm_clear(&domain->local_pdcid_to_tpdc_idm,
		      (int) tpdc->local_pdcid);

	tpdc->state = SUET_PDC_CLOSED;
	ofi_buf_free(tpdc);
}

static void suet_pds_process_cp(struct suet_domain *domain,
				fi_addr_t dg_av_addr,
				struct suet_pkt_entry *pkt_entry)
{
	struct ses_msg_ctrl_pkt *cp =
		(struct ses_msg_ctrl_pkt *) pkt_entry->pkt;
	struct pds_ctrl_hdr *cphdr = &cp->pds;
	struct suet_tpdc *tpdc;
	uint32_t cp_psn = pds_ctrl_get_psn(cphdr);
	uint16_t dpdcid = pds_ctrl_get_dpdcid(cphdr);
	uint8_t ctl_type = pds_ctrl_get_ctl_type(cphdr);

	tpdc = suet_pds_tpdc_get_by_local_pdcid(domain, dpdcid);
	if (!tpdc) {
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"CP for unknown DPDCID=%u (ctl_type=%u), sending "
			"INV_DPDCID NACK\n",
			dpdcid, ctl_type);
		suet_pds_send_nack(domain, dg_av_addr, PDS_NACK_CODE_INV_DPDCID,
				   cp_psn, 0, pds_ctrl_get_spdcid(cphdr));
		goto free_pkt;
	}

	switch (ctl_type) {
	case PDS_CTL_CLEAR_CMD:
		/* Spec 3.5.16.3.1: pds.payload carries cumulative CLEAR_PSN.
		 * No ACK is generated (pds.flags.ar = 0). */
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "Clear Cmd CP rx: tpdcid=%u clear_psn=%u\n",
		       tpdc->local_pdcid, ntohl(cphdr->payload));
		suet_pds_tpdc_advance_rx_clear_psn(tpdc, ntohl(cphdr->payload));
		break;
	case PDS_CTL_CLOSE_CMD:
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "Close Cmd CP rx: tpdcid=%u psn=%u\n", tpdc->local_pdcid,
		       cp_psn);

		/* Spec 3.5.8.3: Close Cmd implicitly clears all forward PSNs.
		 * Diagnostic: non-empty rx state on close => CLOSE_IN_ERR. */
		if (!dlist_empty(&tpdc->rx_list) ||
		    !dlist_empty(&tpdc->rma_rx_list) ||
		    !dlist_empty(&tpdc->ooo_pkts)) {
			FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
				"Close Cmd on non-idle tpdc pdcid=%u\n",
				tpdc->local_pdcid);
			domain->counters.pdc_close_in_err++;
		}

		if (pds_psn_after_eq(cp_psn, tpdc->expected_rx_psn))
			tpdc->expected_rx_psn = cp_psn + 1;

		suet_pds_tpdc_send_ack(domain, tpdc, cp_psn, UET_HDR_NONE, 0, 0,
				       0, 0, 0);

		domain->counters.pdc_closes_rx++;
		suet_pds_free_tpdc(domain, tpdc);
		break;
	default:
		FI_WARN(&suet_prov, FI_LOG_CQ,
			"Unhandled ctl_type=%u on tpdc pdcid=%u, dropping\n",
			ctl_type, tpdc->local_pdcid);
		break;
	}

free_pkt:
	suet_domain_pkt_entry_free(pkt_entry);
}

static struct suet_tpdc *suet_pds_allocate_tpdc(struct suet_domain *domain,
						fi_addr_t dg_av_addr,
						uint16_t ipdcid,
						uint32_t ipdc_start_psn)
{
	struct suet_tpdc *tpdc;

	tpdc = ofi_buf_alloc(domain->tpdc_pool);
	if (!tpdc)
		return NULL;

	memset(tpdc, 0, sizeof(*tpdc));
	tpdc->local_pdcid = suet_domain_allocate_pdcid(domain);
	tpdc->ipdcid = ipdcid;
	tpdc->dg_av_addr = dg_av_addr;
	tpdc->peer_idx =
		suet_domain_dg_av_get_peer_idx_by_addr(domain, dg_av_addr);
	tpdc->expected_rx_psn = ipdc_start_psn;
	/* Mirrors wire cack_psn (= expected_rx_psn - 1); start one before
	 * ipdc_start_psn so the first sent ACK appears as a strict advance. */
	tpdc->last_tx_cack_psn = ipdc_start_psn - 1;
	/* Spec 3.5.11.4.4: CLEAR_PSN starts at Start_PSN - 1. */
	tpdc->last_rx_clear_psn = ipdc_start_psn - 1;
	tpdc->pkts_since_last_ack = 0;
	tpdc->state = SUET_PDC_ESTABLISHED;
	tpdc->curr_rx_id = 0;
	tpdc->curr_unexp = NULL;

	dlist_init(&tpdc->rx_list);
	dlist_init(&tpdc->rma_rx_list);
	dlist_init(&tpdc->ooo_pkts);
	dlist_init(&tpdc->gtd_del_list);

	if (ofi_idm_set(&domain->local_pdcid_to_tpdc_idm,
			(int) tpdc->local_pdcid, tpdc) < 0)
		goto err;

	HASH_ADD(tpdc_syn_key_handle, domain->tpdc_by_syn_key_ht, dg_av_addr,
		 sizeof(struct suet_ut_tpdc_syn_key), tpdc);

	dlist_insert_tail(&tpdc->entry, &domain->active_tpdc_list);

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Created tpdc: local_pdcid=%u ipdcid=%u "
	       "dg_av_addr=%ld start_psn=%u\n",
	       tpdc->local_pdcid, ipdcid, (long) dg_av_addr, ipdc_start_psn);

	return tpdc;

err:
	ofi_buf_free(tpdc);
	return NULL;
}

static struct suet_tpdc *
suet_pds_resolve_tpdc_on_syn(struct suet_domain *domain,
			     fi_addr_t peer_dg_av_addr, uint16_t ipdcid,
			     uint32_t ipdc_start_psn)
{
	struct suet_tpdc *tpdc =
		suet_pds_tpdc_get_by_syn_key(domain, peer_dg_av_addr, ipdcid);
	return tpdc ? tpdc :
		      suet_pds_allocate_tpdc(domain, peer_dg_av_addr, ipdcid,
					     ipdc_start_psn);
}

static void suet_pds_tpdc_process_rod_pkt(struct suet_domain *domain,
					  fi_addr_t dg_av_addr,
					  struct suet_pkt_entry *pkt_entry)
{
	struct ses_msg_data_pkt *pkt =
		(struct ses_msg_data_pkt *) pkt_entry->pkt;
	struct pds_req_hdr *pds = &pkt->pds;
	struct suet_tpdc *tpdc;
	uint32_t ipdc_start_psn;
	uint8_t nack_code;

	/* pds.flags.syn=1: initiator is establishing a new PDC */
	if (pds_prologue_get_flags(pds) & PDS_FLAG_SYN) {
		FI_DBG(&suet_prov, FI_LOG_EP_DATA,
		       "SYN: syn_psn_offset=%u use_rsv_pdc=%u\n",
		       pds_req_get_syn_psn_offset(pds),
		       pds_req_get_syn_use_rsv_pdc(pds));

		ipdc_start_psn =
			pds_req_get_psn(pds) - pds_req_get_syn_psn_offset(pds);

		tpdc = suet_pds_resolve_tpdc_on_syn(domain, dg_av_addr,
						    pds_req_get_spdcid(pds),
						    ipdc_start_psn);
		if (!tpdc) {
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"Failed to resolve TPDC on SYN, sending "
				"NO_RESOURCE NACK\n");
			nack_code = PDS_NACK_CODE_NO_RESOURCE;
			goto nack_pkt;
		}

		/* SYN processing done. Rewrite the overloaded
		 * dpdcid with the newly allocated pdcid so
		 * downstream handlers see it as an established PDC */
		pds_req_set_dpdcid(pds, tpdc->local_pdcid);
	} else {
		tpdc = suet_pds_tpdc_get_by_local_pdcid(
			domain, pds_req_get_dpdcid(pds));
		if (!tpdc) {
			FI_WARN(&suet_prov, FI_LOG_CQ,
				"No TPDC for dpdcid=%d, sending INV_DPDCID "
				"NACK\n",
				pds_req_get_dpdcid(pds));
			nack_code = PDS_NACK_CODE_INV_DPDCID;
			goto nack_pkt;
		}
	}

	suet_pds_process_rx_req(domain, tpdc, pkt_entry);
	return;

nack_pkt:
	suet_pds_send_nack(domain, dg_av_addr, nack_code, pds_req_get_psn(pds),
			   0, pds_req_get_spdcid(pds));
	suet_domain_pkt_entry_free(pkt_entry);
}

void suet_pds_process_rx_cqe(struct suet_domain *domain,
			     struct fi_cq_msg_entry *comp, fi_addr_t dg_av_addr)
{
	struct suet_pkt_entry *pkt_entry =
		container_of(comp->op_context, struct suet_pkt_entry, context);
	struct pds_req_hdr *pds =
		&((struct ses_msg_data_pkt *) pkt_entry->pkt)->pds;
	uint8_t nh;

	FI_DBG(&suet_prov, FI_LOG_EP_DATA, "got recv completion (type: %s)\n",
	       pds_pkt_type_name(pds_prologue_get_type(pds)));

	suet_domain_pkt_entry_unlink(pkt_entry);

	pkt_entry->pkt_size = comp->len;

	switch (pds_prologue_get_type(pds)) {
	case PDS_ACK:
		nh = pds_ack_get_next_hdr((struct pds_ack_hdr *) pds);
		if (nh != UET_HDR_RESPONSE && nh != UET_HDR_NONE)
			goto bad_next_hdr;
		suet_pds_ipdc_process_ack(domain, pkt_entry);
		break;
	case PDS_NACK:
		suet_pds_ipdc_process_nack(domain, pkt_entry);
		break;
	case PDS_ROD_REQ:
		if (pds_prologue_get_next_hdr(pds) != UET_HDR_REQUEST_STD)
			goto bad_next_hdr;
		suet_pds_tpdc_process_rod_pkt(domain, dg_av_addr, pkt_entry);
		break;
	case PDS_CP:
		suet_pds_process_cp(domain, dg_av_addr, pkt_entry);
		break;
	default:
		FI_WARN(&suet_prov, FI_LOG_CQ, "Unknown packet type: %d\n",
			pds_prologue_get_type(pds));
		goto free_pkt;
	}

	return;

bad_next_hdr:
	FI_WARN(&suet_prov, FI_LOG_CQ,
		"Ignoring packet with unsupported pds.next_hdr=%d for "
		"pds.type=%d\n",
		pds_prologue_get_next_hdr(pds), pds_prologue_get_type(pds));
free_pkt:
	suet_domain_pkt_entry_free(pkt_entry);
}

/*
 * Exponential back-off starting at 1ms, max 4s.
 */
static int suet_pds_get_timeout(int retry_cnt)
{
	return MIN(1 << retry_cnt, 4000);
}

static uint64_t suet_pds_get_retry_time(uint64_t start, int retry_cnt)
{
	return start + suet_pds_get_timeout(retry_cnt);
}

struct suet_pkt_entry *suet_pds_ipdc_generate_new_req_pkt(
	struct suet_domain *domain, struct suet_x_entry *tx_entry,
	struct suet_ipdc *ipdc, size_t hdr_len, uint32_t psn, uint16_t flags)
{
	struct suet_pkt_entry *pkt_entry;
	struct pds_req_hdr *pds;

	pkt_entry = suet_domain_get_tx_pkt(domain);
	if (!pkt_entry)
		return NULL;

	memcpy(pkt_entry->pkt, &tx_entry->cached_hdr, hdr_len);
	pds = (struct pds_req_hdr *) pkt_entry->pkt;
	pds_req_set_psn(pds, psn);
	pds_req_set_spdcid(pds, ipdc->local_pdcid);
	pds_prologue_set_flags(pds, flags);

	/* Spec 3.5.11.4.4: pds.clear_psn_offset is signed two's-complement,
	 * never positive, with CLEAR_PSN = pds.psn + clear_psn_offset.
	 * pds.psn was set above; pds_req_set_clear_psn computes the wire
	 * offset from the absolute target. */
	pds_req_set_clear_psn(pds, ipdc->last_tx_clear_psn);

	if (ipdc->state == SUET_PDC_OPENING) {
		pds_prologue_set_flags(pds, pds_prologue_get_flags(pds) |
						    PDS_FLAG_SYN);
		pds_req_set_syn_psn(pds, ipdc->start_psn, 0);
	} else {
		pds_req_set_dpdcid(pds, ipdc->tpdcid);
	}

	pkt_entry->dg_av_addr = ipdc->dg_av_addr;
	pkt_entry->local_pdcid = ipdc->local_pdcid;

	return pkt_entry;
}

static void suet_pds_ipdc_insert_unacked_pkt(struct suet_ipdc *ipdc,
					     struct suet_pkt_entry *pkt_entry)
{
	dlist_insert_tail(&pkt_entry->d_entry, &ipdc->in_flight_pkts);
	ipdc->in_flight_cnt++;
}

void suet_pds_ipdc_send_tracked_pkt(struct suet_domain *domain,
				    struct suet_ipdc *ipdc,
				    struct suet_pkt_entry *pkt_entry)
{
	/* On send failure: PSN already consumed; enqueue so subsequent progress
	 * call can retransmits. */
	(void) suet_domain_dg_ep_send_pkt(domain, pkt_entry);
	suet_pds_ipdc_insert_unacked_pkt(ipdc, pkt_entry);
}

void suet_pds_send_tx_entry(struct suet_x_entry *tx_entry)
{
	struct suet_ipdc *ipdc = tx_entry->ipdc;
	struct suet_pkt_entry *pkt_entry;
	struct ses_req_hdr *ses_hdr;
	struct suet_ep *ep = tx_entry->ep;
	struct suet_domain *domain = suet_ep_domain(ep);
	bool is_som;
	bool initial_post;
	uint32_t seg_size;
	void *payload_ptr;

	if (ipdc->in_flight_cnt >= (uint16_t) suet_env.max_unacked)
		return;

	initial_post = !(tx_entry->flags & SUET_TX_ENTRY_PDS_OWNED);
	if (initial_post) {
		tx_entry->flags |= SUET_TX_ENTRY_PDS_OWNED;
		suet_ses_init_ses_hdr(&tx_entry->cached_hdr.data.ses, ep,
				      tx_entry, 1, 1);
		tx_entry->start_psn = ipdc->tx_seq_no;
		ipdc->tx_seq_no += tx_entry->num_pkts;
	}

	while (tx_entry->next_rel_psn < tx_entry->num_pkts &&
	       ipdc->in_flight_cnt < (uint16_t) suet_env.max_unacked) {
		is_som = (tx_entry->next_rel_psn == 0);

		pkt_entry = suet_pds_ipdc_generate_new_req_pkt(
			domain, tx_entry, ipdc, tx_entry->hdr_len,
			tx_entry->start_psn + tx_entry->next_rel_psn, 0);
		if (!pkt_entry)
			return;

		ses_hdr = &((struct ses_msg_data_pkt *) pkt_entry->pkt)->ses;

		seg_size = (uint32_t) MIN(domain->max_seg_sz,
					  tx_entry->cq_entry.len -
						  tx_entry->bytes_copied);

		ses_req_ctrl_set_som_eom(ses_hdr, is_som,
					 tx_entry->bytes_copied + seg_size >=
						 tx_entry->cq_entry.len);

		if (!is_som) {
			ses_hd_set_cont(ses_hdr,
					(uint32_t) tx_entry->bytes_copied,
					(uint16_t) seg_size);
		}

		if (tx_entry->zc_desc[0] != NULL) {
			pkt_entry->zc_pld_iov_count = suet_tx_iov_find_slice(
				tx_entry, tx_entry->bytes_copied, seg_size,
				&pkt_entry->zc_pld_iov[1],
				&pkt_entry->zc_pld_desc[1]);
		} else {
			payload_ptr =
				(char *) pkt_entry->pkt + tx_entry->hdr_len;
			ofi_copy_from_iov(payload_ptr, seg_size, tx_entry->iov,
					  tx_entry->iov_count,
					  tx_entry->bytes_copied);
		}

		pkt_entry->pkt_size = tx_entry->hdr_len + seg_size +
				      suet_ep_domain(ep)->tx_prefix_size;

		suet_pds_ipdc_send_tracked_pkt(suet_ep_domain(ep), ipdc,
					       pkt_entry);

		tx_entry->bytes_copied += seg_size;
		tx_entry->next_rel_psn++;
	}
}

/*
 * Build and send a PDS ACK.
 *
 * next_hdr selects the ACK shape:
 *   - UET_HDR_RESPONSE: data ACK; carries an SES response trailer
 *     (resp_opcode/resp_rc/message_id/modified_length).
 *   - UET_HDR_NONE: CP ACK (e.g. Close ACK); per spec 3.5.10.8 / Table 3-43
 *     it MUST carry no SES response, so the resp_* / message_id args are
 *     ignored.
 */
void suet_pds_tpdc_send_ack(struct suet_domain *domain, struct suet_tpdc *tpdc,
			    uint32_t trigger_psn, uint8_t next_hdr,
			    uint16_t message_id, uint32_t modified_length,
			    uint8_t resp_opcode, uint8_t resp_rc,
			    uint8_t resp_list)
{
	struct suet_pkt_entry *pkt_entry;
	struct ses_msg_ack_pkt *ack;
	uint32_t cack_psn;

	pkt_entry = suet_domain_get_tx_pkt(domain);
	if (!pkt_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "Unable to send ack\n");
		return;
	}

	ack = (struct ses_msg_ack_pkt *) pkt_entry->pkt;
	pkt_entry->pkt_size =
		(next_hdr == UET_HDR_RESPONSE ? sizeof(*ack) :
						sizeof(ack->pds)) +
		domain->tx_prefix_size;
	pkt_entry->dg_av_addr = tpdc->dg_av_addr;

	/* TX buffers are recycled from a pool; zero the prologue. */
	ack->pds.prologue.raw = 0;

	/* Spec Table 3-35: cack_psn = highest in-order PSN received
	 * ("all PDS Requests with PSN prior and including this PSN are
	 * acknowledged"). expected_rx_psn is next-expected, hence -1. */
	cack_psn = tpdc->expected_rx_psn - 1;

	pds_ack_set_type(&ack->pds, PDS_ACK);
	pds_ack_set_next_hdr(&ack->pds, next_hdr);
	pds_ack_set_spdcid(&ack->pds, tpdc->local_pdcid);
	pds_ack_set_dpdcid(&ack->pds, tpdc->ipdcid);
	pds_ack_set_cack_psn(&ack->pds, cack_psn);
	/* Spec Table 3-35: signed offset from CACK_PSN to ACK_PSN, where
	 * ACK_PSN is the PSN of the packet that triggered this ACK. */
	pds_ack_set_ack_psn(&ack->pds, trigger_psn);

	if (next_hdr == UET_HDR_RESPONSE) {
		ses_resp_init(&ack->ses, resp_list, resp_opcode, resp_rc,
			      message_id, modified_length);
		if (!dlist_empty(&tpdc->gtd_del_list))
			pds_ack_set_req(&ack->pds, PDS_ACK_REQ_CLEAR);
	}

	if (suet_domain_dg_ep_send_pkt(domain, pkt_entry))
		suet_domain_pkt_entry_free(pkt_entry);

	tpdc->last_tx_cack_psn = cack_psn;
	tpdc->pkts_since_last_ack = 0;
}

static struct suet_ipdc *suet_pds_allocate_ipdc(struct suet_domain *domain,
						fi_addr_t dg_av_addr)
{
	struct suet_ipdc *ipdc;

	ipdc = ofi_buf_alloc(domain->ipdc_pool);
	if (!ipdc)
		return NULL;

	memset(ipdc, 0, sizeof(*ipdc));
	ipdc->local_pdcid = suet_domain_allocate_pdcid(domain);
	ipdc->tpdcid = 0;
	ipdc->dg_av_addr = dg_av_addr;
	ipdc->start_psn = pds_generate_start_psn(&domain->psn_seed, 0);
	ipdc->tx_seq_no = ipdc->start_psn;
	/* Spec Table 3-35: cack_psn = highest acked PSN. Before any ACK has
	 * been received nothing in [start_psn, ...] is acked, so initialize
	 * to start_psn - 1 (i.e. one before the first PSN we will send). */
	ipdc->last_rx_cack_psn = ipdc->start_psn - 1;
	/* Spec 3.5.11.4.4: CLEAR_PSN initialized to Start_PSN - 1 in both
	 * directions. Eager-clear: advanced together with last_rx_cack_psn. */
	ipdc->last_tx_clear_psn = ipdc->start_psn - 1;
	ipdc->retry_cnt = 0;
	ipdc->in_flight_cnt = 0;
	ipdc->state = SUET_PDC_OPENING;
	ipdc->teardown_pending = false;
	ipdc->close_psn = 0;
	dlist_init(&ipdc->tx_list);
	dlist_init(&ipdc->in_flight_pkts);

	if (ofi_idm_set(&domain->local_pdcid_to_ipdc_idm,
			(int) ipdc->local_pdcid, ipdc) < 0)
		goto err;

	HASH_ADD(ipdc_dg_av_addr_handle, domain->ipdc_by_dg_av_addr_ht,
		 dg_av_addr, sizeof(ipdc->dg_av_addr), ipdc);

	dlist_insert_tail(&ipdc->entry, &domain->active_ipdc_list);

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Created ipdc: pdcid=%u dg_av_addr=%ld\n", ipdc->local_pdcid,
	       (long) dg_av_addr);

	return ipdc;

err:
	ofi_buf_free(ipdc);
	return NULL;
}

struct suet_ipdc *suet_pds_assign_ipdc(struct suet_domain *domain,
				       fi_addr_t dg_av_addr)
{
	struct suet_ipdc *ipdc =
		suet_pds_ipdc_get_by_dg_av_addr(domain, dg_av_addr);

	if (ipdc && ipdc->state != SUET_PDC_OPENING &&
	    ipdc->state != SUET_PDC_ESTABLISHED) {
		/* Spec 3.5.8.3: once a PDC begins closing, it MUST NOT accept
		 * new SES messages.
		 * A new PDC would be needed instead, but we
		 * only support one ipdc per peer today, so refuse the send. */
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "ipdc pdcid=%u state=%d, refusing new tx\n",
		       ipdc->local_pdcid, ipdc->state);
		return NULL;
	}
	return ipdc ? ipdc : suet_pds_allocate_ipdc(domain, dg_av_addr);
}

void suet_pds_ipdc_send_clear_cmd(struct suet_domain *domain,
				  struct suet_ipdc *ipdc)
{
	struct suet_pkt_entry *pkt_entry;
	struct ses_msg_ctrl_pkt *cp;

	pkt_entry = suet_domain_get_tx_pkt(domain);
	if (!pkt_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"Unable to send Clear Cmd CP\n");
		return;
	}

	cp = (struct ses_msg_ctrl_pkt *) pkt_entry->pkt;
	pkt_entry->pkt_size = sizeof(cp->pds) + domain->tx_prefix_size;
	pkt_entry->dg_av_addr = ipdc->dg_av_addr;

	/* Spec 3.5.16.3.1: pds.psn = 0, pds.flags.ar = 0 (fire-and-forget,
	 * no ACK expected); CLEAR_PSN is carried in pds.payload. */
	pds_ctrl_init(&cp->pds, PDS_CTL_CLEAR_CMD, 0, ipdc->local_pdcid,
		      ipdc->tpdcid, ipdc->last_tx_clear_psn);

	if (suet_domain_dg_ep_send_pkt(domain, pkt_entry))
		suet_domain_pkt_entry_free(pkt_entry);

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Sent Clear Cmd CP: ipdcid=%u tpdcid=%u clear_psn=%u\n",
	       ipdc->local_pdcid, ipdc->tpdcid, ipdc->last_tx_clear_psn);
}

static void suet_pds_ipdc_send_close_cmd(struct suet_domain *domain,
					 struct suet_ipdc *ipdc)
{
	struct suet_pkt_entry *pkt_entry;
	struct ses_msg_ctrl_pkt *cp;

	/* On pkt_entry alloc failure we simply return; the next progress tick
	 * will call us again. */
	pkt_entry = suet_domain_get_tx_pkt(domain);
	if (!pkt_entry)
		return;

	cp = (struct ses_msg_ctrl_pkt *) pkt_entry->pkt;
	pkt_entry->pkt_size = sizeof(*cp) + domain->tx_prefix_size;
	pkt_entry->dg_av_addr = ipdc->dg_av_addr;
	pkt_entry->local_pdcid = ipdc->local_pdcid;

	ipdc->close_psn = ipdc->tx_seq_no++;
	/* Spec 3.5.16.4.1: Close Command CP consumes a forward PSN and
	 * carries no payload. */
	pds_ctrl_init(&cp->pds, PDS_CTL_CLOSE_CMD, ipdc->close_psn,
		      ipdc->local_pdcid, ipdc->tpdcid, 0);

	suet_pds_ipdc_insert_unacked_pkt(ipdc, pkt_entry);
	ipdc->state = SUET_PDC_CLOSE_ACK_WAIT;
	ipdc->retry_cnt = 0;

	/* On NIC send failure the pkt sits on in_flight_pkts and
	 * the RTO path retransmits it. */
	if (suet_domain_dg_ep_send_pkt(domain, pkt_entry)) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"failed to send Close Command CP for pdcid=%u; will "
			"retry\n",
			ipdc->local_pdcid);
	}

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Sent Close Command CP: ipdcid=%u tpdcid=%u close_psn=%u\n",
	       ipdc->local_pdcid, ipdc->tpdcid, ipdc->close_psn);
}

void suet_pds_ipdc_begin_teardown(struct suet_ipdc *ipdc)
{
	/* Mark teardown requested. Actual QUIESCE / Close Cmd happens via
	 * progress once OPENING resolves to ESTABLISHED (so we have a valid
	 * tpdcid to put in the Close Cmd CP). */
	ipdc->teardown_pending = true;
}

void suet_pds_ipdc_progress_teardown(struct suet_domain *domain,
				     struct suet_ipdc *ipdc)
{
	if (ipdc->teardown_pending && ipdc->state == SUET_PDC_ESTABLISHED) {
		ipdc->state = SUET_PDC_QUIESCE;
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "ipdc pdcid=%u entering QUIESCE\n", ipdc->local_pdcid);
	}
	if (ipdc->state != SUET_PDC_QUIESCE)
		return;
	if (!dlist_empty(&ipdc->tx_list) || !dlist_empty(&ipdc->in_flight_pkts))
		return;

	suet_pds_ipdc_send_close_cmd(domain, ipdc);
}

void suet_pds_ipdc_progress_tx_pkt_list(struct suet_domain *domain,
					struct suet_ipdc *ipdc)
{
	struct suet_pkt_entry *pkt_entry;
	uint64_t current;
	ssize_t ret;
	bool retry = false;

	current = ofi_gettime_ms();
	dlist_foreach_container (&ipdc->in_flight_pkts, struct suet_pkt_entry,
				 pkt_entry, d_entry) {
		if (pkt_entry->flags & (SUET_PKT_IN_USE | SUET_PKT_ACKED) ||
		    current <
			    suet_pds_get_retry_time(pkt_entry->timestamp,
						    (uint8_t) ipdc->retry_cnt))
			break;
		retry = true;
		pds_prologue_set_flags(
			(struct pds_req_hdr *) pkt_entry->pkt,
			pds_prologue_get_flags(
				(struct pds_req_hdr *) pkt_entry->pkt) |
				PDS_FLAG_RETX);
		ret = suet_domain_dg_ep_send_pkt(domain, pkt_entry);
		if (ret)
			break;
	}

	if (retry)
		ipdc->retry_cnt++;
}

bool suet_pds_free_ipdc_if_retry_exhausted(struct suet_domain *domain,
					   struct suet_ipdc *ipdc)
{
	if (ipdc->retry_cnt <= suet_env.max_pkt_retry)
		return false;
	domain->counters.pdc_close_in_err++;
	suet_pds_free_ipdc(domain, ipdc);
	return true;
}