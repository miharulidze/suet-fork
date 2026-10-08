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
 * ACK/NACK/CTRL handlers, retransmit, PDC teardown.
 */

#include "suet_pds.h"
#include "suet.h"

#define SUET_PDS_TX_POOL_CHUNK_CNT 1024

static struct suet_pds_pkt_entry *suet_pds_pkt_init(struct suet_pkt_entry *pkt)
{
	struct suet_pds_pkt_entry *entry = pkt->pds_ctx;

	memset(entry, 0, sizeof(*entry));
	dlist_init(&entry->entry);
	entry->pkt = pkt;
	return entry;
}

static struct suet_pds_pkt_entry *suet_pds_pkt_alloc(struct suet_domain *domain)
{
	struct suet_pkt_entry *pkt = suet_dgram_pkt_alloc(domain);

	return pkt ? suet_pds_pkt_init(pkt) : NULL;
}

static void suet_pds_pkt_free(struct suet_pds_pkt_entry *entry)
{
	assert(dlist_empty(&entry->entry));
	suet_dgram_pkt_free(entry->pkt);
}

size_t suet_pds_max_ses_size(const struct suet_domain *domain)
{
	size_t size = suet_dgram_max_pkt_size(domain);

	return size > sizeof(struct pds_req_hdr) ?
		       size - sizeof(struct pds_req_hdr) :
		       0;
}

void suet_pds_rx_release(void *handle)
{
	suet_pds_pkt_free(handle);
}

static bool suet_pds_parse_ses(struct suet_domain *domain,
			       struct suet_pds_pkt_entry *entry,
			       struct suet_ses_rx_packet *ses)
{
	struct suet_pkt_entry *pkt = entry->pkt;
	const struct pds_req_hdr *pds = pkt->pkt;

	if (pkt->pkt_size < sizeof(*pds) ||
	    !suet_ses_rx_parse(domain, pds_prologue_get_next_hdr(pds),
			       (const char *) pkt->pkt + sizeof(*pds),
			       pkt->pkt_size - sizeof(*pds), ses))
		return false;
	ses->handle = entry;
	return true;
}

static ssize_t suet_pds_send_pkt(struct suet_domain *domain,
				 struct suet_pds_pkt_entry *pkt)
{
	return suet_dgram_send(domain, pkt->pkt);
}

static inline uint16_t suet_domain_allocate_pdcid(struct suet_domain *domain)
{
	uint16_t pdcid = domain->pds.next_pdcid;

	/* Spec 3.5.8.2: "PDCID=0 is reserved and MUST not be used by a PDC." */
	if (++domain->pds.next_pdcid == 0)
		domain->pds.next_pdcid = 1;
	return pdcid;
}

static inline struct suet_ipdc *
suet_pds_ipdc_get_by_local_pdcid(struct suet_domain *domain, uint16_t lpdcid)
{
	return ofi_idm_lookup(&domain->pds.local_pdcid_to_ipdc_idm,
			      (int) lpdcid);
}

static inline struct suet_tpdc *
suet_pds_tpdc_get_by_local_pdcid(struct suet_domain *domain, uint16_t lpdcid)
{
	return ofi_idm_lookup(&domain->pds.local_pdcid_to_tpdc_idm,
			      (int) lpdcid);
}

static inline struct suet_ipdc *
suet_pds_ipdc_get_by_dgram_av_addr(struct suet_domain *domain,
				   fi_addr_t dgram_av_addr)
{
	struct suet_ipdc *ipdc = NULL;

	HASH_FIND(ipdc_dgram_av_addr_handle,
		  domain->pds.ipdc_by_dgram_av_addr_ht, &dgram_av_addr,
		  sizeof(dgram_av_addr), ipdc);
	return ipdc;
}

static inline struct suet_tpdc *
suet_pds_tpdc_get_by_syn_key(struct suet_domain *domain,
			     fi_addr_t dgram_av_addr, uint16_t ipdcid)
{
	struct suet_tpdc_syn_key key = {dgram_av_addr, ipdcid};
	struct suet_tpdc *tpdc = NULL;

	HASH_FIND(tpdc_syn_key_handle, domain->pds.tpdc_by_syn_key_ht, &key,
		  sizeof(struct suet_tpdc_syn_key), tpdc);
	return tpdc;
}

static void suet_pds_tpdc_send_ack(struct suet_domain *domain,
				   struct suet_tpdc *tpdc, uint32_t trigger_psn,
				   uint8_t next_hdr, uint16_t message_id,
				   uint32_t modified_length,
				   uint8_t resp_opcode, uint8_t resp_rc,
				   uint8_t resp_list);
static void suet_pds_send_tx(struct suet_pds_tx_entry *tx);
static void suet_pds_ipdc_progress_tx_pkt_list(struct suet_domain *domain,
					       struct suet_ipdc *ipdc);
static bool suet_pds_free_ipdc_if_retry_exhausted(struct suet_domain *domain,
						  struct suet_ipdc *ipdc);
static void suet_pds_ipdc_send_clear_cmd(struct suet_domain *domain,
					 struct suet_ipdc *ipdc);

static void suet_pds_tx_finish(struct suet_pds_tx_entry *tx, int err,
			       int prov_errno)
{
	void *context = tx->context;

	dlist_remove(&tx->entry);
	ofi_buf_free(tx);
	suet_ses_tx_done(context, err, prov_errno);
}

static void suet_pds_ipdc_progress_tx_list(struct suet_ipdc *ipdc)
{
	struct dlist_entry *tmp_entry;
	struct suet_pds_tx_entry *tx_entry;
	struct suet_pds_pkt_entry *head_pkt_entry;
	uint32_t head_psn;

	/* head_psn = PSN of the lowest unacked (in-flight) packet. When the
	 * in-flight list is empty, that is conceptually one past the highest
	 * cumulatively-acked PSN (last_rx_cack_psn per Spec Table 3-35). */
	if (!dlist_empty(&ipdc->in_flight_pkts)) {
		head_pkt_entry = container_of(ipdc->in_flight_pkts.next,
					      struct suet_pds_pkt_entry, entry);
		head_psn = pds_req_get_psn(
			&((struct suet_req_pkt *) head_pkt_entry->pkt->pkt)
				 ->pds);
	} else {
		head_psn = suet_rel_tx_cack(&ipdc->rel) + 1;
	}

	dlist_foreach_container_safe (&ipdc->tx_list, struct suet_pds_tx_entry,
				      tx_entry, entry, tmp_entry) {
		if (!tx_entry->num_pkts)
			continue;
		/* check that all packets for this message are posted */
		if (tx_entry->next_segment >= tx_entry->num_pkts) {
			/* if yes, check if we got all ACKs for this message */
			if (pds_psn_before(tx_entry->start_psn +
						   (tx_entry->num_pkts - 1),
					   head_psn)) {
				suet_pds_tx_finish(tx_entry, 0, 0);
			}
			continue;
		}

		suet_pds_send_tx(tx_entry);
		/* even if suet_pds_send_tx internally failed due to
		 exhausted window or lack of free packets, no completion
		 ordering (comp_order == 0) allows us to try to progress
		 and complete subsequent messages */
	}

	/* Spec 3.5.11.4.4: CLEAR_PSN is the high-water-mark of PSNs whose
	 * SES responses have been delivered locally. Advance it to the
	 * cumulatively-acked horizon. */
	if (pds_psn_before(ipdc->last_tx_clear_psn,
			   suet_rel_tx_cack(&ipdc->rel)))
		ipdc->last_tx_clear_psn = suet_rel_tx_cack(&ipdc->rel);
}

