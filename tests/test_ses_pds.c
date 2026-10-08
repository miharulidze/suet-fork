/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* SES packet preparation must be independent of PDS sequencing and retries. */
#include <config.h>
/* Match libfabric's internal layouts, then enable the test assertions. */
#if !ENABLE_DEBUG && !defined(NDEBUG)
#define NDEBUG
#endif
#include "suet.h"
#include "suet_pds.h"
#include "suet_ses.h"
#undef NDEBUG
#include <assert.h>

static void check_segments(bool zero_copy)
{
	struct suet_domain domain = {0};
	struct suet_ep ep = {0};
	struct suet_ses_tx_entry entry = {0};
	struct suet_ses_tx_entry before;
	struct suet_pkt_entry packet = {0};
	unsigned char payload[141], buffer[256], copied[64];
	const uint32_t order[] = {2, 0, 1, 0, 2};
	struct suet_req_pkt *wire = (void *) buffer;
	size_t i, j, offset, len, done;

	domain.max_pkt_sz = 64;
	domain.dgram.tx_prefix_size = 8;
	ep.util_ep.domain = &domain.util_domain;
	entry.ep = &ep;
	entry.hdr_len = sizeof(*wire);
	entry.num_pkts = 3;
	entry.cq_entry.len = sizeof(payload);
	entry.iov_count = 3;
	entry.iov[0] = (struct iovec) {payload, 17};
	entry.iov[1] = (struct iovec) {payload + 17, 71};
	entry.iov[2] = (struct iovec) {payload + 88, 53};
	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (unsigned char) (i * 37 + 11);
	for (i = 0; i < entry.iov_count; i++)
		entry.zc_desc[i] = zero_copy ? &entry.iov[i] : NULL;
	ses_req_init(&entry.cached_hdr.ses, UET_SEND, 1, 1, 1, 1, 0x1234, 0,
		     0x1122334455667788ULL, 0xaabbccdd, sizeof(payload), 3, 7);
	packet.pkt = buffer;

	memcpy(&before, &entry, sizeof(entry));

	/* Repeat and reorder preparation: only the supplied segment may matter.
	 */
	for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		memset(buffer, 0xa5, sizeof(buffer));
		packet.zc_pld_iov_count = 0;
		suet_ses_prepare_tx(&entry, &packet, order[i]);
		offset = order[i] * 64;
		len = MIN(sizeof(payload) - offset, 64);
		for (j = 0; j < sizeof(wire->pds); j++)
			assert(buffer[j] == 0xa5);
		assert(packet.pkt_size == sizeof(*wire) + len);
		assert(!!ses_req_ctrl_is_som(&wire->ses) == (order[i] == 0));
		assert(!!ses_req_ctrl_is_eom(&wire->ses) == (order[i] == 2));
		assert(ses_get_message_id(&wire->ses) == 0x1234);
		assert(ses_get_ri(&wire->ses) == 7);
		if (order[i]) {
			assert(ses_hd_get_message_offset(&wire->ses) == offset);
		} else {
			assert(ses_get_request_length(&wire->ses) ==
			       sizeof(payload));
			assert(ses_get_match_bits(&wire->ses) ==
			       0x1122334455667788ULL);
			assert(ses_hd_get_completion_data(&wire->ses) ==
			       0xaabbccdd);
		}
		if (zero_copy) {
			done = 0;
			for (j = 1; j <= packet.zc_pld_iov_count; j++) {
				assert(done + packet.zc_pld_iov[j].iov_len <=
				       len);
				memcpy(copied + done,
				       packet.zc_pld_iov[j].iov_base,
				       packet.zc_pld_iov[j].iov_len);
				done += packet.zc_pld_iov[j].iov_len;
				assert(packet.zc_pld_desc[j]);
			}
			assert(done == len);
			assert(!memcmp(copied, payload + offset, len));
		} else {
			assert(!memcmp(wire->msg, payload + offset, len));
		}
	}
	assert(!memcmp(&entry, &before, sizeof(entry)));
}

