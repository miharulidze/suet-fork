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

static void check_segments(bool zero_copy, size_t headroom)
{
	struct suet_domain domain = {0};
	struct suet_ep ep = {0};
	struct suet_ses_tx_entry entry = {0}, before;
	struct iovec iov[SUET_IOV_LIMIT];
	void *desc[SUET_IOV_LIMIT];
	struct suet_ses_tx_segment segment = {
		.iov = iov,
		.desc = desc,
		.iov_capacity = SUET_IOV_LIMIT,
	};
	unsigned char payload[141], buffer[256], copied[64];
	const uint32_t order[] = {2, 0, 1, 0, 2};
	struct ses_req_hdr *hdr = (void *) (buffer + headroom);
	size_t i, j, offset, len, done;

	domain.max_pkt_sz = 64;
	ep.util_ep.domain = &domain.util_domain;
	entry.ep = &ep;
	entry.hdr_len = sizeof(*hdr);
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
	memcpy(&before, &entry, sizeof(entry));

	/* Arbitrary headroom is invisible to SES; payload stays separate.
	 * Repeated/reordered preparation must not mutate the operation.
	 */
	for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		memset(buffer, 0xa5, sizeof(buffer));
		assert(!suet_ses_prepare_tx(&entry, order[i], hdr, sizeof(*hdr),
					    &segment));
		offset = order[i] * 64;
		len = MIN(sizeof(payload) - offset, 64);
		for (j = 0; j < headroom; j++)
			assert(buffer[j] == 0xa5);
		for (j = headroom + sizeof(*hdr); j < sizeof(buffer); j++)
			assert(buffer[j] == 0xa5);
		assert(segment.hdr_len == sizeof(*hdr));
		assert(segment.hdr_type == UET_HDR_REQUEST_STD);
		assert(segment.payload_len == len);
		assert(segment.zero_copy == zero_copy);
		assert(!!ses_req_ctrl_is_som(hdr) == (order[i] == 0));
		assert(!!ses_req_ctrl_is_eom(hdr) == (order[i] == 2));
		assert(ses_get_message_id(hdr) == 0x1234);
		assert(ses_get_ri(hdr) == 7);
		if (order[i]) {
			assert(ses_hd_get_message_offset(hdr) == offset);
		} else {
			assert(ses_get_request_length(hdr) == sizeof(payload));
			assert(ses_get_match_bits(hdr) ==
			       0x1122334455667788ULL);
			assert(ses_hd_get_completion_data(hdr) == 0xaabbccdd);
		}
		done = 0;
		for (j = 0; j < segment.iov_count; j++) {
			assert(done + iov[j].iov_len <= len);
			memcpy(copied + done, iov[j].iov_base, iov[j].iov_len);
			done += iov[j].iov_len;
			assert(!!desc[j] == zero_copy);
		}
		assert(done == len && !memcmp(copied, payload + offset, len));
	}
	assert(!memcmp(&entry, &before, sizeof(entry)));
	memset(buffer, 0xa5, sizeof(buffer));
	assert(suet_ses_prepare_tx(&entry, 0, hdr, sizeof(*hdr) - 1,
				   &segment) == -FI_ETOOSMALL);
	segment.iov_capacity = 2;
	assert(suet_ses_prepare_tx(&entry, 0, hdr, sizeof(*hdr), &segment) ==
	       -FI_ETOOSMALL);
	assert(suet_ses_prepare_tx(&entry, 3, hdr, sizeof(*hdr), &segment) ==
	       -FI_EINVAL);
	for (i = 0; i < sizeof(buffer); i++)
		assert(buffer[i] == 0xa5);
}

