/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include <config.h>
/* Match the library's internal pool layout before enabling assertions. */
#if !ENABLE_DEBUG && !defined(NDEBUG)
#define NDEBUG
#endif
#include "suet.h"
#include "suet_pds.h"
#include "suet_pds_dgram_api.h"
#undef NDEBUG
#include <assert.h>

/* Include the implementation to exercise private provider completion paths.
 * Only the upward events are replaced; production keeps direct calls.
 */
#define suet_pds_receive test_pds_receive
#define suet_pds_tx_done test_pds_tx_done
void test_pds_receive(struct suet_domain *, struct suet_pkt_entry *, fi_addr_t);
void test_pds_tx_done(struct suet_domain *, struct suet_pkt_entry *, int);
#include "../suet/src/suet_dgram.c"
#undef suet_pds_receive
#undef suet_pds_tx_done

static struct suet_pkt_entry *received, *completed;
static fi_addr_t received_addr;
static int completed_status, send_status, close_status;
static void *send_context, *recv_context;
static unsigned char sent[256];
static size_t sent_len, prefix_size;
static bool ep_closed, dispatch_pds_tx;

void test_pds_receive(struct suet_domain *domain, struct suet_pkt_entry *pkt,
		      fi_addr_t src_addr)
{
	assert(!suet_dgram_pkt_in_use(pkt));
	assert(dlist_empty(&domain->dgram.rx_pkt_list));
	received = pkt;
	received_addr = src_addr;
}

void test_pds_tx_done(struct suet_domain *domain, struct suet_pkt_entry *pkt,
		      int status)
{
	(void) domain;
	assert(!suet_dgram_pkt_in_use(pkt));
	completed = pkt;
	completed_status = status;
	if (dispatch_pds_tx)
		suet_pds_tx_done(domain, pkt, status);
}

static ssize_t mock_send(struct fid_ep *ep, const void *buf, size_t len,
			 void *desc, fi_addr_t dest, void *context)
{
	(void) ep;
	(void) desc;
	assert(dest == 9);
	assert(len >= prefix_size && len <= sizeof(sent));
	sent_len = len - prefix_size;
	memcpy(sent, (const char *) buf + prefix_size, sent_len);
	send_context = context;
	return send_status;
}

static ssize_t mock_sendv(struct fid_ep *ep, const struct iovec *iov,
			  void **desc, size_t count, fi_addr_t dest,
			  void *context)
{
	size_t i;

	mock_send(ep, iov[0].iov_base, iov[0].iov_len, desc[0], dest, context);
	for (i = 1; i < count; i++) {
		assert(sent_len + iov[i].iov_len <= sizeof(sent));
		memcpy(sent + sent_len, iov[i].iov_base, iov[i].iov_len);
		sent_len += iov[i].iov_len;
	}
	return send_status;
}

static ssize_t mock_recv(struct fid_ep *ep, void *buf, size_t len, void *desc,
			 fi_addr_t src, void *context)
{
	(void) ep;
	(void) desc;
	assert(src == FI_ADDR_UNSPEC && len == 256);
	recv_context = context;
	memset(buf, 0xa5, prefix_size);
	memcpy((char *) buf + prefix_size, "payload", 7);
	return 0;
}

static int mock_close(fid_t fid)
{
	struct suet_domain *domain = fid->context;

	/* The provider must stop before packet pools are destroyed. */
	assert(domain->dgram.tx_pkt_entry_pool);
	assert(domain->dgram.rx_pkt_entry_pool);
	if (close_status)
		return close_status;
	ep_closed = true;
	return 0;
}

static struct fi_ops_msg msg_ops = {
	.send = mock_send,
	.sendv = mock_sendv,
	.recv = mock_recv,
};
static struct fi_ops ep_ops = {.close = mock_close};