static void check_empty_and_atomic(void)
{
	struct suet_domain domain = {0};
	struct suet_ep ep = {0};
	struct suet_ses_tx_entry entry = {0};
	struct suet_pkt_entry packet = {0};
	unsigned char buffer[256];
	uint64_t operand = 0x123456789abcdef0ULL;
	struct suet_amo_pkt *wire = (void *) buffer;

	domain.max_pkt_sz = 64;
	ep.util_ep.domain = &domain.util_domain;
	entry.ep = &ep;
	entry.hdr_len = sizeof(struct suet_req_pkt);
	entry.iov_count = 1;
	entry.iov[0] = (struct iovec) {&operand, 0};
	packet.pkt = buffer;
	ses_req_init(&entry.cached_hdr.ses, UET_SEND, 1, 1, 1, 0, 1, 0, 0, 0, 0,
		     3, 7);
	suet_ses_prepare_tx(&entry, &packet, 0);
	assert(packet.pkt_size == sizeof(struct suet_req_pkt));
	assert(ses_req_ctrl_is_som(&wire->ses));
	assert(ses_req_ctrl_is_eom(&wire->ses));

	entry.hdr_len = sizeof(*wire);
	entry.cq_entry.len = sizeof(operand);
	entry.iov[0].iov_len = sizeof(operand);
	memset(&entry.cached_hdr.amo, 0x5a, sizeof(entry.cached_hdr.amo));
	ses_req_init(&entry.cached_hdr.ses, UET_ATOMIC, 1, 1, 1, 0, 2, 0x1000,
		     9, 0, sizeof(operand), 3, 7);
	suet_ses_prepare_tx(&entry, &packet, 0);
	assert(packet.pkt_size == sizeof(*wire) + sizeof(operand));
	assert(!memcmp(&wire->amo, &entry.cached_hdr.amo, sizeof(wire->amo)));
	assert(!memcmp(wire->msg, &operand, sizeof(operand)));
}

static void check_ses_domain_lifecycle(void)
{
	struct suet_domain domains[2] = {0};
	struct suet_ep ep = {0};
	struct suet_ses_rx_entry *entry;
	struct iovec iov = {0};
	void *rx[2];
	size_t i;

	for (i = 0; i < 2; i++) {
		assert(!suet_ses_init(&domains[i]));
		assert(domains[i].ses.tx_entry_pool->attr.context ==
		       &domains[i].ses);
		assert(domains[i].ses.rx_entry_pool->attr.context ==
		       &domains[i].ses);
		rx[i] = suet_ses_rx_open(&domains[i], NULL, 0);
		assert(rx[i]);
		assert(!suet_ses_rx_busy(rx[i]));
	}

	suet_ses_rx_close(rx[0]);
	suet_ses_cleanup(&domains[0]);

	/* Closing one domain must leave the other domain's SES usable. */
	ep.util_ep.domain = &domains[1].util_domain;
	entry = suet_ses_rx_entry_init(&ep, &iov, 1, 0, 0, NULL,
				       SUET_ADDR_INVALID, ofi_op_msg, 0);
	assert(entry);
	assert(entry->ep == &ep);
	suet_ses_rx_entry_free(entry);
	suet_ses_rx_close(rx[1]);
	suet_ses_cleanup(&domains[1]);

	/* Cleanup may be repeated, and SES can be initialized again. */
	suet_ses_cleanup(&domains[0]);
	assert(!suet_ses_init(&domains[0]));
	rx[0] = suet_ses_rx_open(&domains[0], NULL, 0);
	assert(rx[0]);
	suet_ses_rx_close(rx[0]);
	suet_ses_cleanup(&domains[0]);
}

static void check_pds_domain_lifecycle(void)
{
	struct suet_domain domains[2] = {0};
	void *tx;

	/* Cleanup also accepts a zeroed or only partially initialized domain.
	 */
	suet_pds_cleanup(&domains[0]);
	assert(!ofi_bufpool_create(&domains[0].pds.ipdc_pool,
				   sizeof(struct suet_ipdc),
				   SUET_BUF_POOL_ALIGNMENT, 0, 1, 0));
	suet_pds_cleanup(&domains[0]);
	assert(!domains[0].pds.ipdc_pool);
	assert(!suet_pds_init(&domains[0]));
	assert(!suet_pds_init(&domains[1]));
	assert(ofi_idm_set(&domains[0].pds.local_pdcid_to_ipdc_idm, 7,
			   &domains[0]) >= 0);
	domains[1].pds.next_pdcid = 42;
	suet_pds_cleanup(&domains[0]);
	suet_pds_cleanup(&domains[0]);
	assert(dlist_empty(&domains[0].pds.active_ipdc_list));
	assert(dlist_empty(&domains[0].pds.active_tpdc_list));
	assert(domains[1].pds.next_pdcid == 42);
	tx = ofi_buf_alloc(domains[1].pds.pds_tx_pool);
	assert(tx);
	ofi_buf_free(tx);
	assert(!suet_pds_init(&domains[0]));
	assert(domains[0].pds.next_pdcid == 1);
	suet_pds_cleanup(&domains[0]);
	suet_pds_cleanup(&domains[1]);
}