static void check_empty_and_atomic(void)
{
	struct suet_domain domain = {0};
	struct suet_ep ep = {0};
	struct suet_ses_tx_entry entry = {0};
	struct iovec iov[SUET_IOV_LIMIT];
	void *desc[SUET_IOV_LIMIT];
	struct suet_ses_tx_segment segment = {
		.iov = iov,
		.desc = desc,
		.iov_capacity = SUET_IOV_LIMIT,
	};
	unsigned char buffer[256];
	uint64_t operand = 0x123456789abcdef0ULL;
	struct ses_req_hdr *hdr = (void *) buffer;

	domain.max_pkt_sz = 64;
	ep.util_ep.domain = &domain.util_domain;
	entry.ep = &ep;
	entry.hdr_len = sizeof(*hdr);
	entry.num_pkts = 1;
	entry.iov_count = 1;
	entry.iov[0] = (struct iovec) {&operand, 0};
	ses_req_init(&entry.cached_hdr.ses, UET_SEND, 1, 1, 1, 0, 1, 0, 0, 0, 0,
		     3, 7);
	assert(!suet_ses_prepare_tx(&entry, 0, hdr, sizeof(buffer), &segment));
	assert(segment.hdr_len == sizeof(*hdr));
	assert(!segment.payload_len && !segment.iov_count);
	assert(ses_req_ctrl_is_som(hdr) && ses_req_ctrl_is_eom(hdr));

	entry.hdr_len += sizeof(struct ses_msg_amo_hdr);
	entry.cq_entry.len = sizeof(operand);
	entry.iov[0].iov_len = sizeof(operand);
	memset(&entry.cached_hdr.amo, 0x5a, sizeof(entry.cached_hdr.amo));
	ses_req_init(&entry.cached_hdr.ses, UET_ATOMIC, 1, 1, 1, 0, 2, 0x1000,
		     9, 0, sizeof(operand), 3, 7);
	assert(!suet_ses_prepare_tx(&entry, 0, hdr, sizeof(buffer), &segment));
	assert(segment.hdr_len == entry.hdr_len);
	assert(segment.payload_len == sizeof(operand));
	assert(!memcmp(hdr + 1, &entry.cached_hdr.amo,
		       sizeof(entry.cached_hdr.amo)));
	assert(segment.iov_count == 1 && iov[0].iov_base == &operand);
}

