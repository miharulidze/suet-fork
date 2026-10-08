/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Go-Back-N and selective repeat behind a PDC-independent contract. */
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
	*slot = window->head + offset;
	if (*slot >= window->capacity)
		*slot -= window->capacity;
	return true;
}

static void suet_rel_advance(struct suet_rel_window *window, uint32_t count)
{
	window->base_psn += count;
	window->head += count;
	if (window->head >= window->capacity)
		window->head -= window->capacity;
}

int suet_rel_tx_init(struct suet_rel_tx *tx, uint32_t base_psn,
		     uint32_t capacity, uint32_t max_retries,
		     enum suet_rel_algorithm algorithm)
{
	int ret;

	memset(tx, 0, sizeof(*tx));
	ret = suet_rel_window_init(&tx->window, base_psn, capacity);
	if (ret)
		return ret;
	tx->attempt_time = calloc(capacity, sizeof(*tx->attempt_time));
	tx->attempts = calloc(capacity, sizeof(*tx->attempts));
	tx->nacked = calloc(suet_rel_words(capacity), sizeof(uint64_t));
	tx->sacked = calloc(suet_rel_words(capacity), sizeof(uint64_t));
	if (!tx->attempt_time || !tx->sacked || !tx->nacked || !tx->attempts) {
		suet_rel_tx_cleanup(tx);
		return -ENOMEM;
	}
	tx->max_retries = max_retries;
	tx->algorithm = algorithm;
	return 0;
}

void suet_rel_tx_cleanup(struct suet_rel_tx *tx)
{
	free(tx->attempts);
	free(tx->nacked);
	free(tx->sacked);
	free(tx->attempt_time);
	free(tx->window.present);
	memset(tx, 0, sizeof(*tx));
}

bool suet_rel_tx_can_track(const struct suet_rel_tx *tx, uint32_t psn,
			   uint32_t *slot)
{
	return suet_rel_slot(&tx->window, psn, slot) &&
	       !suet_rel_bit(tx->window.present, *slot);
}

void suet_rel_tx_track(struct suet_rel_tx *tx, uint32_t slot)
{
	assert(slot < tx->window.capacity &&
	       !suet_rel_bit(tx->window.present, slot));
	tx->attempts[slot] = 0;
	suet_rel_clear(tx->sacked, slot);
	suet_rel_set(tx->window.present, slot);
	tx->tracked++;
}

void suet_rel_tx_attempt(struct suet_rel_tx *tx, uint32_t slot, uint64_t now)
{
	assert(slot < tx->window.capacity &&
	       suet_rel_bit(tx->window.present, slot));
	if (tx->attempts[slot] != UINT32_MAX)
		tx->attempts[slot]++;
	if (tx->attempts[slot] > tx->max_attempts)
		tx->max_attempts = tx->attempts[slot];
	tx->attempt_time[slot] = now;
	if (suet_rel_bit(tx->nacked, slot)) {
		suet_rel_clear(tx->nacked, slot);
		tx->pending_nacks--;
	}
}

/* Walk a circular range by bitmap words, splitting at the physical end.
 * Validation is read-only: a hole must never partially retire an ACK.
 */
static bool suet_rel_range(struct suet_rel_window *window, uint32_t count,
			   bool clear)
{
	uint32_t slot = window->head, n, shift;
	uint64_t mask;

	while (count) {
		shift = slot % 64;
		n = 64 - shift;
		if (n > window->capacity - slot)
			n = window->capacity - slot;
		if (n > count)
			n = count;
		mask = (UINT64_MAX >> (64 - n)) << shift;
		if (clear)
			window->present[slot / 64] &= ~mask;
		else if ((window->present[slot / 64] & mask) != mask)
			return false;
		count -= n;
		slot += n;
		if (slot == window->capacity)
			slot = 0;
	}
	return true;
}

enum suet_rel_ack_result suet_rel_tx_ack(struct suet_rel_tx *tx, uint32_t cack,
					 struct suet_rel_retired *retired)
{
	struct suet_rel_window *window = &tx->window;
	uint32_t count = cack - window->base_psn + 1;
	uint32_t slot, n, left, shift;
	uint64_t mask;

	memset(retired, 0, sizeof(*retired));
	if ((int32_t) (cack - window->base_psn) < 0)
		return SUET_REL_ACK_STALE;
	if (count > window->capacity)
		return SUET_REL_ACK_INVALID;
	/* A cumulative ACK must not cover an unsubmitted hole. */
	if (!suet_rel_range(window, count, false))
		return SUET_REL_ACK_INVALID;
	retired->first_psn = window->base_psn;
	retired->first_slot = window->head;
	retired->count = count;
	retired->newly_acked = count;
	slot = window->head;
	left = count;
	while (left) {
		shift = slot % 64;
		n = 64 - shift;
		if (n > window->capacity - slot)
			n = window->capacity - slot;
		if (n > left)
			n = left;
		mask = (UINT64_MAX >> (64 - n)) << shift;
		retired->newly_acked -=
			__builtin_popcountll(tx->sacked[slot / 64] & mask);
		tx->sacked[slot / 64] &= ~mask;
		tx->pending_nacks -=
			__builtin_popcountll(tx->nacked[slot / 64] & mask);
		tx->nacked[slot / 64] &= ~mask;
		left -= n;
		slot += n;
		if (slot == window->capacity)
			slot = 0;
	}
	suet_rel_range(window, count, true);
	tx->tracked -= count;
	suet_rel_advance(window, count);
	tx->retry_count = 0;
	tx->rto_round = 0;
	return SUET_REL_ACK_ADVANCED;
}

