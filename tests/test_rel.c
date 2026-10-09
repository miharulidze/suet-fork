/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* This test links only reliability: no PDC, SES, wire headers or libfabric. */
#include "suet_rel.h"

#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* PSN-oriented test helpers independently exercise slot lookup. */
static bool can_track(struct suet_rel_tx *tx, uint32_t psn)
{
	uint32_t slot;
	return suet_rel_tx_can_track(tx, psn, &slot);
}
static void track(struct suet_rel_tx *tx, uint32_t psn)
{
	uint32_t slot;
	assert(suet_rel_tx_can_track(tx, psn, &slot));
	suet_rel_tx_track(tx, slot);
}
static void attempt(struct suet_rel_tx *tx, uint32_t psn, uint64_t now)
{
	uint32_t slot;
	assert(suet_rel_slot(&tx->window, psn, &slot));
	suet_rel_tx_attempt(tx, slot, now);
}
static bool retry_next(struct suet_rel_tx *tx, uint64_t now,
		       uint32_t *cursor, uint32_t *psn)
{
	uint32_t slot, expected;
	if (!suet_rel_tx_retry_next(tx, now, cursor, psn, &slot))
		return false;
	assert(suet_rel_slot(&tx->window, *psn, &expected));
	assert(slot == expected);
	return true;
}

static void check_windows(uint32_t capacity, uint32_t base)
{
	struct suet_rel_tx tx;
	struct suet_rel_retired retired;
	uint32_t i, round, psn = base, slot, previous_slot;

	assert(!suet_rel_tx_init(&tx, base, capacity, 100, SUET_REL_GBN));
	for (round = 0; round < 4 * capacity; round++) {
		for (i = 0; i < capacity; i++) {
			assert(can_track(&tx, psn + i));
			track(&tx, psn + i);
			attempt(&tx, psn + i, 0);
			assert(!can_track(&tx, psn + i));
		}
		assert(!can_track(&tx, psn + capacity));
		assert(suet_rel_tx_ack(&tx, psn + capacity, &retired) ==
		       SUET_REL_ACK_INVALID);
		assert(suet_rel_tx_ack(&tx, psn - 1, &retired) ==
		       SUET_REL_ACK_STALE);
		assert(suet_rel_slot(&tx.window, psn, &previous_slot));
		assert(suet_rel_tx_ack(&tx, psn, &retired) ==
		       SUET_REL_ACK_ADVANCED);
		assert(retired.first_psn == psn &&
		       retired.first_slot == previous_slot);
		assert(retired.count == 1 && suet_rel_tx_cack(&tx) == psn);
		if (capacity > 1) {
			assert(suet_rel_slot(&tx.window, psn + 1, &slot));
			assert(slot == (previous_slot + 1) % capacity);
			assert(suet_rel_tx_ack(&tx, psn + capacity - 1,
					       &retired) ==
			       SUET_REL_ACK_ADVANCED);
			assert(retired.count == capacity - 1);
		}
		assert(suet_rel_tx_ack(&tx, psn + capacity - 1, &retired) ==
		       SUET_REL_ACK_STALE);
		psn += capacity;
	}
	suet_rel_tx_cleanup(&tx);
}

static void check_hole_and_retry(void)
{
	struct suet_rel_tx tx;
	struct suet_rel_retired retired;
	uint32_t cursor, psn, retry;
	uint64_t now = 0, delay;

	assert(!suet_rel_tx_init(&tx, 100, 4, 100, SUET_REL_GBN));
	track(&tx, 100);
	attempt(&tx, 100, 0);
	track(&tx, 102);
	attempt(&tx, 102, 0);
	assert(suet_rel_tx_ack(&tx, 102, &retired) == SUET_REL_ACK_INVALID);
	assert(suet_rel_tx_cack(&tx) == 99);
	track(&tx, 101);
	attempt(&tx, 101, 0);
	assert(!suet_rel_tx_nack(&tx, 103, 10, 0));
	cursor = 0;
	assert(!retry_next(&tx, 0, &cursor, &psn));
	/* A whole GBN pass increments the shared retry round only once. The
	 * original retry budget includes failed local submission attempts.
	 */
	for (retry = 0; retry <= 100; retry++) {
		delay = retry >= 12 ? 4000 : UINT64_C(1) << retry;
		cursor = 0;
		assert(!retry_next(&tx, now + delay - 1, &cursor,
					       &psn));
		now += delay;
		cursor = 0;
		for (uint32_t expected = 100; expected <= 102; expected++) {
			assert(retry_next(&tx, now, &cursor, &psn));
			assert(psn == expected);
			attempt(&tx, psn, now);
		}
		assert(!retry_next(&tx, now, &cursor, &psn));
		assert(tx.retry_count == retry);
		suet_rel_tx_retry_end(&tx, true);
		assert(suet_rel_tx_failed(&tx) == (retry == 100));
	}
	assert(suet_rel_tx_ack(&tx, 102, &retired) == SUET_REL_ACK_ADVANCED);
	assert(!suet_rel_tx_failed(&tx) && tx.retry_count == 0);
	suet_rel_tx_cleanup(&tx);
}

