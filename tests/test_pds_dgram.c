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
static bool sent_ev;
static uint16_t last_ev;

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

static ssize_t mock_sendmsg(struct fid_ep *ep, const struct fi_msg *msg,
			    uint64_t flags)
{
	assert(!flags && !msg->data);
	sent_ev = false;
	return mock_sendv(ep, msg->msg_iov, msg->desc, msg->iov_count,
			  msg->addr, msg->context);
}

static ssize_t mock_sendmsg_ev(struct fid_ep *ep, const struct fi_msg_ev *msg,
			       uint64_t flags)
{
	ssize_t ret = mock_sendmsg(ep, &msg->msg, flags);

	sent_ev = true;
	last_ev = msg->ev;
	return ret;
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
	.size = sizeof(struct fi_ops_msg),
	.sendmsg = mock_sendmsg,
	.sendmsg_ev = mock_sendmsg_ev,
	.send = mock_send,
	.sendv = mock_sendv,
	.recv = mock_recv,
};
static struct fi_ops ep_ops = {.close = mock_close};

static void check_datagram(size_t prefix, bool ev)
{
	struct suet_domain domain = {0};
	struct fid_ep ep = {.fid = {.ops = &ep_ops, .context = &domain},
			    .msg = &msg_ops};
	struct suet_pkt_entry *pkt;
	struct suet_pds_pkt_entry *pds;
	struct fi_cq_data_entry comp = {0};
	struct iovec header;
	unsigned char payload[] = "payload";
	size_t i, j;

	prefix_size = prefix;
	domain.dgram.sendmsg_ev = ev;
	domain.dgram.rx_metadata = ev ? FI_SUET_DGRAM_EV : 0;
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
	pkt->ev = 65535;
	memcpy(pkt->pkt, "headpayload", pkt->pkt_size);

	send_status = -FI_EAGAIN;
	assert(suet_dgram_send(&domain, pkt) == -FI_EAGAIN);
	assert(sent_ev == ev && (!ev || last_ev == 65535));
	assert(!suet_dgram_pkt_in_use(pkt));
	assert(pds->psn == 123);
	send_status = 0;
	assert(!suet_dgram_send(&domain, pkt));
	assert(suet_dgram_pkt_in_use(pkt));
	assert(sent_len == 11 && !memcmp(sent, "headpayload", 11));
	assert(sent_ev == ev && (!ev || last_ev == 65535));
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
		assert(sent_ev == ev && (!ev || last_ev == 65535));
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
	comp.flags = FI_SUET_DGRAM_EV;
	comp.data = 54321;
	suet_dgram_rx_complete(&domain, &comp, 17);
	assert(received && received_addr == 17);
	assert(received->pkt_size == 7);
	assert(received->ev == (ev ? 54321 : 0));
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
	assert(!dlist_empty(&ipdc.in_flight_pkts));
	assert(!suet_dgram_send(&domain, pkt));
	suet_dgram_tx_complete(&domain, send_context, -FI_EIO);
	assert(!suet_dgram_pkt_in_use(pkt));
	assert(!dlist_empty(&ipdc.in_flight_pkts));
	assert(!suet_dgram_send(&domain, pkt));
	pds->acked = true;
	suet_dgram_tx_complete(&domain, send_context, 0);
	assert(dlist_empty(&ipdc.in_flight_pkts));
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
	struct suet_pkt_entry *packets[4];
	struct suet_pds_pkt_entry saved[4];
	struct suet_ses_rx_dispatch_result result;
	struct fi_cq_data_entry comp = {0};
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
	for (i = 0; i < 4; i++) {
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
		ses_req_init(&wire->ses, UET_SEND, !(i % 2), !!(i % 2), 1, 0, 7, 0, 0, 0,
			     128, 0, 0);
		if (i % 2)
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
	/* The sender can reuse an ID after all packets are ACKed while the
	 * older complete unexpected message is still waiting for a receive.
	 */
	unexp = container_of(unexp->entry.next, struct suet_ses_unexp_msg, entry);
	assert(unexp->pkt_list.next ==
	       &((struct suet_ses_pkt_entry *) packets[2]->ses_ctx)->entry);
	assert(unexp->pkt_list.prev ==
	       &((struct suet_ses_pkt_entry *) packets[3]->ses_ctx)->entry);
	for (i = 0; i < 4; i++)
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
	suet_cc_init(&ipdc.cc, suet_env.max_unacked);
	suet_lb_init(&ipdc.lb, 65535, 7);
	assert(!suet_rel_tx_init(&ipdc.rel, 100, suet_env.max_unacked,
				 suet_env.max_pkt_retry, SUET_REL_GBN));
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
	/* Congestion admission must block even with packet/bitmap capacity.
	 * Use synthetic outstanding credit here; real ACK accounting is
	 * exercised below by check_bitmap_retirement.
	 */
	for (i = 0; i < (size_t) suet_env.max_unacked; i++)
		suet_cc_track(&ipdc.cc);
	suet_pds_tx_submit(&tx, ses.num_pkts);
	assert(!tx.started && !tx.next_segment);
	assert(dlist_empty(&ipdc.in_flight_pkts));
	suet_cc_ack(&ipdc.cc, suet_env.max_unacked);
	suet_pds_tx_submit(&tx, ses.num_pkts);
	assert(tx.next_segment == ses.num_pkts);
	assert(ipdc.cc.in_flight == ses.num_pkts);
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

		assert(pkt->ev == (uint16_t) (65535 + segment));
		assert(pkt->zc_pld_iov_count ==
		       (zero_copy && len ?
				(segment == 0 && length > 23 ? 2 : 1) :
				0));
		suet_dgram_tx_complete(&domain, &dgram->context, 0);
		assert(!suet_dgram_send(&domain, pkt));
		assert(ipdc.cc.in_flight == ses.num_pkts);
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

static void inject_sack(struct suet_domain *domain, uint16_t id, uint64_t bits,
			bool short_header)
{
	struct suet_pkt_entry *pkt = suet_dgram_pkt_alloc(domain);
	/* Golden ACK_CC bytes: type 8, CACK 0xfffffffe, SACK base 0xfffffff8,
	 * signed offset -6; bitmap bit 7 acknowledges PSN 0xffffffff.
	 */
	const unsigned char wire[32] = {
		0x40, 0x20, 0, 0, 0xff, 0xff, 0xff, 0xfe,
		0,    8,    0, 7, 0,	0,    0xff, 0xfa,
	};
	memcpy(pkt->pkt, wire, sizeof(wire));
	struct pds_ack_cc_hdr *ack = pkt->pkt;
	pds_ack_set_dpdcid(&ack->ack, id);
	ack->sack_bitmap = htonll(bits);
	pkt->pkt_size = short_header ? 31 : sizeof(wire);
	suet_pds_receive(domain, pkt, 9);
}

static void check_bitmap_retirement(void)
{
	struct suet_domain domain = {0};
	struct fid_ep ep = {.msg = &msg_ops};
	struct suet_ipdc ipdc = {0};
	struct suet_ep ses_ep = {0};
	struct suet_ses_tx_entry ses = {.ep = &ses_ep,
					.num_pkts = 1,
					.hdr_len = sizeof(struct ses_req_hdr)};
	struct suet_pds_tx_entry tx = {.domain = &domain,
				       .ipdc = &ipdc,
				       .context = &ses};
	struct suet_pds_pkt_entry *packets[3];
	struct suet_dgram_pkt_entry *local[3];
	struct suet_dgram_pkt_entry *new_local;
	uint32_t i, slot;
	int max_unacked = suet_env.max_unacked;

	suet_env.max_unacked = 3;
	domain.dgram.ep = &ep;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.max_pkt_size = sizeof(struct suet_req_pkt) + 64;
	domain.max_pkt_sz = 64;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	prefix_size = 0;
	send_status = 0;
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	assert(!suet_pds_init(&domain));
	assert(!suet_rel_tx_init(&ipdc.rel, UINT32_MAX - 1, 3, 100,
				 SUET_REL_GBN));
	suet_cc_init(&ipdc.cc, 3);
	suet_lb_init(&ipdc.lb, 7, 1);
	ipdc.tx_pkts = calloc(3, sizeof(*ipdc.tx_pkts));
	assert(ipdc.tx_pkts);
	ipdc.local_pdcid = 7;
	ipdc.tpdcid = 8;
	ipdc.dgram_av_addr = 9;
	ipdc.tx_seq_no = 1;
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
		suet_rel_tx_track(&ipdc.rel, slot);
		suet_cc_track(&ipdc.cc);
		suet_rel_tx_attempt(&ipdc.rel, slot,
				    suet_domain_now_ms(&domain));
		dlist_insert_tail(&pds->entry, &ipdc.in_flight_pkts);
		assert(!suet_dgram_send(&domain, pkt));
		local[i] = container_of(pkt, struct suet_dgram_pkt_entry, pkt);
	}

	/* Local completion alone must not open the congestion window. */
	suet_dgram_tx_complete(&domain, &local[2]->context, 0);
	assert(ipdc.cc.in_flight == 3 && !suet_cc_can_send(&ipdc.cc));
	assert(!suet_dgram_send(&domain, packets[2]->pkt));
	assert(ipdc.cc.in_flight == 3);
	inject_cack(&domain, 7, UINT32_MAX - 1);
	assert(packets[0]->acked && !ipdc.tx_pkts[0]);
	assert(ipdc.cc.in_flight == 2 && suet_cc_can_send(&ipdc.cc));
	assert(suet_rel_tx_can_track(&ipdc.rel, 1, &slot));
	/* Reuse the ACKed slot while its old buffer is still borrowed. */
	ses_ep.util_ep.domain = &domain.util_domain;
	ses_req_init(&ses.cached_hdr.ses, UET_SEND, 1, 1, 1, 0, 17, 0, 0, 0, 0,
		     0, 0);
	suet_pds_tx_submit(&tx, 1);
	assert(tx.started && tx.next_segment == 1);
	assert(ipdc.cc.in_flight == 3 && !suet_cc_can_send(&ipdc.cc));
	assert(ipdc.tx_pkts[0] != packets[0] && ipdc.tx_pkts[0]->psn == 1);
	assert(suet_dgram_pkt_in_use(packets[0]->pkt));
	new_local = container_of(ipdc.tx_pkts[0]->pkt,
				 struct suet_dgram_pkt_entry, pkt);
	inject_cack(&domain, 7, UINT32_MAX - 1); /* duplicate */
	assert(ipdc.cc.in_flight == 3);
	inject_cack(&domain, 7, 2); /* covers a PSN never submitted */
	assert(suet_rel_tx_cack(&ipdc.rel) == UINT32_MAX - 1);
	assert(!packets[1]->acked && !packets[2]->acked);
	assert(ipdc.cc.in_flight == 3);
	/* A stale CACK may carry new SACK evidence. Keep packet pointers and
	 * semantic completion state, and return congestion credit only once.
	 */
	inject_sack(&domain, 7, UINT64_C(1) << 7, true);
	assert(ipdc.cc.in_flight == 3);
	inject_sack(&domain, 7, UINT64_C(1) << 7, false);
	assert(ipdc.cc.in_flight == 2 && ipdc.tx_pkts[1] == packets[1]);
	assert(ipdc.cc.ecn_events == 1);
	inject_sack(&domain, 7, UINT64_C(1) << 7, false);
	inject_sack(&domain, 7, 0, false);
	assert(ipdc.cc.ecn_events == 1);
	assert(ipdc.cc.in_flight == 2);
	inject_cack(&domain, 7, 0);
	assert(!ipdc.tx_pkts[1] && !ipdc.tx_pkts[2]);
	assert(packets[1]->acked && packets[2]->acked);
	assert(ipdc.cc.in_flight == 1);
	inject_cack(&domain, 7, 1);
	assert(!ipdc.tx_pkts[0]);
	assert(!ipdc.cc.in_flight && ipdc.cc.cwnd == 3);
	/* Complete in a different order from PSNs and ACKs. */
	suet_dgram_tx_complete(&domain, &local[2]->context, 0);
	suet_dgram_tx_complete(&domain, &local[0]->context, 0);
	suet_dgram_tx_complete(&domain, &new_local->context, 0);
	suet_dgram_tx_complete(&domain, &local[1]->context, 0);
	assert(dlist_empty(&ipdc.in_flight_pkts));
	assert(!ipdc.cc.in_flight && ipdc.cc.cwnd == 3);
	dispatch_pds_tx = false;
	free(ipdc.tx_pkts);
	suet_rel_tx_cleanup(&ipdc.rel);
	suet_pds_cleanup(&domain);
	suet_dgram_free_pkt_entry_pools(&domain);
	suet_env.max_unacked = max_unacked;
}

static void check_trimmed_request(void)
{
	struct suet_domain domain = {0};
	struct fid_ep ep = {.msg = &msg_ops};
	domain.dgram.ep = &ep;
	domain.dgram.sendmsg_ev = true;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	prefix_size = 0;
	send_status = 0;
	dispatch_pds_tx = true;
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	for (unsigned syn = 0; syn < 2; syn++) {
		for (unsigned last = 0; last < 2; last++) {
			struct suet_pkt_entry *pkt =
				suet_dgram_pkt_alloc(&domain);
			struct pds_req_hdr *req = pkt->pkt;
			memset(req, 0, sizeof(*req));
			pds_prologue_set_type(req, PDS_RUD_REQ);
			pds_prologue_set_flags(req, syn ? PDS_FLAG_SYN : 0);
			pds_req_set_psn(req, 101);
			pds_req_set_spdcid(req, 7);
			pds_req_set_dpdcid(req, 8);
			pkt->pkt_size =
				sizeof(*req); /* no SES header/payload */
			pkt->ev = 54321;
			pkt->rx_flags =
				SUET_DGRAM_RX_TRIMMED |
				(last ? SUET_DGRAM_RX_TRIMMED_LASTHOP : 0);
			suet_pds_receive(&domain, pkt, 9);
			struct pds_nack_hdr *nack = (void *) sent;
			assert(sent_len == sizeof(*nack));
			assert(sent_ev && last_ev == 54321);
			assert(pds_nack_get_code(nack) == (last ? 2 : 1));
			assert(pds_nack_get_psn(nack) == 101);
			assert(pds_nack_get_spdcid(nack) == (syn ? 0 : 8));
			assert(pds_nack_get_dpdcid(nack) == 7);
			assert(!domain.pds.tpdc_by_syn_key_ht);
			suet_dgram_tx_complete(&domain, send_context, 0);
		}
	}
	dispatch_pds_tx = false;
	suet_dgram_free_pkt_entry_pools(&domain);
}

/* A late SES completion must echo its own request, not the most recent EV
 * observed on this PDC. The response record remains PDS-owned.
 */
static void check_deferred_response_ev(void)
{
	struct suet_domain domain = {0};
	struct fid_ep ep = {.msg = &msg_ops};
	struct suet_tpdc tpdc = {.dgram_av_addr = 9,
				 .local_pdcid = 8,
				 .ipdcid = 7,
				 .ack_ev = 12345};
	struct suet_pds_pkt_entry req = {.psn = 100};
	struct suet_ses_resp resp = {.ses_opcode = UET_RESPONSE,
				     .message_id = 17};
	struct suet_pds_ses_resp_entry *saved;
	struct suet_pkt_entry *pkt;
	struct suet_req_pkt *wire;

	domain.dgram.ep = &ep;
	domain.dgram.sendmsg_ev = true;
	domain.dgram.max_mtu_sz = 256;
	domain.dgram.pds_pkt_size = sizeof(struct suet_pds_pkt_entry);
	domain.dgram.ses_pkt_size = sizeof(struct suet_ses_pkt_entry);
	prefix_size = 0;
	send_status = 0;
	dispatch_pds_tx = true;
	assert(!suet_dgram_init_pkt_entry_pools(&domain));
	assert(!suet_pds_init(&domain));
	assert(!suet_rel_rx_init(&tpdc.rel, 100, 128, SUET_REL_GBN));
	dlist_init(&tpdc.gtd_del_list);
	saved = suet_pds_response_reserve(&domain, &tpdc, &resp, 1, &req);
	assert(saved && saved->ev == 12345);
	tpdc.ack_ev = 54321;
	suet_pds_response_complete(saved, &resp);
	assert(sent_ev && last_ev == 12345);
	assert(pds_prologue_get_type((const void *) sent) == PDS_ACK);
	suet_dgram_tx_complete(&domain, send_context, 0);
	/* A duplicate request on a different EV must replay the saved semantic
	 * response on that duplicate's EV, rather than the saved original EV.
	 */
	domain.max_pkt_sz = 64;
	tpdc.type = SUET_PDC_ROD;
	assert(ofi_idm_set(&domain.pds.local_pdcid_to_tpdc_idm, 8, &tpdc) >= 0);
	assert(suet_rel_rx_record(&tpdc.rel, 100, false) == SUET_REL_RX_NEW);
	suet_rel_rx_commit(&tpdc.rel, 100);
	tpdc.expected_rx_psn = 101;
	pkt = suet_dgram_pkt_alloc(&domain);
	assert(pkt);
	wire = pkt->pkt;
	memset(wire, 0, sizeof(*wire));
	pds_prologue_set_type(&wire->pds, PDS_ROD_REQ);
	pds_prologue_set_next_hdr(&wire->pds, UET_HDR_REQUEST_STD);
	pds_prologue_set_flags(&wire->pds, PDS_FLAG_RETX);
	pds_req_set_psn(&wire->pds, 100);
	pds_req_set_spdcid(&wire->pds, 7);
	pds_req_set_dpdcid(&wire->pds, 8);
	ses_req_init(&wire->ses, UET_SEND, 1, 1, 1, 0, 17, 0, 0, 0, 0, 0, 0);
	pkt->pkt_size = sizeof(*wire);
	pkt->ev = 43210;
	sent_ev = false;
	suet_pds_receive(&domain, pkt, 9);
	assert(sent_ev && last_ev == 43210);
	assert(pds_prologue_get_type((const void *) sent) == PDS_ACK);
	suet_dgram_tx_complete(&domain, send_context, 0);
	ofi_idm_clear(&domain.pds.local_pdcid_to_tpdc_idm, 8);
	suet_pds_response_cancel(saved);
	suet_rel_rx_cleanup(&tpdc.rel);
	suet_pds_cleanup(&domain);
	suet_dgram_free_pkt_entry_pools(&domain);
	dispatch_pds_tx = false;
}

int main(void)
{
	ofi_mem_init();
	check_datagram(0, false);
	check_datagram(8, false);
	check_datagram(0, true);
	check_datagram(8, true);
	check_pds_retention();
	check_ses_retention();
	check_bitmap_retirement();
	check_trimmed_request();
	check_deferred_response_ev();
	check_pds_framing(false, false, 141);
	check_pds_framing(true, false, 141);
	check_pds_framing(false, false, 0);
	check_pds_framing(true, false, 0);
	check_pds_framing(false, true, 60);
	ofi_mem_fini();
	puts("PDS/datagram prefixes, completion ownership and shutdown: PASS");
	return 0;
}