static void check_rx_views(void)
{
	struct suet_domain domain = {0};
	struct suet_ep ep = {0};
	struct suet_ep *ep_table[] = {&ep};
	unsigned char buffer[128];
	struct ses_req_hdr *hdr = (void *) (buffer + 37);
	struct suet_ses_rx_packet pkt = {0};
	size_t hdr_len = sizeof(*hdr);

	domain.ep_table = ep_table;
	memset(buffer, 0, sizeof(buffer));
	ses_req_init(hdr, UET_SEND, 1, 1, 1, 0, 7, 0, 0, 0, 8, 0, 0);
	assert(!suet_ses_rx_parse(&domain, UET_HDR_REQUEST_STD, hdr,
				  hdr_len - 1, &pkt));
	assert(!suet_ses_rx_parse(&domain, UET_HDR_NONE, hdr, hdr_len, &pkt));
	assert(suet_ses_rx_parse(&domain, UET_HDR_REQUEST_STD, hdr, hdr_len + 8,
				 &pkt));
	assert(pkt.hdr == hdr && pkt.payload == hdr + 1 &&
	       pkt.payload_len == 8);
	assert(suet_ses_rx_parse(&domain, UET_HDR_REQUEST_STD, hdr, hdr_len,
				 &pkt));
	assert(!pkt.payload_len);

	ses_req_init(hdr, UET_ATOMIC, 1, 1, 1, 0, 7, 0, 0, 0, 8, 0, 0);
	hdr_len += sizeof(struct ses_msg_amo_hdr);
	assert(!suet_ses_rx_parse(&domain, UET_HDR_REQUEST_STD, hdr,
				  hdr_len - 1, &pkt));
	assert(!suet_ses_rx_parse(&domain, UET_HDR_REQUEST_STD, hdr,
				  hdr_len + 7, &pkt));
	assert(suet_ses_rx_parse(&domain, UET_HDR_REQUEST_STD, hdr, hdr_len + 8,
				 &pkt));
	assert(pkt.payload == (char *) hdr + hdr_len && pkt.payload_len == 8);
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
		domains[i].dgram.max_pkt_size = 256;
		assert(!suet_ses_init(&domains[i]));
		assert(domains[i].max_pkt_sz + sizeof(struct ses_req_hdr) ==
		       suet_pds_max_ses_size(&domains[i]));
		assert(domains[i].max_inline_atom + sizeof(struct ses_req_hdr) +
			       sizeof(struct ses_msg_amo_hdr) ==
		       suet_pds_max_ses_size(&domains[i]));
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

/* The semantic header and payload need not be adjacent in an SES view. */
static void check_rx_payload_view(void)
{
	struct suet_domain domain = {0};
	struct suet_ep ep = {0};
	struct suet_ep *ep_table[] = {&ep};
	struct ses_req_hdr hdr;
	unsigned char payload[17], output[17] = {0};
	struct iovec iov = {output, sizeof(output)};
	struct suet_ses_rx_packet pkt = {
		.hdr = &hdr,
		.payload = payload,
		.payload_len = sizeof(payload),
	};
	struct suet_ses_rx_dispatch_result result;
	struct suet_ses_rx_entry *entry;
	void *rx;

	domain.dgram.max_pkt_size = 256;
	domain.ep_table = ep_table;
	ep.util_ep.domain = &domain.util_domain;
	dlist_init(&ep.rx_list);
	assert(!suet_ses_init(&domain));
	rx = suet_ses_rx_open(&domain, NULL, 0);
	assert(rx);
	entry = suet_ses_rx_entry_init(&ep, &iov, 1, 0, 0, NULL,
				       SUET_ADDR_INVALID, ofi_op_msg, 0);
	assert(entry);
	dlist_insert_tail(&entry->entry, &ep.rx_list);
	memset(payload, 0x5a, sizeof(payload));
	ses_req_init(&hdr, UET_SEND, 1, 1, 1, 0, 7, 0, 0, 0, sizeof(payload), 0,
		     0);
	suet_ses_receive(rx, NULL, &pkt, &result);
	assert(result.accepted && !result.pkt_retained);
	assert(result.completion == entry);
	assert(entry->bytes_copied == sizeof(output));
	assert(!memcmp(payload, output, sizeof(output)));
	/* Inspect dispatch before commit writes the application CQ. */
	suet_ses_rx_entry_free(entry);
	suet_ses_rx_close(rx);
	suet_ses_cleanup(&domain);
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
	struct suet_ses_rx_packet packet = {0};
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

	domain.dgram.max_pkt_size = 64 + sizeof(struct suet_req_pkt);
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
	struct suet_pds_pkt_entry request_ctx = {.psn = tpdc.expected_rx_psn};
	saved = suet_pds_response_reserve(&domain, &tpdc, &placeholder, 3,
					  &request_ctx);
	assert(saved);
	assert(saved->psn == UINT32_MAX - 1 && saved->num_pkts == 3);
	assert(!memcmp(&placeholder, &before, sizeof(before)));
	assert(!suet_pds_response_reserve(&domain, &tpdc, &placeholder, 3,
					  &request_ctx));

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
	packet.hdr = &request.ses;
	packet.handle = &request_ctx;
	memset(&result, 0xa5, sizeof(result));
	suet_ses_receive(rx, NULL, &packet, &result);
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
	request_ctx.psn = 1;
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
	saved = suet_pds_response_reserve(&domain, &tpdc, &placeholder, 1,
					  &request_ctx);
	assert(saved && saved->psn == 1 && saved->num_pkts == 1);
	suet_pds_response_cancel(saved);
	ofi_buf_free(held_packet);
	ofi_bufpool_destroy(tx_pool);
	suet_pds_cleanup(&domain);
	suet_ses_cleanup(&domain);
}

int main(void)
{
	check_segments(false, 0);
	check_segments(true, 0);
	check_segments(false, 37);
	check_segments(true, 61);
	check_rx_views();
	check_empty_and_atomic();
	puts("SES/PDS packet preparation: PASS");
	/* Pool tests run without fi_getinfo, which normally initializes this.
	 */
	ofi_mem_init();
	check_ses_domain_lifecycle();
	check_rx_payload_view();
	check_pds_domain_lifecycle();
	check_response_retention();
	ofi_mem_fini();
	puts("SES/PDS domain lifecycle and response retention: PASS");
	return 0;
}