static void check_rx(void)
{
	struct suet_rel_rx rx;
	uint32_t base = UINT32_MAX - 1;

	assert(!suet_rel_rx_init(&rx, base, 3, SUET_REL_GBN));
	assert(suet_rel_rx_record(&rx, base + 1, false) == SUET_REL_RX_GAP);
	assert(suet_rel_rx_cack(&rx) == base - 1);
	assert(suet_rel_rx_record(&rx, base, false) == SUET_REL_RX_NEW);
	/* Arrival alone never advances acceptance. A rejected SES request can
	 * be offered again, without becoming a false duplicate.
	 */
	assert(suet_rel_rx_cack(&rx) == base - 1);
	suet_rel_rx_cancel(&rx, base);
	assert(suet_rel_rx_record(&rx, base, true) == SUET_REL_RX_NEW);
	suet_rel_rx_commit(&rx, base);
	assert(suet_rel_rx_cack(&rx) == base);
	assert(suet_rel_rx_ack_needed(&rx, base, false));
	suet_rel_rx_ack_sent(&rx);
	assert(!suet_rel_rx_ack_needed(&rx, base, false));
	assert(suet_rel_rx_ack_needed(&rx, base, true));
	assert(suet_rel_rx_record(&rx, base, false) == SUET_REL_RX_DROP);
	assert(suet_rel_rx_record(&rx, base, true) == SUET_REL_RX_REPLAY);
	for (uint32_t i = 1; i < 12; i++) {
		assert(suet_rel_rx_record(&rx, base + i, false) ==
		       SUET_REL_RX_NEW);
		suet_rel_rx_commit(&rx, base + i);
		assert(suet_rel_rx_cack(&rx) == base + i);
	}
	suet_rel_rx_reset(&rx, 44);
	assert(suet_rel_rx_cack(&rx) == 43);
	assert(suet_rel_rx_record(&rx, 44, false) == SUET_REL_RX_NEW);
	suet_rel_rx_cleanup(&rx);
}

static void check_ack_ranges(uint32_t capacity)
{
	struct suet_rel_tx tx;
	struct suet_rel_retired retired;
	uint32_t base = UINT32_MAX - 100, hole, i, count;

	assert(!suet_rel_tx_init(&tx, base, capacity, 100, SUET_REL_GBN));
	/* Every missing bit position, at every physical head, including partial
	 * words and PSN wrap. An invalid CACK must preserve the whole window.
	 */
	for (uint32_t head = 0; head < capacity; head++) {
		for (hole = 0; hole < capacity; hole++) {
			for (i = 0; i < capacity; i++)
				if (i != hole)
					track(&tx, base + i);
			assert(suet_rel_tx_ack(&tx, base + capacity - 1,
					       &retired) == SUET_REL_ACK_INVALID);
			assert(tx.window.base_psn == base);
			assert(tx.tracked == capacity - 1 && !retired.count);
			for (i = 0; i < capacity; i++)
				assert(can_track(&tx, base + i) == (i == hole));
			track(&tx, base + hole);
			assert(suet_rel_tx_ack(&tx, base + capacity - 1,
					       &retired) == SUET_REL_ACK_ADVANCED);
			assert(retired.count == capacity && !tx.tracked);
			base += capacity;
		}
		count = 1;
		track(&tx, base);
		assert(suet_rel_tx_ack(&tx, base, &retired) ==
		       SUET_REL_ACK_ADVANCED);
		base += count;
	}
	suet_rel_tx_cleanup(&tx);
}