static void check_datagram(size_t prefix)
{
	struct suet_domain domain = {0};
	struct fid_ep ep = {.fid = {.ops = &ep_ops, .context = &domain},
			    .msg = &msg_ops};
	struct suet_pkt_entry *pkt;
	struct suet_pds_pkt_entry *pds;
	struct fi_cq_msg_entry comp = {0};
	struct iovec header;
	unsigned char payload[] = "payload";
	size_t i, j;

	prefix_size = prefix;
	domain.dgram.ep = &ep;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	domain.dgram.tx_prefix_size = prefix;
	domain.dgram.rx_prefix_size = prefix;
	dlist_init(&domain.dgram.rx_pkt_list);
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	pkt = suet_dgram_pkt_alloc(&domain);
	assert(pkt);
	pds = pkt->pds_ctx;
	memset(pds, 0, sizeof(*pds));
	pds->pkt = pkt;
	pkt->dgram_av_addr = 9;
	pds->psn = 123;
	assert(!((uintptr_t) pkt->pds_ctx % SUET_BUF_POOL_ALIGNMENT));
	assert(!((uintptr_t) pkt->ses_ctx % SUET_BUF_POOL_ALIGNMENT));
	assert((char *) pkt->ses_ctx >= (char *) pds + sizeof(*pds));
	assert((char *) pkt->pkt - prefix >=
	       (char *) pkt->ses_ctx + sizeof(struct suet_ses_pkt_entry));
	memset(pkt->ses_ctx, 0x5a, sizeof(struct suet_ses_pkt_entry));
	pkt->pkt_size = 11;
	memcpy(pkt->pkt, "headpayload", pkt->pkt_size);

	send_status = -FI_EAGAIN;
	assert(suet_dgram_send(&domain, pkt) == -FI_EAGAIN);
	assert(!suet_dgram_pkt_in_use(pkt));
	assert(pds->psn == 123);
	send_status = 0;
	assert(!suet_dgram_send(&domain, pkt));
	assert(suet_dgram_pkt_in_use(pkt));
	assert(sent_len == 11 && !memcmp(sent, "headpayload", 11));
	suet_dgram_tx_complete(&domain, send_context, 0);
	assert(completed == pkt && completed_status == 0);

	/* Repeated sends must not accumulate prefix bytes or change PDS timing.
	 */
	pkt->zc_pld_iov_count = 1;
	pkt->zc_pld_iov[0] = (struct iovec) {pkt->pkt, 4};
	pkt->zc_pld_iov[1] = (struct iovec) {payload, 7};
	header = pkt->zc_pld_iov[0];
	for (i = 0; i < 3; i++) {
		assert(!suet_dgram_send(&domain, pkt));
		assert(sent_len == 11 && !memcmp(sent, "headpayload", 11));
		assert(!memcmp(&header, &pkt->zc_pld_iov[0], sizeof(header)));
		assert(pkt->pkt_size == 11 && pds->psn == 123);
		for (j = 0; j < sizeof(struct suet_ses_pkt_entry); j++)
			assert(((unsigned char *) pkt->ses_ctx)[j] == 0x5a);
		suet_dgram_tx_complete(&domain, send_context, -FI_EIO);
		assert(completed == pkt && completed_status == -FI_EIO);
	}
	suet_dgram_pkt_free(pkt);

	received = NULL;
	assert(!suet_dgram_ep_recv_pkt(&domain));
	comp.op_context = recv_context;
	comp.len = prefix + 7;
	suet_dgram_rx_complete(&domain, &comp, 17);
	assert(received && received_addr == 17);
	assert(received->pkt_size == 7);
	assert(!memcmp(received->pkt, "payload", 7));
	/* PDS/SES can retain the RX allocation after the completion returns. */
	suet_dgram_pkt_free(received);

	if (prefix) {
		received = NULL;
		assert(!suet_dgram_ep_recv_pkt(&domain));
		comp.op_context = recv_context;
		comp.len = prefix - 1;
		suet_dgram_rx_complete(&domain, &comp, 17);
		assert(!received && dlist_empty(&domain.dgram.rx_pkt_list));
	}
	assert(!suet_dgram_ep_recv_pkt(&domain));
	ep_closed = false;
	close_status = -FI_EBUSY;
	assert(suet_dgram_cleanup(&domain) == -FI_EBUSY);
	assert(!ep_closed && domain.dgram.ep == &ep);
	assert(!dlist_empty(&domain.dgram.rx_pkt_list));
	assert(domain.dgram.tx_pkt_entry_pool &&
	       domain.dgram.rx_pkt_entry_pool);
	close_status = 0;
	assert(!suet_dgram_cleanup(&domain));
	assert(ep_closed && !domain.dgram.ep);
	assert(!domain.dgram.tx_pkt_entry_pool &&
	       !domain.dgram.rx_pkt_entry_pool);
	assert(!suet_dgram_cleanup(&domain));
}

/* A local completion releases the provider borrow, not PDS retention.
 * An ACK arriving while a send is pending must defer free until completion.
 */
