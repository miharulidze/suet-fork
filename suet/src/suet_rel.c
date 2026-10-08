/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Go-Back-N implementation of the PDC-independent reliability contract. */
#include "suet_rel.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool suet_rel_bit(const uint64_t *bits, uint32_t slot)
{
	return (bits[slot / 64] >> (slot % 64)) & 1;
}

static void suet_rel_set(uint64_t *bits, uint32_t slot)
{
	bits[slot / 64] |= UINT64_C(1) << (slot % 64);
}

static void suet_rel_clear(uint64_t *bits, uint32_t slot)
{
	bits[slot / 64] &= ~(UINT64_C(1) << (slot % 64));
}

static size_t suet_rel_words(uint32_t capacity)
{
	return ((size_t) capacity + 63) / 64;
}

static int suet_rel_window_init(struct suet_rel_window *window,
				uint32_t base_psn, uint32_t capacity)
{
	/* Serial-number comparisons require a window below half the PSN space.
	 */
	if (!capacity || capacity >= UINT32_C(0x80000000))
		return -EINVAL;
	memset(window, 0, sizeof(*window));
	window->present = calloc(suet_rel_words(capacity), sizeof(uint64_t));
	if (!window->present)
		return -ENOMEM;
	window->base_psn = base_psn;
	window->capacity = capacity;
	return 0;
}

bool suet_rel_slot(const struct suet_rel_window *window, uint32_t psn,
		   uint32_t *slot)
{
	uint32_t offset = psn - window->base_psn;

	if (offset >= window->capacity)
		return false;
	*slot = (window->head + offset) % window->capacity;
	return true;
}

static void suet_rel_advance(struct suet_rel_window *window, uint32_t count)
{
	window->base_psn += count;
	window->head = (window->head + count) % window->capacity;
}

int suet_rel_tx_init(struct suet_rel_tx *tx, uint32_t base_psn,
		     uint32_t capacity, uint32_t max_retries)
{
	int ret;

	memset(tx, 0, sizeof(*tx));
	ret = suet_rel_window_init(&tx->window, base_psn, capacity);
	if (ret)
		return ret;
	tx->attempt_time = calloc(capacity, sizeof(*tx->attempt_time));
	if (!tx->attempt_time) {
		suet_rel_tx_cleanup(tx);
		return -ENOMEM;
	}
	tx->max_retries = max_retries;
	return 0;
}

void suet_rel_tx_cleanup(struct suet_rel_tx *tx)
{
	free(tx->attempt_time);
	free(tx->window.present);
	memset(tx, 0, sizeof(*tx));
}

bool suet_rel_tx_can_track(const struct suet_rel_tx *tx, uint32_t psn)
{
	uint32_t slot;

	return suet_rel_slot(&tx->window, psn, &slot) &&
	       !suet_rel_bit(tx->window.present, slot);
}

void suet_rel_tx_track(struct suet_rel_tx *tx, uint32_t psn)
{
	uint32_t slot;
	bool valid = suet_rel_slot(&tx->window, psn, &slot);

	assert(valid && !suet_rel_bit(tx->window.present, slot));
	if (valid) {
		suet_rel_set(tx->window.present, slot);
		tx->tracked++;
	}
}

void suet_rel_tx_attempt(struct suet_rel_tx *tx, uint32_t psn, uint64_t now)
{
	uint32_t slot;
	bool valid = suet_rel_slot(&tx->window, psn, &slot);

	assert(valid && suet_rel_bit(tx->window.present, slot));
	if (valid)
		tx->attempt_time[slot] = now;
}

enum suet_rel_ack_result suet_rel_tx_ack(struct suet_rel_tx *tx, uint32_t cack,
					 struct suet_rel_retired *retired)
{
	struct suet_rel_window *window = &tx->window;
	uint32_t count = cack - window->base_psn + 1;
	uint32_t i, slot;

	memset(retired, 0, sizeof(*retired));
	if ((int32_t) (cack - window->base_psn) < 0)
		return SUET_REL_ACK_STALE;
	if (count > window->capacity)
		return SUET_REL_ACK_INVALID;
	/* A cumulative ACK must not cover an unsubmitted hole. */
	for (i = 0; i < count; i++) {
		slot = (window->head + i) % window->capacity;
		if (!suet_rel_bit(window->present, slot))
			return SUET_REL_ACK_INVALID;
	}
	retired->first_psn = window->base_psn;
	retired->first_slot = window->head;
	retired->count = count;
	for (i = 0; i < count; i++)
		suet_rel_clear(window->present,
			       (window->head + i) % window->capacity);
	tx->tracked -= count;
	suet_rel_advance(window, count);
	tx->retry_count = 0;
	return SUET_REL_ACK_ADVANCED;
}

uint32_t suet_rel_tx_cack(const struct suet_rel_tx *tx)
{
	return tx->window.base_psn - 1;
}