static void check_response_retention(void)
{
	struct suet_domain domain = {0};
	struct suet_tpdc tpdc = {0};
	struct suet_ep ep = {0};
	struct suet_ep *ep_table[] = {&ep};
	struct ofi_bufpool *tx_pool;
	struct suet_pkt_entry packet = {0};
	struct suet_req_pkt request = {0};
	struct suet_ses_rx_dispatch_result result;
	struct suet_ses_resp placeholder = {
		.ses_opcode = UET_NO_RESPONSE,
		.ses_rc = RC_OK,
		.list = UET_EXPECTED,
		.message_id = 7,
		.modified_length = 141,
	};
	struct suet_ses_resp reply = {
		.ses_opcode = UET_RESPONSE,
		.ses_rc = RC_OK,
		.list = UET_OVERFLOW,
		.message_id = 7,
		.modified_length = 141,
	};
	struct suet_ses_resp before;
	struct suet_pds_ses_resp_entry *saved;
	void *held_packet, *rx;

	assert(!suet_ses_init(&domain));
	assert(!suet_pds_init(&domain));
	/* Use a one-slot chunk so pool exhaustion is deterministic. */
	ofi_bufpool_destroy(domain.pds.gtd_del_resp_pool);
	assert(!ofi_bufpool_create(&domain.pds.gtd_del_resp_pool,
				   sizeof(struct suet_pds_ses_resp_entry),
				   SUET_BUF_POOL_ALIGNMENT, 1, 1, 0));
	dlist_init(&tpdc.gtd_del_list);
	tpdc.expected_rx_psn = UINT32_MAX - 1;
	memcpy(&before, &placeholder, sizeof(before));
	saved = suet_pds_response_reserve(&domain, &tpdc, &placeholder, 3);
	assert(saved);
	assert(saved->psn == UINT32_MAX - 1 && saved->num_pkts == 3);
	assert(!memcmp(&placeholder, &before, sizeof(before)));
	assert(!suet_pds_response_reserve(&domain, &tpdc, &placeholder, 3));

	/* A full response pool must reject unexpected intake without retaining
	 * its packet or advancing receive state. SES returns no wire NACK code.
	 */
	domain.max_pkt_sz = 64;
	domain.ep_table = ep_table;
	ep.util_ep.domain = &domain.util_domain;
	dlist_init(&ep.rx_list);
	dlist_init(&ep.unexp_list);
	rx = suet_ses_rx_open(&domain, &tpdc, 0);
	assert(rx);
	ses_req_init(&request.ses, UET_SEND, 1, 0, 1, 0, 7, 0, 0, 0, 141, 0, 0);
	packet.pkt = &request;
	memset(&result, 0xa5, sizeof(result));
	suet_ses_receive(rx, &packet, &result);
	assert(result.status == -FI_ENOMEM);
	assert(!result.accepted && !result.pkt_retained && !result.completion);
	assert(dlist_empty(&ep.unexp_list));
	assert(tpdc.expected_rx_psn == UINT32_MAX - 1);
	suet_ses_rx_close(rx);

	/* Exhaust packet storage to test response replacement without sending.
	 * The saved replay range must survive replacement of semantic data.
	 */
	assert(!ofi_bufpool_create(&tx_pool, sizeof(struct suet_pkt_entry),
				   SUET_BUF_POOL_ALIGNMENT, 1, 1, 0));
	domain.dgram.tx_pkt_entry_pool = tx_pool;
	held_packet = ofi_buf_alloc(tx_pool);
	assert(held_packet);
	tpdc.expected_rx_psn = 1;
	memcpy(&before, &reply, sizeof(before));
	suet_pds_response_complete(saved, &reply);
	assert(!memcmp(&reply, &before, sizeof(before)));
	assert(saved->resp.ses_opcode == reply.ses_opcode);
	assert(saved->resp.ses_rc == reply.ses_rc);
	assert(saved->resp.list == reply.list);
	assert(saved->resp.message_id == reply.message_id);
	assert(saved->resp.modified_length == reply.modified_length);
	assert(saved->psn == UINT32_MAX - 1 && saved->num_pkts == 3);
	assert(!saved->reserved);
	suet_pds_response_cancel(saved);
	assert(dlist_empty(&tpdc.gtd_del_list));

	/* Releasing a retained response makes capacity available again. */
	saved = suet_pds_response_reserve(&domain, &tpdc, &placeholder, 1);
	assert(saved && saved->psn == 1 && saved->num_pkts == 1);
	suet_pds_response_cancel(saved);
	ofi_buf_free(held_packet);
	ofi_bufpool_destroy(tx_pool);
	suet_pds_cleanup(&domain);
	suet_ses_cleanup(&domain);
}

int main(void)
{
	check_segments(false);
	check_segments(true);
	check_empty_and_atomic();
	puts("SES/PDS packet preparation: PASS");
	/* Pool tests run without fi_getinfo, which normally initializes this.
	 */
	ofi_mem_init();
	check_ses_domain_lifecycle();
	check_pds_domain_lifecycle();
	check_response_retention();
	ofi_mem_fini();
	puts("SES/PDS domain lifecycle and response retention: PASS");
	return 0;
}