static void check_pds_retention(void)
{
	struct suet_domain domain = {0};
	struct suet_ipdc ipdc = {0};
	struct fid_ep ep = {.msg = &msg_ops};
	struct suet_pkt_entry *pkt;
	struct suet_pds_pkt_entry *pds;

	prefix_size = 0;
	send_status = 0;
	domain.dgram.ep = &ep;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	assert(!suet_pds_init(&domain));
	ipdc.local_pdcid = 1;
	ipdc.in_flight_cnt = 1;
	dlist_init(&ipdc.in_flight_pkts);
	dlist_init(&ipdc.tx_list);
	assert(ofi_idm_set(&domain.pds.local_pdcid_to_ipdc_idm, 1, &ipdc) >= 0);
	pkt = suet_dgram_pkt_alloc(&domain);
	assert(pkt);
	pds = pkt->pds_ctx;
	memset(pds, 0, sizeof(*pds));
	pds->pkt = pkt;
	memset(pkt->pkt, 0, sizeof(struct suet_req_pkt));
	pds_prologue_set_type(pkt->pkt, PDS_ROD_REQ);
	pds->local_pdcid = 1;
	pkt->dgram_av_addr = 9;
	pkt->pkt_size = sizeof(struct suet_req_pkt);
	dlist_insert_tail(&pds->entry, &ipdc.in_flight_pkts);
	dispatch_pds_tx = true;
	assert(!suet_dgram_send(&domain, pkt));
	suet_dgram_tx_complete(&domain, send_context, 0);
	assert(!suet_dgram_pkt_in_use(pkt));
	assert(ipdc.in_flight_cnt == 1 && !dlist_empty(&ipdc.in_flight_pkts));
	assert(!suet_dgram_send(&domain, pkt));
	suet_dgram_tx_complete(&domain, send_context, -FI_EIO);
	assert(!suet_dgram_pkt_in_use(pkt));
	assert(ipdc.in_flight_cnt == 1 && !dlist_empty(&ipdc.in_flight_pkts));
	assert(!suet_dgram_send(&domain, pkt));
	pds->acked = true;
	suet_dgram_tx_complete(&domain, send_context, 0);
	assert(!ipdc.in_flight_cnt && dlist_empty(&ipdc.in_flight_pkts));
	dispatch_pds_tx = false;
	suet_pds_cleanup(&domain);
	suet_dgram_free_pkt_entry_pools(&domain);
}

/* Retained SES packets must not overwrite PDS state or datagram's RX node.
 * Exercise both first and continuation segments, then release through SES.
 */