static void suet_pds_tpdc_send_ack_if_needed(
	struct suet_domain *domain, struct suet_tpdc *tpdc,
	struct suet_req_pkt *pkt,
	const struct suet_ses_rx_dispatch_result *ses_resp)
{
	if (suet_rel_rx_ack_needed(&tpdc->rel,
				   ses_resp->ack_now ||
					   (pds_prologue_get_flags(&pkt->pds) &
					    PDS_FLAG_AR))) {
		suet_pds_tpdc_send_ack(
			domain, tpdc, pds_req_get_psn(&pkt->pds),
			UET_HDR_RESPONSE, ses_resp->resp.message_id,
			ses_resp->resp.modified_length,
			ses_resp->resp.ses_opcode, ses_resp->resp.ses_rc,
			ses_resp->resp.list);
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
	struct suet_pds_ses_resp_entry *gtd_resp;
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
		gtd_resp = container_of(item, struct suet_pds_ses_resp_entry,
					entry);
		if (pds_psn_before(new_clear, gtd_resp->psn))
			break;
		if (gtd_resp->reserved)
			continue;
		dlist_remove(&gtd_resp->entry);
		ofi_buf_free(gtd_resp);
	}
}

/*
 * Spec 3.5.11.4.4 / 3.5.12.5.5: persist a SES response so the destination
 * can replay it on retransmit until CLEAR_PSN moves past its PSN.
 * psn and num_pkts describe the covered request range for duplicate replay.
 *
 * Returns the saved slot, or NULL on pool exhaustion. Caller decides how to
 * surface the failure without advancing expected_rx_psn, allowing retry.
 */
static struct suet_pds_ses_resp_entry *suet_pds_tpdc_save_gtd_del_resp(
	struct suet_domain *domain, struct suet_tpdc *tpdc,
	const struct suet_ses_resp *resp, uint32_t psn, uint16_t num_pkts)
{
	struct suet_pds_ses_resp_entry *saved;

	assert(num_pkts >= 1);
	saved = ofi_buf_alloc(domain->pds.gtd_del_resp_pool);
	if (!saved)
		return NULL;
	saved->resp = *resp;
	saved->psn = psn;
	saved->num_pkts = num_pkts;
	saved->domain = domain;
	saved->tpdc = tpdc;
	saved->reserved = false;
	dlist_init(&saved->entry);
	dlist_insert_tail(&saved->entry, &tpdc->gtd_del_list);
	return saved;
}

struct suet_pds_ses_resp_entry *
suet_pds_response_reserve(struct suet_domain *domain, void *pds_ctx,
			  const struct suet_ses_resp *resp, uint16_t num_pkts)
{
	struct suet_tpdc *route = pds_ctx;
	struct suet_pds_ses_resp_entry *saved;

	if (!route)
		return NULL;
	saved = suet_pds_tpdc_save_gtd_del_resp(
		domain, route, resp, route->expected_rx_psn, num_pkts);
	if (saved)
		saved->reserved = true;
	return saved;
}

void suet_pds_response_cancel(struct suet_pds_ses_resp_entry *response)
{
	if (!response)
		return;
	if (response->tpdc)
		dlist_remove(&response->entry);
	ofi_buf_free(response);
}

void suet_pds_response_complete(struct suet_pds_ses_resp_entry *response,
				const struct suet_ses_resp *resp)
{
	assert(response->reserved);
	if (!response->tpdc) {
		ofi_buf_free(response);
		return;
	}
	response->resp = *resp;
	response->reserved = false;
	suet_pds_tpdc_send_ack(response->domain, response->tpdc, response->psn,
			       UET_HDR_RESPONSE, resp->message_id,
			       resp->modified_length, resp->ses_opcode,
			       resp->ses_rc, resp->list);
}

static void suet_pds_send_nack(struct suet_domain *domain,
			       fi_addr_t dgram_av_addr, uint8_t nack_code,
			       uint32_t nack_psn, uint16_t spdcid,
			       uint16_t dpdcid)
{
	struct suet_pds_pkt_entry *pkt_entry;
	struct pds_nack_hdr *nack;

	pkt_entry = suet_pds_pkt_alloc(domain);
	if (!pkt_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "Unable to send nack\n");
		return;
	}

	nack = (struct pds_nack_hdr *) pkt_entry->pkt->pkt;
	pkt_entry->pkt->pkt_size = sizeof(*nack);
	pkt_entry->pkt->dgram_av_addr = dgram_av_addr;

	pds_nack_init(nack, nack_code, nack_psn, spdcid, dpdcid);

	if (suet_pds_send_pkt(domain, pkt_entry)) {
		suet_pds_pkt_free(pkt_entry);
	} else {
		domain->counters.nacks_tx++;
	}
}

static int
suet_pds_dispatch_req_to_ses_in_order(struct suet_domain *domain,
				      struct suet_tpdc *tpdc,
				      struct suet_pds_pkt_entry *pkt_entry,
				      const struct suet_ses_rx_packet *ses)
{
	struct pds_req_hdr *pds = pkt_entry->pkt->pkt;
	struct suet_ses_rx_dispatch_result result;
	uint8_t nack_code = 0;

	suet_pds_tpdc_advance_rx_clear_psn(tpdc, pds_req_get_clear_psn(pds));
	assert(dlist_empty(&pkt_entry->entry));
	suet_ses_receive(tpdc->ses_ctx, pkt_entry->pkt->ses_ctx, ses, &result);
	if (result.status) {
		nack_code = PDS_NACK_CODE_NO_GTD_DEL_AVAIL;
		goto send_resp;
	}
	if (result.gtd_del &&
	    !suet_pds_tpdc_save_gtd_del_resp(domain, tpdc, &result.resp,
					     pds_req_get_psn(pds), 1)) {
		nack_code = PDS_NACK_CODE_NO_GTD_DEL_AVAIL;
		goto send_resp;
	}
	if (result.accepted) {
		suet_ses_rx_commit(tpdc->ses_ctx, &result);
		tpdc->expected_rx_psn++;
		suet_rel_rx_commit(&tpdc->rel, pds_req_get_psn(pds));
	}
send_resp:
	if (nack_code)
		suet_pds_send_nack(domain, tpdc->dgram_av_addr, nack_code,
				   pds_req_get_psn(pds), tpdc->local_pdcid,
				   tpdc->ipdcid);
	else
		suet_pds_tpdc_send_ack_if_needed(
			domain, tpdc,
			(struct suet_req_pkt *) pkt_entry->pkt->pkt, &result);
	if (!result.pkt_retained)
		suet_pds_pkt_free(pkt_entry);
	return nack_code || !result.accepted ? -FI_EAGAIN : 0;
}