static void check_selective_repeat(void)
{
	struct suet_rel_tx tx;
	struct suet_rel_rx rx;
	struct suet_rel_retired retired;
	uint32_t base = UINT32_MAX - 31, cursor, psn;
	uint64_t sack;

	assert(!suet_rel_tx_init(&tx, base, 128, 10, SUET_REL_SR));
	assert(!suet_rel_rx_init(&rx, base, 128, SUET_REL_SR));
	for (uint32_t i = 0; i < 128; i++) {
		track(&tx, base + i);
		attempt(&tx, base + i, 0);
	}
	/* Deliver in reverse order, with a hole at zero. Receipt cannot imply
	 * CACK advancement or semantic success across that hole.
	 */
	for (uint32_t i = 127; i; i--) {
		assert(suet_rel_rx_record(&rx, base + i, false) ==
		       SUET_REL_RX_NEW);
		assert(suet_rel_rx_cack(&rx) == base - 1);
		suet_rel_rx_commit(&rx, base + i);
		assert(suet_rel_rx_record(&rx, base + i, true) ==
		       SUET_REL_RX_REPLAY);
		assert(suet_rel_rx_record(&rx, base + i, false) ==
		       SUET_REL_RX_DROP);
	}
	sack = suet_rel_rx_sack(&rx, base);
	assert(sack == (UINT64_MAX ^ 1));
	assert(suet_rel_tx_sack(&tx, base, sack) == 63);
	assert(suet_rel_tx_sack(&tx, base, 0) == 0);
	assert(suet_rel_tx_sack(&tx, base, sack) == 0);
	assert(suet_rel_tx_sack(&tx, base + 64,
				suet_rel_rx_sack(&rx, base + 64)) == 64);
	assert(suet_rel_tx_cack(&tx) == base - 1 && tx.tracked == 128);
	cursor = 0;
	assert(retry_next(&tx, 1, &cursor, &psn) && psn == base);
	assert(!retry_next(&tx, 1, &cursor, &psn));
	/* Refused semantic dispatch must not appear in SACK. */
	assert(suet_rel_rx_record(&rx, base, true) == SUET_REL_RX_NEW);
	suet_rel_rx_cancel(&rx, base);
	assert(suet_rel_rx_sack(&rx, base) == sack);
	assert(suet_rel_rx_record(&rx, base, true) == SUET_REL_RX_NEW);
	suet_rel_rx_commit(&rx, base);
	assert(suet_rel_rx_cack(&rx) == base + 127);
	assert(suet_rel_tx_ack(&tx, base + 127, &retired) ==
	       SUET_REL_ACK_ADVANCED);
	assert(retired.count == 128 && retired.newly_acked == 1);
	assert(suet_rel_tx_sack(&tx, base, UINT64_MAX) == 0);
	assert(suet_rel_tx_sack(&tx, base + 128, UINT64_MAX) == 0);
	/* An unexpired earlier packet must not hide an expired later packet. */
	base += 128;
	track(&tx, base);
	track(&tx, base + 1);
	attempt(&tx, base, 10);
	attempt(&tx, base + 1, 0);
	cursor = 0;
	assert(retry_next(&tx, 10, &cursor, &psn) && psn == base + 1);
	assert(!retry_next(&tx, 10, &cursor, &psn));
	suet_rel_tx_cleanup(&tx);
	suet_rel_rx_cleanup(&rx);
}

static void check_nack(void)
{
	for (enum suet_rel_algorithm algo = SUET_REL_GBN; algo <= SUET_REL_SR;
	     algo++) {
		struct suet_rel_tx tx;
		uint32_t cursor = 0, psn;
		assert(!suet_rel_tx_init(&tx, 100, 3, 3, algo));
		track(&tx, 100);
		track(&tx, 101);
		attempt(&tx, 100, 10);
		attempt(&tx, 101, 10);
		assert(!suet_rel_tx_nack(&tx, 99, 10, 0));
		assert(!suet_rel_tx_nack(&tx, 102, 10, 0));
		assert(suet_rel_tx_nack(&tx, 101, 10, 0));
		assert(!suet_rel_tx_nack(&tx, 101, 10, 0));
		assert(retry_next(&tx, 10, &cursor, &psn) && psn == 101);
		attempt(&tx, psn, 10);
		suet_rel_tx_retry_end(&tx, true);
		assert(tx.retry_count == 1 && tx.rto_round == 0);
		cursor = 0;
		assert(!retry_next(&tx, 10, &cursor, &psn));
		assert(suet_rel_tx_sack(&tx, 101, 1) == 1);
		assert(!suet_rel_tx_nack(&tx, 101, 10, 0));
		suet_rel_tx_cleanup(&tx);
	}
}