static void check_ses_retention(void)
{
	struct suet_domain domain = {0};
	struct fid_ep dgram_ep = {.msg = &msg_ops};
	struct suet_ep ep = {0};
	struct suet_ep *ep_table[] = {&ep};
	struct suet_tpdc tpdc = {0};
	struct suet_pkt_entry *packets[2];
	struct suet_pds_pkt_entry saved[2];
	struct suet_ses_rx_dispatch_result result;
	struct fi_cq_msg_entry comp = {0};
	struct suet_ses_unexp_msg *unexp;
	void *rx;
	size_t i;

	prefix_size = 8;
	domain.dgram.ep = &dgram_ep;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.rx_prefix_size = prefix_size;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	domain.dgram.max_pkt_size = sizeof(struct suet_req_pkt) + 64;
	domain.ep_table = ep_table;
	ep.util_ep.domain = &domain.util_domain;
	dlist_init(&domain.dgram.rx_pkt_list);
	dlist_init(&ep.rx_list);
	dlist_init(&ep.unexp_list);
	dlist_init(&tpdc.gtd_del_list);
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	assert(!suet_ses_init(&domain));
	assert(!suet_pds_init(&domain));
	rx = suet_ses_rx_open(&domain, &tpdc, 0);
	assert(rx);
	for (i = 0; i < 2; i++) {
		struct suet_dgram_pkt_entry *dgram;
		struct suet_pds_pkt_entry *pds;
		struct suet_ses_pkt_entry *ses;
		struct suet_req_pkt *wire;
		struct suet_ses_rx_packet view;

		assert(!suet_dgram_ep_recv_pkt(&domain));
		comp.op_context = recv_context;
		comp.len = prefix_size + sizeof(*wire) + 64;
		suet_dgram_rx_complete(&domain, &comp, 17);
		packets[i] = received;
		dgram = container_of(received, struct suet_dgram_pkt_entry,
				     pkt);
		pds = received->pds_ctx;
		ses = received->ses_ctx;
		memset(pds, 0, sizeof(*pds));
		pds->pkt = received;
		pds->psn = 123 + i;
		pds->local_pdcid = 7;
		dlist_init(&pds->entry);
		memcpy(&saved[i], pds, sizeof(*pds));
		wire = received->pkt;
		memset(wire, 0, sizeof(*wire) + 64);
		ses_req_init(&wire->ses, UET_SEND, !i, !!i, 1, 0, 7, 0, 0, 0,
			     128, 0, 0);
		if (i)
			ses_hd_set_cont(&wire->ses, 64, 64);
		assert(suet_ses_rx_parse(
			&domain, UET_HDR_REQUEST_STD, &wire->ses,
			received->pkt_size - sizeof(wire->pds), &view));
		view.handle = pds;
		suet_ses_receive(rx, received->ses_ctx, &view, &result);
		assert(result.accepted && result.pkt_retained);
		assert(ses->pkt.handle == pds && ses->pkt.hdr == &wire->ses &&
		       !dlist_empty(&ses->entry));
		assert(dlist_empty(&dgram->entry));
		assert(dlist_empty(&pds->entry));
		suet_ses_rx_commit(rx, &result);
	}
	unexp = container_of(ep.unexp_list.next, struct suet_ses_unexp_msg,
			     entry);
	assert(unexp->pkt_list.next ==
	       &((struct suet_ses_pkt_entry *) packets[0]->ses_ctx)->entry);
	assert(unexp->pkt_list.prev ==
	       &((struct suet_ses_pkt_entry *) packets[1]->ses_ctx)->entry);
	for (i = 0; i < 2; i++)
		assert(!memcmp(packets[i]->pds_ctx, &saved[i],
			       sizeof(saved[i])));
	suet_ses_unexp_msg_list_cleanup(&ep.unexp_list);
	assert(dlist_empty(&ep.unexp_list));
	suet_ses_rx_close(rx);
	suet_ses_cleanup(&domain);
	suet_pds_cleanup(&domain);
	suet_dgram_free_pkt_entry_pools(&domain);
}

/* Exercise the real PDS assembler with separate SES headers/payloads, then
 * compare the emitted wire bytes on repeated sends with a provider prefix.
 */