static void suet_pds_dispatch_req_to_ses_ooo(
	struct suet_domain *domain, struct suet_tpdc *tpdc,
	struct suet_pds_pkt_entry *pkt_entry, struct pds_req_hdr *pds_hdr,
	const struct suet_ses_rx_packet *ses, enum suet_rel_rx_result action)
{
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
	if (action == SUET_REL_RX_REPLAY) {
		struct suet_pds_ses_resp_entry *gtd_del_resp;
		uint32_t dup_psn = pds_req_get_psn(pds_hdr);
		bool replayed = false;

		dlist_foreach_container (&tpdc->gtd_del_list,
					 struct suet_pds_ses_resp_entry,
					 gtd_del_resp, entry) {
			if (!pds_psn_before(dup_psn, gtd_del_resp->psn) &&
			    pds_psn_before(dup_psn,
					   gtd_del_resp->psn +
						   gtd_del_resp->num_pkts)) {
				suet_pds_tpdc_send_ack(
					domain, tpdc, dup_psn, UET_HDR_RESPONSE,
					gtd_del_resp->resp.message_id,
					gtd_del_resp->resp.modified_length,
					gtd_del_resp->resp.ses_opcode,
					gtd_del_resp->resp.ses_rc,
					gtd_del_resp->resp.list);
				replayed = true;
				break;
			}
		}
		if (!replayed) {
			struct suet_ses_resp resp;
			suet_ses_default_response(ses->hdr, &resp);
			suet_pds_tpdc_send_ack(
				domain, tpdc, dup_psn, UET_HDR_RESPONSE,
				resp.message_id, resp.modified_length,
				resp.ses_opcode, resp.ses_rc, resp.list);
		}
		goto free_pkt;
	}

	if (action == SUET_REL_RX_GAP)
		/* Reliability reports a gap; PDS chooses the ROD wire code. */
		suet_pds_send_nack(domain, tpdc->dgram_av_addr,
				   PDS_NACK_CODE_ROD_OOO,
				   pds_req_get_psn(pds_hdr), tpdc->local_pdcid,
				   tpdc->ipdcid);
	else
		domain->counters.dup_drops++;
free_pkt:
	suet_pds_pkt_free(pkt_entry);
}

static void suet_pds_process_rx_req(struct suet_domain *domain,
				    struct suet_tpdc *tpdc,
				    struct suet_pds_pkt_entry *pkt_entry)
{
	struct pds_req_hdr *pds = pkt_entry->pkt->pkt;
	struct suet_ses_rx_packet ses;
	enum suet_rel_rx_result action;
	uint32_t psn = pds_req_get_psn(pds);

	if (!suet_pds_parse_ses(domain, pkt_entry, &ses)) {
		suet_pds_pkt_free(pkt_entry);
		return;
	}
	action = suet_rel_rx_record(
		&tpdc->rel, psn, pds_prologue_get_flags(pds) & PDS_FLAG_RETX);
	if (action != SUET_REL_RX_NEW) {
		suet_pds_dispatch_req_to_ses_ooo(domain, tpdc, pkt_entry, pds,
						 &ses, action);
		return;
	}
	/* ROD drops out-of-order requests instead of retaining packet storage.
	 * Keep this delivery constraint independent of reliability admission.
	 */
	if (psn != tpdc->expected_rx_psn) {
		suet_rel_rx_cancel(&tpdc->rel, psn);
		suet_pds_dispatch_req_to_ses_ooo(domain, tpdc, pkt_entry, pds,
						 &ses, SUET_REL_RX_GAP);
		return;
	}
	if (!suet_pds_parse_ses(domain, pkt_entry, &ses)) {
		suet_rel_rx_cancel(&tpdc->rel, psn);
		suet_pds_pkt_free(pkt_entry);
		return;
	}
	if (suet_pds_dispatch_req_to_ses_in_order(domain, tpdc, pkt_entry, &ses))
		suet_rel_rx_cancel(&tpdc->rel, psn);
}

/* SES interprets semantic response codes and owns application errors. */
static void suet_pds_handle_ses_error_resp(struct suet_ipdc *ipdc,
					   const struct ses_resp_hdr *resp)
{
	struct suet_pds_tx_entry *tx;
	struct dlist_entry *tmp;

	dlist_foreach_container_safe (&ipdc->tx_list, struct suet_pds_tx_entry,
				      tx, entry, tmp) {
		if (suet_ses_tx_response(tx->context, resp))
			break;
	}
}

static void suet_pds_free_ipdc(struct suet_domain *domain,
			       struct suet_ipdc *ipdc)
{
	struct suet_pds_pkt_entry *pkt_entry;

	while (!dlist_empty(&ipdc->tx_list)) {
		struct suet_pds_tx_entry *tx = container_of(
			ipdc->tx_list.next, struct suet_pds_tx_entry, entry);
		suet_pds_tx_finish(tx, FI_ECONNREFUSED, 0);
	}

	while (!dlist_empty(&ipdc->in_flight_pkts)) {
		dlist_pop_front(&ipdc->in_flight_pkts,
				struct suet_pds_pkt_entry, pkt_entry, entry);
		dlist_init(&pkt_entry->entry);
		if (suet_dgram_pkt_in_use(pkt_entry->pkt)) {
			/* NIC still owns the buffer; we cannot free it yet.
			 * Mark it ACKED and self-loop its entry so the TX
			 * completion handler takes the ACKED branch (where
			 * the ipdc lookup will fail) and the completion handler
			 * can safely unlink it. */
			pkt_entry->acked = true;
		} else {
			suet_pds_pkt_free(pkt_entry);
		}
	}

	dlist_remove(&ipdc->entry);
	HASH_DELETE(ipdc_dgram_av_addr_handle,
		    domain->pds.ipdc_by_dgram_av_addr_ht, ipdc);
	ofi_idm_clear(&domain->pds.local_pdcid_to_ipdc_idm,
		      (int) ipdc->local_pdcid);

	ipdc->state = SUET_PDC_CLOSED;
	free(ipdc->tx_pkts);
	suet_rel_tx_cleanup(&ipdc->rel);
	ofi_buf_free(ipdc);
}

static void suet_pds_ipdc_process_ack(struct suet_domain *domain,
				      struct suet_pds_pkt_entry *ack_entry)
{
	struct suet_ack_pkt *ack = (struct suet_ack_pkt *) ack_entry->pkt->pkt;
	uint32_t cack_psn = pds_ack_get_cack_psn(&ack->pds);
	bool has_ses_resp = pds_ack_get_next_hdr(&ack->pds) == UET_HDR_RESPONSE;
	struct suet_rel_retired retired;
	enum suet_rel_ack_result action;
	struct suet_pds_pkt_entry *pkt_entry;
	uint32_t i, slot;
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