uint32_t suet_rel_tx_sack(struct suet_rel_tx *tx, uint32_t base, uint64_t bits)
{
	uint32_t count = 0, slot, bit;

	while (bits) {
		bit = __builtin_ctzll(bits);
		bits &= bits - 1;
		if (!suet_rel_slot(&tx->window, base + bit, &slot) ||
		    !suet_rel_bit(tx->window.present, slot) ||
		    suet_rel_bit(tx->sacked, slot))
			continue;
		suet_rel_set(tx->sacked, slot);
		if (suet_rel_bit(tx->nacked, slot)) {
			suet_rel_clear(tx->nacked, slot);
			tx->pending_nacks--;
		}
		count++;
	}
	return count;
}

uint64_t suet_rel_rx_sack(const struct suet_rel_rx *rx, uint32_t base)
{
	uint64_t bits = 0;
	uint32_t slot;

	for (uint32_t i = 0; i < 64; i++)
		if (suet_rel_slot(&rx->window, base + i, &slot) &&
		    suet_rel_bit(rx->accepted, slot))
			bits |= UINT64_C(1) << i;
	return bits;
}

uint32_t suet_rel_tx_cack(const struct suet_rel_tx *tx)
{
	return tx->window.base_psn - 1;
}

bool suet_rel_tx_nack(struct suet_rel_tx *tx, uint32_t psn, uint64_t now,
		      uint32_t delay_ms)
{
	uint32_t slot;

	if (!suet_rel_slot(&tx->window, psn, &slot) ||
	    !suet_rel_bit(tx->window.present, slot) ||
	    suet_rel_bit(tx->sacked, slot) || suet_rel_bit(tx->nacked, slot))
		return false;
	suet_rel_set(tx->nacked, slot);
	tx->pending_nacks++;
	tx->attempt_time[slot] = now + delay_ms;
	tx->rto_round = 0;
	return true;
}

bool suet_rel_tx_retry_next(struct suet_rel_tx *tx, uint64_t now,
			    uint32_t *cursor, uint32_t *psn,
			    uint32_t *retry_slot)
{
	const struct suet_rel_window *window = &tx->window;
	uint64_t timeout =
		tx->rto_round >= 12 ? 4000 : UINT64_C(1) << tx->rto_round;
	uint32_t offset, slot;
	bool nack;

	if (!tx->tracked)
		return false;
	while (*cursor < window->capacity) {
		offset = (*cursor)++;
		slot = window->head + offset;
		if (slot >= window->capacity)
			slot -= window->capacity;
		if (!suet_rel_bit(window->present, slot) ||
		    (tx->algorithm == SUET_REL_SR &&
		     suet_rel_bit(tx->sacked, slot)))
			continue;
		nack = suet_rel_bit(tx->nacked, slot);
		if (nack && now < tx->attempt_time[slot])
			continue;
		if (!nack && (now < tx->attempt_time[slot] ||
			      now - tx->attempt_time[slot] < timeout)) {
			if (tx->algorithm == SUET_REL_GBN && !tx->pending_nacks)
				return false;
			continue;
		}
		tx->rto_attempted |= !nack;
		*retry_slot = slot;
		*psn = window->base_psn + offset;
		return true;
	}
	return false;
}

void suet_rel_tx_retry_end(struct suet_rel_tx *tx, bool attempted)
{
	if (attempted && tx->retry_count != UINT32_MAX)
		tx->retry_count++;
	if (attempted && tx->rto_attempted && tx->rto_round != UINT32_MAX)
		tx->rto_round++;
	tx->rto_attempted = false;
}

bool suet_rel_tx_failed(const struct suet_rel_tx *tx)
{
	if (!tx->max_attempts || tx->max_attempts - 1 <= tx->max_retries)
		return false;
	for (uint32_t slot = 0; slot < tx->window.capacity; slot++)
		if (suet_rel_bit(tx->window.present, slot) &&
		    tx->attempts[slot] &&
		    tx->attempts[slot] - 1 > tx->max_retries)
			return true;
	return false;
}

int suet_rel_rx_init(struct suet_rel_rx *rx, uint32_t base_psn,
		     uint32_t capacity, enum suet_rel_algorithm algorithm)
{
	int ret;

	memset(rx, 0, sizeof(*rx));
	ret = suet_rel_window_init(&rx->window, base_psn, capacity);
	if (ret)
		return ret;
	rx->algorithm = algorithm;
	rx->sack_base = base_psn - 1;
	rx->max_psn = base_psn - 1;
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
	if (rx->algorithm == SUET_REL_GBN && psn != rx->window.base_psn)
		return SUET_REL_RX_GAP;
	if (!suet_rel_slot(&rx->window, psn, &slot))
		return SUET_REL_RX_GAP;
	if (suet_rel_bit(rx->window.present, slot))
		return retransmit && suet_rel_bit(rx->accepted, slot) ?
			       SUET_REL_RX_REPLAY :
			       SUET_REL_RX_DROP;
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
	if ((int32_t) (psn - rx->max_psn) > 0)
		rx->max_psn = psn;
	if ((int32_t) (psn - rx->sack_base) < 0)
		rx->sack_base = psn;
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
	if ((int32_t) (rx->max_psn - (rx->sack_base + 63)) > 0)
		rx->sack_base += 64;
}

void suet_rel_rx_reset(struct suet_rel_rx *rx, uint32_t base_psn)
{
	memset(rx->window.present, 0,
	       suet_rel_words(rx->window.capacity) * sizeof(uint64_t));
	memset(rx->accepted, 0,
	       suet_rel_words(rx->window.capacity) * sizeof(uint64_t));
	rx->window.base_psn = base_psn;
	rx->window.head = 0;
	rx->sack_base = base_psn - 1;
	rx->max_psn = base_psn - 1;
	rx->since_ack = 0;
}