static void check_retry_budget_per_packet(void)
{
	struct suet_rel_tx tx;
	uint32_t cursor, psn;
	assert(!suet_rel_tx_init(&tx, 100, 128, 2, SUET_REL_SR));
	for (uint32_t i = 0; i < 128; i++) {
		track(&tx, 100 + i);
		attempt(&tx, 100 + i, 0);
		assert(suet_rel_tx_nack(&tx, 100 + i, 0, 0));
		cursor = i;
		assert(retry_next(&tx, 0, &cursor, &psn) && psn == 100 + i);
		attempt(&tx, psn, 0);
		suet_rel_tx_retry_end(&tx, true);
		assert(!suet_rel_tx_failed(&tx));
	}
	for (int i = 0; i < 2; i++) {
		assert(suet_rel_tx_nack(&tx, 100, 0, 1));
		cursor = 0;
		assert(!retry_next(&tx, 0, &cursor, &psn));
		cursor = 0;
		assert(retry_next(&tx, 1, &cursor, &psn) && psn == 100);
		attempt(&tx, psn, 1);
		suet_rel_tx_retry_end(&tx, true);
	}
	assert(suet_rel_tx_failed(&tx));
	suet_rel_tx_cleanup(&tx);
}

static void check_pending_ack(uint32_t base)
{
	struct suet_rel_rx rx;
	const uint32_t order[] = {2, 5, 4, 3, 0, 1};
	uint32_t psn;
	size_t i;

	assert(!suet_rel_rx_init(&rx, base, 128, SUET_REL_SR));
	for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
		psn = base + order[i];
		assert(suet_rel_rx_record(&rx, psn, false) == SUET_REL_RX_NEW);
		suet_rel_rx_commit(&rx, psn);
		if (i == 0 || i == 2 || i == 3)
			suet_rel_rx_request_ack(&rx, psn);
		assert(rx.ack_pending);
		assert(rx.ack_target_psn == base + (i < 2 ? 2 : 4));
		if (i == 1) {
			/* Traffic above the requested PSN does not need extra ACKs. */
			assert(!suet_rel_rx_ack_needed(&rx, psn, false));
			continue;
		}
		assert(suet_rel_rx_ack_needed(&rx, psn, false));
		if (i != 5) {
			suet_rel_rx_ack_sent(&rx);
			assert(rx.ack_pending); /* Interim SACK keeps the target. */
		}
	}
	assert(suet_rel_rx_cack(&rx) == base + 5);
	/* A failed local send never calls ack_sent. Retry even on later data. */
	assert(suet_rel_rx_ack_needed(&rx, base + 6, false));
	assert(rx.since_ack);
	suet_rel_rx_ack_sent(&rx);
	assert(!rx.ack_pending && !rx.since_ack);
	assert(!suet_rel_rx_ack_needed(&rx, base + 6, false));
	/* A new, already cumulative request is answered once. */
	suet_rel_rx_request_ack(&rx, base + 3);
	assert(suet_rel_rx_ack_needed(&rx, base + 3, false));
	suet_rel_rx_ack_sent(&rx);
	assert(!rx.ack_pending);
	suet_rel_rx_request_ack(&rx, base + 10);
	suet_rel_rx_reset(&rx, 70);
	assert(!rx.ack_pending && !rx.since_ack);
	assert(!suet_rel_rx_ack_needed(&rx, 70, false));
	suet_rel_rx_cleanup(&rx);
}

int main(void)
{
	struct suet_rel_tx tx;
	assert(suet_rel_tx_init(&tx, 0, 0, 0, SUET_REL_GBN) < 0);
	assert(suet_rel_tx_init(&tx, 0, UINT32_C(0x80000000), 0, SUET_REL_GBN) <
	       0);
	check_windows(1, UINT32_MAX);
	check_windows(3, UINT32_MAX - 2);
	check_windows(65, UINT32_MAX - 32);
	check_windows(128, 100);
	check_ack_ranges(65);
	check_ack_ranges(129);
	check_hole_and_retry();
	check_rx();
	check_pending_ack(100);
	check_pending_ack(UINT32_MAX - 2);
	check_selective_repeat();
	check_nack();
	check_retry_budget_per_packet();
	puts("Reliability windows, wraparound, SACK, GBN/SR and NACK recovery: "
	     "PASS");
	return 0;
}