	/* SES error ACK (e.g. RC_NO_MATCH) or stale/duplicate ACK:
	 * don't advance the sender window. Still check for RTO'ed in-flight
	 * packets so we retransmit promptly instead of waiting for the next
	 * domain progress cycle.
	 *
	 * Spec 3.4.3 / Table 3-19: a non-RC_OK SES response is a fatal
	 * semantic failure. Surface it as a CQ error completion on the
	 * matching tx_entry instead of letting RTO loop until the retry
	 * budget is exhausted. */
	if (has_ses_resp && suet_ses_response_is_error(&ack->ses)) {
		domain->counters.error_acks_rx++;
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"Received ACK with SES error: opcode=%d rc=%d "
			"cack_psn=%u dpdcid=%d\n",
			ses_resp_ctrl_get_opcode(&ack->ses),
			ses_resp_ctrl_get_rc(&ack->ses), cack_psn,
			pds_ack_get_dpdcid(&ack->pds));
		suet_pds_handle_ses_error_resp(ipdc, &ack->ses);
		goto retry_check;
	}
	action = suet_rel_tx_ack(&ipdc->rel, cack_psn, &retired);
	if (action == SUET_REL_ACK_STALE) {
		domain->counters.stale_acks_rx++;
		goto retry_check;
	}
	if (action == SUET_REL_ACK_INVALID)
		goto out;
	suet_cc_ack(&ipdc->cc, retired.count);

	/* Spec 3.5.8.3: Close ACK frees the ipdc and ends the lifecycle.
	 * Identified by being in CLOSE_ACK_WAIT with a cack covering close_psn.
	 * After validating the ACK, end the lifecycle without feeding the
	 * closing tx_list back into progress. */
	if (ipdc->state == SUET_PDC_CLOSE_ACK_WAIT &&
	    pds_psn_after_eq(cack_psn, ipdc->close_psn)) {
		FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
		       "Close ACK received for ipdc pdcid=%u close_psn=%u\n",
		       ipdc->local_pdcid, ipdc->close_psn);
		domain->counters.pdc_closes_ok++;
		suet_pds_free_ipdc(domain, ipdc);
		goto out;
	}

	/* Consume retired slots before any progress can submit into the new
	 * window. */
	for (i = 0; i < retired.count; i++) {
		slot = (retired.first_slot + i) % ipdc->rel.window.capacity;
		pkt_entry = ipdc->tx_pkts[slot];
		assert(pkt_entry && pkt_entry->psn == retired.first_psn + i);
		ipdc->tx_pkts[slot] = NULL;
		if (suet_dgram_pkt_in_use(pkt_entry->pkt)) {
			pkt_entry->acked = true;
		} else {
			dlist_remove_init(&pkt_entry->entry);
			suet_pds_pkt_free(pkt_entry);
		}
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
	    dlist_empty(&ipdc->tx_list) && dlist_empty(&ipdc->in_flight_pkts) &&
	    ipdc->state == SUET_PDC_ESTABLISHED)
		suet_pds_ipdc_send_clear_cmd(domain, ipdc);
	goto out;

retry_check:
	suet_pds_ipdc_progress_tx_pkt_list(domain, ipdc);
	(void) suet_pds_free_ipdc_if_retry_exhausted(domain, ipdc);
out:
	suet_pds_pkt_free(ack_entry);
}

static void suet_pds_ipdc_process_nack(struct suet_domain *domain,
				       struct suet_pds_pkt_entry *nack_entry)
{
	struct pds_nack_hdr *nack =
		(struct pds_nack_hdr *) nack_entry->pkt->pkt;
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
	(void) suet_rel_tx_nack(&ipdc->rel, pds_nack_get_psn(nack));

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Received NACK: code=%d nack_psn=%u dpdcid=%d\n",
	       pds_nack_get_code(nack), pds_nack_get_psn(nack),
	       pds_nack_get_dpdcid(nack));
out:
	suet_pds_pkt_free(nack_entry);
}