static void check_pds_framing(bool zero_copy, bool atomic, size_t length)
{
	struct suet_domain domain = {0};
	struct fid_ep dgram_ep = {.msg = &msg_ops};
	struct suet_ep ep = {0};
	struct suet_ses_tx_entry ses = {0};
	struct suet_ipdc ipdc = {0};
	struct suet_pds_tx_entry tx = {0};
	unsigned char payload[141];
	size_t i, offset = 0;
	uint32_t segment = 0;

	prefix_size = 8;
	send_status = 0;
	domain.dgram.ep = &dgram_ep;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.max_pkt_size = sizeof(struct suet_req_pkt) + 64;
	domain.dgram.tx_prefix_size = prefix_size;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	domain.max_pkt_sz = 64;
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	ep.util_ep.domain = &domain.util_domain;
	ses.ep = &ep;
	ses.hdr_len = sizeof(struct ses_req_hdr) +
		      (atomic ? sizeof(struct ses_msg_amo_hdr) : 0);
	ses.num_pkts = length ? ofi_div_ceil(length, 64) : 1;
	ses.iov_count = 2;
	ses.iov[0] = (struct iovec) {payload, MIN(length, 23)};
	ses.iov[1] = (struct iovec) {payload + ses.iov[0].iov_len,
				     length - ses.iov[0].iov_len};
	ses.cq_entry.len = length;
	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (unsigned char) (i * 37 + 11);
	for (i = 0; i < ses.iov_count; i++)
		ses.zc_desc[i] = zero_copy ? &ses.iov[i] : NULL;
	ses_req_init(&ses.cached_hdr.ses, atomic ? UET_ATOMIC : UET_SEND, 1, 1,
		     1, 0, 17, 0, 0, 0, length, 0, 0);
	memset(&ses.cached_hdr.amo, 0x5a, sizeof(ses.cached_hdr.amo));
	ipdc.state = SUET_PDC_ESTABLISHED;
	ipdc.tx_seq_no = 100;
	assert(!suet_rel_tx_init(&ipdc.rel, 100, suet_env.max_unacked,
				 suet_env.max_pkt_retry));
	ipdc.tx_pkts = calloc(suet_env.max_unacked, sizeof(*ipdc.tx_pkts));
	assert(ipdc.tx_pkts);
	ipdc.last_tx_clear_psn = 99;
	ipdc.local_pdcid = 7;
	ipdc.tpdcid = 8;
	ipdc.dgram_av_addr = 9;
	dlist_init(&ipdc.in_flight_pkts);
	tx.domain = &domain;
	tx.ipdc = &ipdc;
	tx.context = &ses;
	suet_pds_tx_submit(&tx, ses.num_pkts);
	assert(tx.next_segment == ses.num_pkts);
	assert(ipdc.in_flight_cnt == ses.num_pkts);
	while (!dlist_empty(&ipdc.in_flight_pkts)) {
		struct suet_pds_pkt_entry *pds =
			container_of(ipdc.in_flight_pkts.next,
				     struct suet_pds_pkt_entry, entry);
		struct suet_pkt_entry *pkt = pds->pkt;
		struct suet_dgram_pkt_entry *dgram =
			container_of(pkt, struct suet_dgram_pkt_entry, pkt);
		const struct suet_req_pkt *wire = (const void *) sent;
		size_t len = MIN(length - offset, 64);
		size_t hdr_len = sizeof(struct pds_req_hdr) + ses.hdr_len;

		assert(pkt->zc_pld_iov_count ==
		       (zero_copy && len ?
				(segment == 0 && length > 23 ? 2 : 1) :
				0));
		suet_dgram_tx_complete(&domain, &dgram->context, 0);
		assert(!suet_dgram_send(&domain, pkt));
		assert(sent_len == hdr_len + len);
		assert(pds_prologue_get_type(&wire->pds) == PDS_ROD_REQ);
		assert(pds_prologue_get_next_hdr(&wire->pds) ==
		       UET_HDR_REQUEST_STD);
		assert(pds_req_get_psn(&wire->pds) == 100 + segment);
		assert(pds_req_get_spdcid(&wire->pds) == 7);
		assert(pds_req_get_dpdcid(&wire->pds) == 8);
		assert(pds_req_get_clear_psn(&wire->pds) == 99);
		assert(!!ses_req_ctrl_is_som(&wire->ses) == (segment == 0));
		assert(!!ses_req_ctrl_is_eom(&wire->ses) ==
		       (offset + len == length));
		assert(ses_get_message_id(&wire->ses) == 17);
		if (segment)
			assert(ses_hd_get_message_offset(&wire->ses) == offset);
		if (atomic)
			assert(!memcmp(sent + sizeof(*wire),
				       &ses.cached_hdr.amo,
				       sizeof(ses.cached_hdr.amo)));
		assert(!memcmp(sent + hdr_len, payload + offset, len));
		suet_dgram_tx_complete(&domain, &dgram->context, 0);
		dlist_remove_init(&pds->entry);
		suet_dgram_pkt_free(pkt);
		offset += len;
		segment++;
	}
	assert(offset == length && segment == ses.num_pkts);
	free(ipdc.tx_pkts);
	suet_rel_tx_cleanup(&ipdc.rel);
	suet_dgram_free_pkt_entry_pools(&domain);
}

/* Exercise the real ACK path while packets are still borrowed by datagram.
 * Retired bitmap slots must detach immediately; local completions retain and
 * release the old packet records independently of those slots.
 */
static void inject_cack(struct suet_domain *domain, uint16_t pdcid,
			uint32_t psn)
{
	struct suet_pkt_entry *pkt = suet_dgram_pkt_alloc(domain);
	struct pds_ack_hdr *ack = pkt->pkt;

	memset(ack, 0, sizeof(*ack));
	pds_ack_set_type(ack, PDS_ACK);
	pds_ack_set_next_hdr(ack, UET_HDR_NONE);
	pds_ack_set_dpdcid(ack, pdcid);
	pds_ack_set_spdcid(ack, 8);
	pds_ack_set_cack_psn(ack, psn);
	pkt->pkt_size = sizeof(*ack);
	suet_pds_receive(domain, pkt, 9);
}