bool suet_rel_tx_nack(struct suet_rel_tx *tx, uint32_t psn)
{
	uint32_t slot;

	return suet_rel_slot(&tx->window, psn, &slot) &&
	       suet_rel_bit(tx->window.present, slot);
}

bool suet_rel_tx_retry_next(const struct suet_rel_tx *tx, uint64_t now,
			    uint32_t *cursor, uint32_t *psn)
{
	const struct suet_rel_window *window = &tx->window;
	uint64_t timeout =
		tx->retry_count >= 12 ? 4000 : UINT64_C(1) << tx->retry_count;
	uint32_t offset, slot;

	if (!tx->tracked)
		return false;
	while (*cursor < window->capacity) {
		offset = (*cursor)++;
		slot = (window->head + offset) % window->capacity;
		if (!suet_rel_bit(window->present, slot))
			continue;
		if (now < tx->attempt_time[slot] ||
		    now - tx->attempt_time[slot] < timeout)
			return false;
		*psn = window->base_psn + offset;
		return true;
	}
	return false;
}

void suet_rel_tx_retry_end(struct suet_rel_tx *tx, bool attempted)
{
	if (attempted && tx->retry_count != UINT32_MAX)
		tx->retry_count++;
}

bool suet_rel_tx_failed(const struct suet_rel_tx *tx)
{
	return tx->retry_count > tx->max_retries;
}

int suet_rel_rx_init(struct suet_rel_rx *rx, uint32_t base_psn,
		     uint32_t capacity)
{
	int ret;

	memset(rx, 0, sizeof(*rx));
	ret = suet_rel_window_init(&rx->window, base_psn, capacity);
	if (ret)
		return ret;
	rx->accepted = calloc(suet_rel_words(capacity), sizeof(uint64_t));
	if (!rx->accepted) {
		suet_rel_rx_cleanup(rx);
		return -ENOMEM;
	}
	return 0;
}

void suet_rel_rx_cleanup(struct suet_rel_rx *rx)
{
	free(rx->accepted);
	free(rx->window.present);
	memset(rx, 0, sizeof(*rx));
}

enum suet_rel_rx_result suet_rel_rx_record(struct suet_rel_rx *rx, uint32_t psn,
					   bool retransmit)
{
	uint32_t slot;

	if ((int32_t) (psn - rx->window.base_psn) < 0)
		return retransmit ? SUET_REL_RX_REPLAY : SUET_REL_RX_DROP;
	/* GBN discards packets beyond the first gap regardless of delivery
	 * type. A selective algorithm may instead record these slots and return
	 * NEW; ordering before SES dispatch is the PDC's responsibility.
	 */
	if (psn != rx->window.base_psn)
		return SUET_REL_RX_GAP;
	if (!suet_rel_slot(&rx->window, psn, &slot))
		return SUET_REL_RX_GAP;
	if (suet_rel_bit(rx->window.present, slot))
		return SUET_REL_RX_DROP;
	suet_rel_set(rx->window.present, slot);
	return SUET_REL_RX_NEW;
}

void suet_rel_rx_commit(struct suet_rel_rx *rx, uint32_t psn)
{
	struct suet_rel_window *window = &rx->window;
	uint32_t slot;
	bool valid = suet_rel_slot(window, psn, &slot);

	assert(valid && suet_rel_bit(window->present, slot));
	if (!valid)
		return;
	assert(!suet_rel_bit(rx->accepted, slot));
	suet_rel_set(rx->accepted, slot);
	rx->since_ack++;
	while (suet_rel_bit(rx->accepted, window->head)) {
		suet_rel_clear(rx->accepted, window->head);
		suet_rel_clear(window->present, window->head);
		suet_rel_advance(window, 1);
	}
}

void suet_rel_rx_cancel(struct suet_rel_rx *rx, uint32_t psn)
{
	uint32_t slot;

	if (suet_rel_slot(&rx->window, psn, &slot)) {
		suet_rel_clear(rx->window.present, slot);
		suet_rel_clear(rx->accepted, slot);
	}
}

uint32_t suet_rel_rx_cack(const struct suet_rel_rx *rx)
{
	return rx->window.base_psn - 1;
}

bool suet_rel_rx_ack_needed(const struct suet_rel_rx *rx, bool requested)
{
	uint32_t interval = rx->window.capacity / 2;

	return requested || rx->since_ack >= (interval ? interval : 1);
}

void suet_rel_rx_ack_sent(struct suet_rel_rx *rx)
{
	rx->since_ack = 0;
}

void suet_rel_rx_reset(struct suet_rel_rx *rx, uint32_t base_psn)
{
	memset(rx->window.present, 0,
	       suet_rel_words(rx->window.capacity) * sizeof(uint64_t));
	memset(rx->accepted, 0,
	       suet_rel_words(rx->window.capacity) * sizeof(uint64_t));
	rx->window.base_psn = base_psn;
	rx->window.head = 0;
	rx->since_ack = 0;
}