void suet_pds_tx_done(struct suet_domain *domain, struct suet_pkt_entry *pkt,
		      int status)
{
	struct suet_pds_pkt_entry *pkt_entry = pkt->pds_ctx;
	struct suet_ipdc *ipdc;
	struct pds_req_hdr *pds =
		&((struct suet_req_pkt *) pkt_entry->pkt->pkt)->pds;
	uint16_t lpdcid;

	FI_DBG(&suet_prov, FI_LOG_EP_DATA,
	       "got send completion (type: %s, status: %d)\n",
	       pds_pkt_type_name(pds_prologue_get_type(pds)), status);

	switch (pds_prologue_get_type(pds)) {
	case PDS_ACK:
	case PDS_NACK:
		suet_pds_pkt_free(pkt_entry);
		break;
	case PDS_CP:
		/* Spec 3.5.16.3.1: Clear Cmd CP is never linked into
		 * in_flight_pkts and the receiver does not ACK it. Free
		 * directly on TX completion. Close Cmd CP IS linked and IS
		 * ACKed; it falls through to PDS_ROD_REQ. */
		if (pds_ctrl_get_ctl_type((struct pds_ctrl_hdr *) pds) ==
		    PDS_CTL_CLEAR_CMD) {
			suet_pds_pkt_free(pkt_entry);
			break;
		}
		/* fallthrough */
	case PDS_ROD_REQ:
		if (pkt_entry->acked) {
			lpdcid = pkt_entry->local_pdcid;
			ipdc = suet_pds_ipdc_get_by_local_pdcid(domain, lpdcid);
			dlist_remove_init(&pkt_entry->entry);
			suet_pds_pkt_free(pkt_entry);
			if (ipdc) {
				/* try to send more if window allows and we have
				 * outstanding data */
				suet_pds_ipdc_progress_tx_list(ipdc);
			} else {
				/* ipdc was freed (e.g. Close ACK).
				 * Buffer is already gone. */
			}
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
	suet_rel_rx_cleanup(&tpdc->rel);

	suet_ses_rx_close(tpdc->ses_ctx);

	/* Drain any saved guaranteed-delivery responses; the peer is gone
	 * so retained state is no longer needed. */
	while (!dlist_empty(&tpdc->gtd_del_list)) {
		struct suet_pds_ses_resp_entry *resp;
		dlist_pop_front(&tpdc->gtd_del_list,
				struct suet_pds_ses_resp_entry, resp, entry);
		resp->tpdc = NULL;
		if (!resp->reserved)
			ofi_buf_free(resp);
	}

	dlist_remove(&tpdc->entry);
	HASH_DELETE(tpdc_syn_key_handle, domain->pds.tpdc_by_syn_key_ht, tpdc);
	ofi_idm_clear(&domain->pds.local_pdcid_to_tpdc_idm,
		      (int) tpdc->local_pdcid);

	tpdc->state = SUET_PDC_CLOSED;
	ofi_buf_free(tpdc);
}

static void suet_pds_process_cp(struct suet_domain *domain,
				fi_addr_t dgram_av_addr,
				struct suet_pds_pkt_entry *pkt_entry)
{
	struct suet_ctrl_pkt *cp = (struct suet_ctrl_pkt *) pkt_entry->pkt->pkt;
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
		suet_pds_send_nack(domain, dgram_av_addr,
				   PDS_NACK_CODE_INV_DPDCID, cp_psn, 0,
				   pds_ctrl_get_spdcid(cphdr));
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
		if (suet_ses_rx_busy(tpdc->ses_ctx)) {
			FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
				"Close Cmd on non-idle tpdc pdcid=%u\n",
				tpdc->local_pdcid);
			domain->counters.pdc_close_in_err++;
		}

		if (pds_psn_after_eq(cp_psn, tpdc->expected_rx_psn)) {
			tpdc->expected_rx_psn = cp_psn + 1;
			suet_rel_rx_reset(&tpdc->rel, cp_psn + 1);
		}

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
	suet_pds_pkt_free(pkt_entry);
}

static struct suet_tpdc *suet_pds_allocate_tpdc(struct suet_domain *domain,
						fi_addr_t dgram_av_addr,
						uint16_t ipdcid,
						uint32_t ipdc_start_psn)
{
	struct suet_tpdc *tpdc;

	tpdc = ofi_buf_alloc(domain->pds.tpdc_pool);
	if (!tpdc)
		return NULL;

	memset(tpdc, 0, sizeof(*tpdc));
	tpdc->local_pdcid = suet_domain_allocate_pdcid(domain);
	tpdc->ipdcid = ipdcid;
	tpdc->dgram_av_addr = dgram_av_addr;
	tpdc->ses_ctx = suet_ses_rx_open(
		domain, tpdc,
		suet_dgram_av_get_peer_idx_by_addr(domain, dgram_av_addr));
	if (!tpdc->ses_ctx) {
		ofi_buf_free(tpdc);
		return NULL;
	}
	tpdc->expected_rx_psn = ipdc_start_psn;
	if (suet_rel_rx_init(&tpdc->rel, ipdc_start_psn, suet_env.max_unacked))
		goto err;
	/* Spec 3.5.11.4.4: CLEAR_PSN starts at Start_PSN - 1. */
	tpdc->last_rx_clear_psn = ipdc_start_psn - 1;
	tpdc->state = SUET_PDC_ESTABLISHED;

	dlist_init(&tpdc->gtd_del_list);

	if (ofi_idm_set(&domain->pds.local_pdcid_to_tpdc_idm,
			(int) tpdc->local_pdcid, tpdc) < 0)
		goto err;

	HASH_ADD(tpdc_syn_key_handle, domain->pds.tpdc_by_syn_key_ht,
		 dgram_av_addr, sizeof(struct suet_tpdc_syn_key), tpdc);

	dlist_insert_tail(&tpdc->entry, &domain->pds.active_tpdc_list);

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Created tpdc: local_pdcid=%u ipdcid=%u "
	       "dgram_av_addr=%ld start_psn=%u\n",
	       tpdc->local_pdcid, ipdcid, (long) dgram_av_addr, ipdc_start_psn);

	return tpdc;

err:
	suet_rel_rx_cleanup(&tpdc->rel);
	suet_ses_rx_close(tpdc->ses_ctx);
	ofi_buf_free(tpdc);
	return NULL;
}

static struct suet_tpdc *
suet_pds_resolve_tpdc_on_syn(struct suet_domain *domain,
			     fi_addr_t peer_dgram_av_addr, uint16_t ipdcid,
			     uint32_t ipdc_start_psn)
{
	struct suet_tpdc *tpdc = suet_pds_tpdc_get_by_syn_key(
		domain, peer_dgram_av_addr, ipdcid);
	return tpdc ? tpdc :
		      suet_pds_allocate_tpdc(domain, peer_dgram_av_addr, ipdcid,
					     ipdc_start_psn);
}

static void suet_pds_tpdc_process_rod_pkt(struct suet_domain *domain,
					  fi_addr_t dgram_av_addr,
					  struct suet_pds_pkt_entry *pkt_entry)
{
	struct suet_req_pkt *pkt = (struct suet_req_pkt *) pkt_entry->pkt->pkt;
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