static void check_bitmap_retirement(void)
{
	struct suet_domain domain = {0};
	struct fid_ep ep = {.msg = &msg_ops};
	struct suet_ipdc ipdc = {0};
	struct suet_pds_pkt_entry *packets[3];
	struct suet_dgram_pkt_entry *local[3];
	uint32_t i, slot;

	domain.dgram.ep = &ep;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	prefix_size = 0;
	send_status = 0;
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	assert(!suet_pds_init(&domain));
	assert(!suet_rel_tx_init(&ipdc.rel, UINT32_MAX - 1, 3, 100));
	ipdc.tx_pkts = calloc(3, sizeof(*ipdc.tx_pkts));
	assert(ipdc.tx_pkts);
	ipdc.local_pdcid = 7;
	ipdc.state = SUET_PDC_ESTABLISHED;
	ipdc.last_tx_clear_psn = UINT32_MAX - 2;
	dlist_init(&ipdc.tx_list);
	dlist_init(&ipdc.in_flight_pkts);
	assert(ofi_idm_set(&domain.pds.local_pdcid_to_ipdc_idm, 7, &ipdc) >= 0);
	dispatch_pds_tx = true;
	for (i = 0; i < 3; i++) {
		struct suet_pkt_entry *pkt = suet_dgram_pkt_alloc(&domain);
		struct suet_pds_pkt_entry *pds = pkt->pds_ctx;
		uint32_t psn = UINT32_MAX - 1 + i;

		memset(pds, 0, sizeof(*pds));
		pds->pkt = pkt;
		pds->psn = psn;
		pds->local_pdcid = 7;
		packets[i] = pds;
		memset(pkt->pkt, 0, sizeof(struct pds_req_hdr));
		pds_prologue_set_type(pkt->pkt, PDS_ROD_REQ);
		pds_req_set_psn(pkt->pkt, psn);
		pkt->pkt_size = sizeof(struct pds_req_hdr);
		pkt->dgram_av_addr = 9;
		assert(suet_rel_slot(&ipdc.rel.window, psn, &slot));
		ipdc.tx_pkts[slot] = pds;
		suet_rel_tx_track(&ipdc.rel, psn);
		suet_rel_tx_attempt(&ipdc.rel, psn,
				    suet_domain_now_ms(&domain));
		dlist_insert_tail(&pds->entry, &ipdc.in_flight_pkts);
		ipdc.in_flight_cnt++;
		assert(!suet_dgram_send(&domain, pkt));
		local[i] = container_of(pkt, struct suet_dgram_pkt_entry, pkt);
	}

	inject_cack(&domain, 7, UINT32_MAX - 1);
	assert(packets[0]->acked && !ipdc.tx_pkts[0]);
	assert(ipdc.in_flight_cnt == 3);
	assert(suet_rel_tx_can_track(&ipdc.rel, 1));
	inject_cack(&domain, 7, UINT32_MAX - 1); /* duplicate */
	assert(ipdc.in_flight_cnt == 3);
	inject_cack(&domain, 7, 1); /* covers a PSN never submitted */
	assert(suet_rel_tx_cack(&ipdc.rel) == UINT32_MAX - 1);
	assert(!packets[1]->acked && !packets[2]->acked);
	inject_cack(&domain, 7, 0);
	assert(!ipdc.tx_pkts[1] && !ipdc.tx_pkts[2]);
	assert(packets[1]->acked && packets[2]->acked);
	/* Complete in a different order from PSNs and ACKs. */
	suet_dgram_tx_complete(&domain, &local[2]->context, 0);
	suet_dgram_tx_complete(&domain, &local[0]->context, 0);
	suet_dgram_tx_complete(&domain, &local[1]->context, 0);
	assert(!ipdc.in_flight_cnt && dlist_empty(&ipdc.in_flight_pkts));
	dispatch_pds_tx = false;
	free(ipdc.tx_pkts);
	suet_rel_tx_cleanup(&ipdc.rel);
	suet_pds_cleanup(&domain);
	suet_dgram_free_pkt_entry_pools(&domain);
}

int main(void)
{
	ofi_mem_init();
	check_datagram(0);
	check_datagram(8);
	check_pds_retention();
	check_ses_retention();
	check_bitmap_retirement();
	check_pds_framing(false, false, 141);
	check_pds_framing(true, false, 141);
	check_pds_framing(false, false, 0);
	check_pds_framing(true, false, 0);
	check_pds_framing(false, true, 60);
	ofi_mem_fini();
	puts("PDS/datagram prefixes, completion ownership and shutdown: PASS");
	return 0;
}