		tpdc = suet_pds_resolve_tpdc_on_syn(domain, dgram_av_addr,
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
	suet_pds_send_nack(domain, dgram_av_addr, nack_code,
			   pds_req_get_psn(pds), 0, pds_req_get_spdcid(pds));
	suet_pds_pkt_free(pkt_entry);
}

void suet_pds_receive(struct suet_domain *domain, struct suet_pkt_entry *pkt,
		      fi_addr_t dgram_av_addr)
{
	struct suet_pds_pkt_entry *pkt_entry = suet_pds_pkt_init(pkt);
	struct pds_req_hdr *pds =
		&((struct suet_req_pkt *) pkt_entry->pkt->pkt)->pds;
	uint8_t nh;

	if (pkt->pkt_size < sizeof(union pds_prologue))
		goto free_pkt;

	FI_DBG(&suet_prov, FI_LOG_EP_DATA, "got recv completion (type: %s)\n",
	       pds_pkt_type_name(pds_prologue_get_type(pds)));

	switch (pds_prologue_get_type(pds)) {
	case PDS_ACK:
		if (pkt->pkt_size < sizeof(struct pds_ack_hdr))
			goto free_pkt;
		nh = pds_ack_get_next_hdr((struct pds_ack_hdr *) pds);
		if (nh != UET_HDR_RESPONSE && nh != UET_HDR_NONE)
			goto bad_next_hdr;
		if (nh == UET_HDR_RESPONSE &&
		    pkt->pkt_size < sizeof(struct pds_ack_hdr) +
					    sizeof(struct ses_resp_hdr))
			goto free_pkt;
		suet_pds_ipdc_process_ack(domain, pkt_entry);
		break;
	case PDS_NACK:
		if (pkt->pkt_size < sizeof(struct pds_nack_hdr))
			goto free_pkt;
		suet_pds_ipdc_process_nack(domain, pkt_entry);
		break;
	case PDS_ROD_REQ:
		if (pkt->pkt_size < sizeof(*pds))
			goto free_pkt;
		if (pds_prologue_get_next_hdr(pds) != UET_HDR_REQUEST_STD)
			goto bad_next_hdr;
		suet_pds_tpdc_process_rod_pkt(domain, dgram_av_addr, pkt_entry);
		break;
	case PDS_CP:
		if (pkt->pkt_size < sizeof(struct pds_ctrl_hdr))
			goto free_pkt;
		suet_pds_process_cp(domain, dgram_av_addr, pkt_entry);
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
	suet_pds_pkt_free(pkt_entry);
}

static struct suet_pds_pkt_entry *suet_pds_ipdc_generate_new_req_pkt(
	struct suet_domain *domain, struct suet_pds_tx_entry *tx,
	struct suet_ipdc *ipdc, uint32_t psn, uint16_t flags)
{
	struct suet_pds_pkt_entry *pkt_entry;
	struct pds_req_hdr *pds;
	struct suet_pkt_entry *pkt;
	struct suet_ses_tx_segment ses;
	size_t max_ses_size = suet_pds_max_ses_size(domain);
	size_t hdr_len;
	int ret;

	pkt_entry = suet_pds_pkt_alloc(domain);
	if (!pkt_entry)
		return NULL;

	pkt = pkt_entry->pkt;
	pds = pkt->pkt;
	ses.iov = &pkt->zc_pld_iov[1];
	ses.desc = &pkt->zc_pld_desc[1];
	ses.iov_capacity = SUET_IOV_LIMIT;
	ret = suet_ses_prepare_tx(tx->context, tx->next_segment,
				  (char *) pkt->pkt + sizeof(*pds),
				  max_ses_size, &ses);
	if (!ret && (ses.hdr_len > max_ses_size ||
		     ses.payload_len > max_ses_size - ses.hdr_len))
		ret = -FI_EMSGSIZE;
	if (ret) {
		suet_pds_pkt_free(pkt_entry);
		suet_pds_tx_finish(tx, -ret, 0);
		return NULL;
	}
	hdr_len = sizeof(*pds) + ses.hdr_len;
	if (ses.zero_copy && ses.iov_count) {
		pkt->zc_pld_iov_count = ses.iov_count;
		pkt->zc_pld_iov[0].iov_base = pkt->pkt;
		pkt->zc_pld_iov[0].iov_len = hdr_len;
	} else {
		ofi_copy_from_iov((char *) pkt->pkt + hdr_len, ses.payload_len,
				  ses.iov, ses.iov_count, 0);
	}
	pkt->pkt_size = hdr_len + ses.payload_len;
	memset(pds, 0, sizeof(*pds));
	pds_prologue_set_type(pds, PDS_ROD_REQ);
	pds_prologue_set_next_hdr(pds, ses.hdr_type);
	pds_req_set_psn(pds, psn);
	pkt_entry->psn = psn;
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

	pkt_entry->pkt->dgram_av_addr = ipdc->dgram_av_addr;
	pkt_entry->local_pdcid = ipdc->local_pdcid;

	return pkt_entry;
}

static void
suet_pds_ipdc_insert_unacked_pkt(struct suet_ipdc *ipdc,
				 struct suet_pds_pkt_entry *pkt_entry)
{
	uint32_t slot;
	bool valid = suet_rel_slot(&ipdc->rel.window, pkt_entry->psn, &slot);

	assert(valid && !ipdc->tx_pkts[slot]);
	if (!valid)
		return;
	ipdc->tx_pkts[slot] = pkt_entry;
	suet_rel_tx_track(&ipdc->rel, pkt_entry->psn);
	suet_cc_track(&ipdc->cc);
	dlist_insert_tail(&pkt_entry->entry, &ipdc->in_flight_pkts);
}

static void suet_pds_ipdc_send_tracked_pkt(struct suet_domain *domain,
					   struct suet_ipdc *ipdc,
					   struct suet_pds_pkt_entry *pkt_entry)
{
	/* On send failure: PSN already consumed; enqueue so subsequent progress
	 * call can retransmits. */
	suet_pds_ipdc_insert_unacked_pkt(ipdc, pkt_entry);
	suet_rel_tx_attempt(&ipdc->rel, pkt_entry->psn,
			    suet_domain_now_ms(domain));
	(void) suet_pds_send_pkt(domain, pkt_entry);
}

static void suet_pds_send_tx(struct suet_pds_tx_entry *tx)
{
	struct suet_ipdc *ipdc = tx->ipdc;
	struct suet_pds_pkt_entry *pkt_entry;

	if (!suet_cc_can_send(&ipdc->cc))
		return;
	if (!tx->started) {
		tx->started = true;
		tx->start_psn = ipdc->tx_seq_no;
		ipdc->tx_seq_no += tx->num_pkts;
	}
	while (tx->next_segment < tx->num_pkts && suet_cc_can_send(&ipdc->cc)) {
		if (!suet_rel_tx_can_track(&ipdc->rel,
					   tx->start_psn + tx->next_segment))
			return;
		pkt_entry = suet_pds_ipdc_generate_new_req_pkt(
			tx->domain, tx, ipdc, tx->start_psn + tx->next_segment,
			0);
		if (!pkt_entry)
			return;
		suet_pds_ipdc_send_tracked_pkt(tx->domain, ipdc, pkt_entry);
		tx->next_segment++;
	}
}

void suet_pds_tx_submit(void *pds_ctx, uint32_t num_pkts)
{
	struct suet_pds_tx_entry *tx = pds_ctx;

	tx->num_pkts = num_pkts;
	suet_pds_send_tx(tx);
}

void suet_pds_tx_cancel(void *pds_ctx)
{
	struct suet_pds_tx_entry *tx = pds_ctx;

	if (!tx)
		return;
	dlist_remove(&tx->entry);
	ofi_buf_free(tx);
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
static void suet_pds_tpdc_send_ack(struct suet_domain *domain,
				   struct suet_tpdc *tpdc, uint32_t trigger_psn,
				   uint8_t next_hdr, uint16_t message_id,
				   uint32_t modified_length,
				   uint8_t resp_opcode, uint8_t resp_rc,
				   uint8_t resp_list)
{
	struct suet_pds_pkt_entry *pkt_entry;
	struct suet_ack_pkt *ack;
	uint32_t cack_psn;

	pkt_entry = suet_pds_pkt_alloc(domain);
	if (!pkt_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "Unable to send ack\n");
		return;
	}

	ack = (struct suet_ack_pkt *) pkt_entry->pkt->pkt;
	pkt_entry->pkt->pkt_size =
		(next_hdr == UET_HDR_RESPONSE ? sizeof(*ack) :
						sizeof(ack->pds));
	pkt_entry->pkt->dgram_av_addr = tpdc->dgram_av_addr;

	/* TX buffers are recycled from a pool; zero the prologue. */
	ack->pds.prologue.raw = 0;

	/* Spec Table 3-35: cack_psn = highest in-order PSN received
	 * ("all PDS Requests with PSN prior and including this PSN are
	 * acknowledged"). Reliability reports this accepted receive horizon. */
	cack_psn = suet_rel_rx_cack(&tpdc->rel);

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

	if (suet_pds_send_pkt(domain, pkt_entry))
		suet_pds_pkt_free(pkt_entry);

	suet_rel_rx_ack_sent(&tpdc->rel);
}

static struct suet_ipdc *suet_pds_allocate_ipdc(struct suet_domain *domain,
						fi_addr_t dgram_av_addr)
{
	struct suet_ipdc *ipdc;

	ipdc = ofi_buf_alloc(domain->pds.ipdc_pool);
	if (!ipdc)
		return NULL;

	memset(ipdc, 0, sizeof(*ipdc));
	ipdc->local_pdcid = suet_domain_allocate_pdcid(domain);
	ipdc->tpdcid = 0;
	ipdc->dgram_av_addr = dgram_av_addr;
	ipdc->start_psn = pds_generate_start_psn(&domain->pds.psn_seed, 0);
	ipdc->tx_seq_no = ipdc->start_psn;
	suet_cc_init(&ipdc->cc, suet_env.max_unacked);
	/* Spec Table 3-35: cack_psn = highest acked PSN. Before any ACK has
	 * been received nothing in [start_psn, ...] is acked, so initialize
	 * to start_psn - 1 (i.e. one before the first PSN we will send). */
	if (suet_rel_tx_init(&ipdc->rel, ipdc->start_psn, suet_env.max_unacked,
			     suet_env.max_pkt_retry))
		goto err;
	ipdc->tx_pkts = calloc(suet_env.max_unacked, sizeof(*ipdc->tx_pkts));
	if (!ipdc->tx_pkts)
		goto err;
	/* Spec 3.5.11.4.4: CLEAR_PSN initialized to Start_PSN - 1 in both
	 * directions. Eager-clear: advanced with the cumulative ACK horizon. */
	ipdc->last_tx_clear_psn = ipdc->start_psn - 1;
	ipdc->state = SUET_PDC_OPENING;
	ipdc->teardown_pending = false;
	ipdc->close_psn = 0;
	dlist_init(&ipdc->tx_list);
	dlist_init(&ipdc->in_flight_pkts);

	if (ofi_idm_set(&domain->pds.local_pdcid_to_ipdc_idm,
			(int) ipdc->local_pdcid, ipdc) < 0)
		goto err;

	HASH_ADD(ipdc_dgram_av_addr_handle,
		 domain->pds.ipdc_by_dgram_av_addr_ht, dgram_av_addr,
		 sizeof(ipdc->dgram_av_addr), ipdc);

	dlist_insert_tail(&ipdc->entry, &domain->pds.active_ipdc_list);

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Created ipdc: pdcid=%u dgram_av_addr=%ld\n", ipdc->local_pdcid,
	       (long) dgram_av_addr);

	return ipdc;

err:
	free(ipdc->tx_pkts);
	suet_rel_tx_cleanup(&ipdc->rel);
	ofi_buf_free(ipdc);
	return NULL;
}

static struct suet_ipdc *suet_pds_assign_ipdc(struct suet_domain *domain,
					      fi_addr_t dgram_av_addr)
{
	struct suet_ipdc *ipdc =
		suet_pds_ipdc_get_by_dgram_av_addr(domain, dgram_av_addr);

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
	return ipdc ? ipdc : suet_pds_allocate_ipdc(domain, dgram_av_addr);
}

static void suet_pds_ipdc_send_clear_cmd(struct suet_domain *domain,
					 struct suet_ipdc *ipdc)
{
	struct suet_pds_pkt_entry *pkt_entry;
	struct suet_ctrl_pkt *cp;

	pkt_entry = suet_pds_pkt_alloc(domain);
	if (!pkt_entry) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"Unable to send Clear Cmd CP\n");
		return;
	}

	cp = (struct suet_ctrl_pkt *) pkt_entry->pkt->pkt;
	pkt_entry->pkt->pkt_size = sizeof(cp->pds);
	pkt_entry->pkt->dgram_av_addr = ipdc->dgram_av_addr;

	/* Spec 3.5.16.3.1: pds.psn = 0, pds.flags.ar = 0 (fire-and-forget,
	 * no ACK expected); CLEAR_PSN is carried in pds.payload. */
	pds_ctrl_init(&cp->pds, PDS_CTL_CLEAR_CMD, 0, ipdc->local_pdcid,
		      ipdc->tpdcid, ipdc->last_tx_clear_psn);

	if (suet_pds_send_pkt(domain, pkt_entry))
		suet_pds_pkt_free(pkt_entry);

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Sent Clear Cmd CP: ipdcid=%u tpdcid=%u clear_psn=%u\n",
	       ipdc->local_pdcid, ipdc->tpdcid, ipdc->last_tx_clear_psn);
}

static void suet_pds_ipdc_send_close_cmd(struct suet_domain *domain,
					 struct suet_ipdc *ipdc)
{
	struct suet_pds_pkt_entry *pkt_entry;
	struct suet_ctrl_pkt *cp;

	/* On pkt_entry alloc failure we simply return; the next progress tick
	 * will call us again. */
	pkt_entry = suet_pds_pkt_alloc(domain);
	if (!pkt_entry)
		return;

	cp = (struct suet_ctrl_pkt *) pkt_entry->pkt->pkt;
	pkt_entry->pkt->pkt_size = sizeof(*cp);
	pkt_entry->pkt->dgram_av_addr = ipdc->dgram_av_addr;
	pkt_entry->local_pdcid = ipdc->local_pdcid;

	ipdc->close_psn = ipdc->tx_seq_no++;
	pkt_entry->psn = ipdc->close_psn;
	/* Spec 3.5.16.4.1: Close Command CP consumes a forward PSN and
	 * carries no payload. */
	pds_ctrl_init(&cp->pds, PDS_CTL_CLOSE_CMD, ipdc->close_psn,
		      ipdc->local_pdcid, ipdc->tpdcid, 0);

	suet_pds_ipdc_insert_unacked_pkt(ipdc, pkt_entry);
	ipdc->state = SUET_PDC_CLOSE_ACK_WAIT;

	/* On NIC send failure the pkt sits on in_flight_pkts and
	 * the RTO path retransmits it. */
	suet_rel_tx_attempt(&ipdc->rel, pkt_entry->psn,
			    suet_domain_now_ms(domain));
	if (suet_pds_send_pkt(domain, pkt_entry)) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"failed to send Close Command CP for pdcid=%u; will "
			"retry\n",
			ipdc->local_pdcid);
	}

	FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
	       "Sent Close Command CP: ipdcid=%u tpdcid=%u close_psn=%u\n",
	       ipdc->local_pdcid, ipdc->tpdcid, ipdc->close_psn);
}

static void suet_pds_ipdc_begin_teardown(struct suet_ipdc *ipdc)
{
	/* Mark teardown requested. Actual QUIESCE / Close Cmd happens via
	 * progress once OPENING resolves to ESTABLISHED (so we have a valid
	 * tpdcid to put in the Close Cmd CP). */
	ipdc->teardown_pending = true;
}

static void suet_pds_ipdc_progress_teardown(struct suet_domain *domain,
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

static void suet_pds_ipdc_progress_tx_pkt_list(struct suet_domain *domain,
					       struct suet_ipdc *ipdc)
{
	struct suet_pds_pkt_entry *pkt_entry;
	uint64_t current = suet_domain_now_ms(domain);
	uint32_t cursor = 0, psn, slot;
	bool retry = false;

	while (suet_rel_tx_retry_next(&ipdc->rel, current, &cursor, &psn)) {
		if (!suet_rel_slot(&ipdc->rel.window, psn, &slot))
			break;
		pkt_entry = ipdc->tx_pkts[slot];
		assert(pkt_entry && pkt_entry->psn == psn);
		if (suet_dgram_pkt_in_use(pkt_entry->pkt))
			break;
		retry = true;
		pds_prologue_set_flags(
			pkt_entry->pkt->pkt,
			pds_prologue_get_flags(pkt_entry->pkt->pkt) |
				PDS_FLAG_RETX);
		suet_rel_tx_attempt(&ipdc->rel, psn,
				    suet_domain_now_ms(domain));
		if (suet_pds_send_pkt(domain, pkt_entry))
			break;
	}
	suet_rel_tx_retry_end(&ipdc->rel, retry);
}

static bool suet_pds_free_ipdc_if_retry_exhausted(struct suet_domain *domain,
						  struct suet_ipdc *ipdc)
{
	if (!suet_rel_tx_failed(&ipdc->rel))
		return false;
	domain->counters.pdc_close_in_err++;
	suet_pds_free_ipdc(domain, ipdc);
	return true;
}

void *suet_pds_tx_alloc(struct suet_domain *domain, int peer_idx, void *context)
{
	fi_addr_t addr = suet_dgram_av_get_addr_by_peer_idx(domain, peer_idx);
	struct suet_ipdc *ipdc = suet_pds_assign_ipdc(domain, addr);
	struct suet_pds_tx_entry *tx;

	if (!ipdc)
		return NULL;
	tx = ofi_buf_alloc(domain->pds.pds_tx_pool);
	if (!tx)
		return NULL;
	memset(tx, 0, sizeof(*tx));
	tx->domain = domain;
	tx->ipdc = ipdc;
	tx->context = context;
	dlist_insert_tail(&tx->entry, &ipdc->tx_list);
	return tx;
}

void suet_pds_progress(struct suet_domain *domain)
{
	struct suet_ipdc *ipdc;
	struct dlist_entry *tmp;

	dlist_foreach_container_safe (&domain->pds.active_ipdc_list,
				      struct suet_ipdc, ipdc, entry, tmp) {
		suet_pds_ipdc_progress_tx_pkt_list(domain, ipdc);
		if (suet_pds_free_ipdc_if_retry_exhausted(domain, ipdc))
			continue;
		if (dlist_empty(&ipdc->in_flight_pkts))
			suet_pds_ipdc_progress_tx_list(ipdc);
		suet_pds_ipdc_progress_teardown(domain, ipdc);
	}
}

int suet_pds_drain(struct suet_domain *domain, bool blocking)
{
	struct suet_ipdc *ipdc;
	struct dlist_entry *tmp;

	dlist_foreach_container_safe (&domain->pds.active_ipdc_list,
				      struct suet_ipdc, ipdc, entry, tmp)
		suet_pds_ipdc_begin_teardown(ipdc);

	while (!dlist_empty(&domain->pds.active_ipdc_list) ||
	       !dlist_empty(&domain->pds.active_tpdc_list)) {
		suet_domain_progress(domain);
		if (!blocking && (!dlist_empty(&domain->pds.active_ipdc_list) ||
				  !dlist_empty(&domain->pds.active_tpdc_list)))
			return -FI_EAGAIN;
	}
	return 0;
}

void suet_pds_cleanup(struct suet_domain *suet_domain)
{
	ofi_idm_reset(&suet_domain->pds.local_pdcid_to_ipdc_idm, NULL);
	ofi_idm_reset(&suet_domain->pds.local_pdcid_to_tpdc_idm, NULL);
	HASH_CLEAR(ipdc_dgram_av_addr_handle,
		   suet_domain->pds.ipdc_by_dgram_av_addr_ht);
	HASH_CLEAR(tpdc_syn_key_handle, suet_domain->pds.tpdc_by_syn_key_ht);

	if (suet_domain->pds.ipdc_pool)
		ofi_bufpool_destroy(suet_domain->pds.ipdc_pool);
	if (suet_domain->pds.tpdc_pool)
		ofi_bufpool_destroy(suet_domain->pds.tpdc_pool);
	if (suet_domain->pds.gtd_del_resp_pool)
		ofi_bufpool_destroy(suet_domain->pds.gtd_del_resp_pool);
	if (suet_domain->pds.pds_tx_pool)
		ofi_bufpool_destroy(suet_domain->pds.pds_tx_pool);
	memset(&suet_domain->pds, 0, sizeof(suet_domain->pds));
	dlist_init(&suet_domain->pds.active_ipdc_list);
	dlist_init(&suet_domain->pds.active_tpdc_list);
}

int suet_pds_init(struct suet_domain *suet_domain)
{
	int ret;

	ret = ofi_bufpool_create(
		&suet_domain->pds.ipdc_pool, sizeof(struct suet_ipdc),
		SUET_BUF_POOL_ALIGNMENT, 0, suet_env.max_peers, 0);
	if (ret)
		goto err;

	ret = ofi_bufpool_create(
		&suet_domain->pds.tpdc_pool, sizeof(struct suet_tpdc),
		SUET_BUF_POOL_ALIGNMENT, 0, suet_env.max_peers, 0);
	if (ret)
		goto err;

	ret = ofi_bufpool_create(&suet_domain->pds.gtd_del_resp_pool,
				 sizeof(struct suet_pds_ses_resp_entry),
				 SUET_BUF_POOL_ALIGNMENT,
				 suet_env.max_gtd_del_resp_pool_size, 0, 0);
	if (ret)
		goto err;

	memset(&suet_domain->pds.local_pdcid_to_ipdc_idm, 0,
	       sizeof(suet_domain->pds.local_pdcid_to_ipdc_idm));
	memset(&suet_domain->pds.local_pdcid_to_tpdc_idm, 0,
	       sizeof(suet_domain->pds.local_pdcid_to_tpdc_idm));
	suet_domain->pds.ipdc_by_dgram_av_addr_ht = NULL;
	suet_domain->pds.tpdc_by_syn_key_ht = NULL;
	dlist_init(&suet_domain->pds.active_ipdc_list);
	dlist_init(&suet_domain->pds.active_tpdc_list);
	suet_domain->pds.next_pdcid =
		1; /* PDS Spec 3.5.8.2: PDCID 0 is reserved */
	suet_domain->pds.psn_seed = ofi_generate_seed();

	ret = ofi_bufpool_create(
		&suet_domain->pds.pds_tx_pool, sizeof(struct suet_pds_tx_entry),
		SUET_BUF_POOL_ALIGNMENT, 0, SUET_PDS_TX_POOL_CHUNK_CNT, 0);
	if (ret)
		goto err;
	return 0;
err:
	suet_pds_cleanup(suet_domain);
	return ret;
}
