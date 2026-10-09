/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#ifndef _SUET_REL_H_
#define _SUET_REL_H_

#include <stdbool.h>
#include <stdint.h>

/* Direct-call reliability contract. No PDC types, wire headers, packet
 * pointers or SES state cross this interface. The caller serializes access
 * and supplies monotonic elapsed milliseconds for every time argument.
 * PSNs are assigned by PDC; bitmap slots are shared with its pointer array.
 */
struct suet_rel_window {
	uint32_t base_psn;
	uint32_t capacity;
	uint32_t head;
	uint64_t *present;
};

enum suet_rel_algorithm {
	SUET_REL_GBN,
	SUET_REL_SR,
};

struct suet_rel_tx {
	enum suet_rel_algorithm algorithm;
	uint64_t *sacked;
	uint64_t *nacked;
	uint32_t pending_nacks;
	uint32_t rto_round;
	bool rto_attempted;
	struct suet_rel_window window;
	uint64_t *attempt_time;
	uint32_t *attempts;
	/* Upper bound avoids scanning healthy windows for retry exhaustion. */
	uint32_t max_attempts;
	uint32_t tracked;
	uint32_t retry_count;
	uint32_t max_retries;
};

struct suet_rel_rx {
	enum suet_rel_algorithm algorithm;
	struct suet_rel_window window;
	uint64_t *accepted;
	uint32_t since_ack;
	uint32_t sack_base;
	uint32_t max_psn;
	uint32_t ack_target_psn;
	bool ack_pending;
};

/* Slots occupied before ACK processing. Consume every pointer in this range
 * before submitting new packets or otherwise reusing the advanced window.
 * Packet storage may remain borrowed by datagram after its slot is detached.
 */
struct suet_rel_retired {
	uint32_t first_psn;
	uint32_t first_slot;
	uint32_t count;
	uint32_t newly_acked;
};

enum suet_rel_ack_result {
	SUET_REL_ACK_INVALID = -1,
	SUET_REL_ACK_STALE,
	SUET_REL_ACK_ADVANCED,
};

enum suet_rel_rx_result {
	SUET_REL_RX_NEW,
	SUET_REL_RX_DROP,
	SUET_REL_RX_REPLAY,
	SUET_REL_RX_GAP,
};

int suet_rel_tx_init(struct suet_rel_tx *tx, uint32_t base_psn,
		     uint32_t capacity, uint32_t max_retries,
		     enum suet_rel_algorithm algorithm);
void suet_rel_tx_cleanup(struct suet_rel_tx *tx);
int suet_rel_rx_init(struct suet_rel_rx *rx, uint32_t base_psn,
		     uint32_t capacity, enum suet_rel_algorithm algorithm);
void suet_rel_rx_cleanup(struct suet_rel_rx *rx);
bool suet_rel_slot(const struct suet_rel_window *window, uint32_t psn,
		   uint32_t *slot);
bool suet_rel_tx_can_track(const struct suet_rel_tx *tx, uint32_t psn, uint32_t *slot);
void suet_rel_tx_track(struct suet_rel_tx *tx, uint32_t slot);
/* Track/attempt consume the slot returned by admission or retry_next.
 * The caller must not advance the window between these calls. */
/* Preserve GBN's existing timing of attempts, including local send failure. */
void suet_rel_tx_attempt(struct suet_rel_tx *tx, uint32_t slot, uint64_t now);
enum suet_rel_ack_result suet_rel_tx_ack(struct suet_rel_tx *tx, uint32_t cack,
					 struct suet_rel_retired *retired);
/* Positive receipt evidence only; zeros never revoke previous evidence. */
uint32_t suet_rel_tx_sack(struct suet_rel_tx *tx, uint32_t base, uint64_t bits);
uint64_t suet_rel_rx_sack(const struct suet_rel_rx *rx, uint32_t base);
uint32_t suet_rel_tx_cack(const struct suet_rel_tx *tx);
/* Schedule a valid, not already received packet after delay_ms (zero is immediate).
 * Duplicate NACKs coalesce; positive receipt evidence takes precedence. */
bool suet_rel_tx_nack(struct suet_rel_tx *tx, uint32_t psn, uint64_t now,
		      uint32_t delay_ms);
/* cursor starts at zero for each progress pass. Stop if local access prevents
 * resubmission, or if a submission fails. Finish the pass exactly once.
 */
bool suet_rel_tx_retry_next(struct suet_rel_tx *tx, uint64_t now,
			    uint32_t *cursor, uint32_t *psn, uint32_t *slot);
void suet_rel_tx_retry_end(struct suet_rel_tx *tx, bool attempted);
bool suet_rel_tx_failed(const struct suet_rel_tx *tx);

enum suet_rel_rx_result suet_rel_rx_record(struct suet_rel_rx *rx, uint32_t psn,
					   bool retransmit);
void suet_rel_rx_commit(struct suet_rel_rx *rx, uint32_t psn);
void suet_rel_rx_cancel(struct suet_rel_rx *rx, uint32_t psn);
uint32_t suet_rel_rx_cack(const struct suet_rel_rx *rx);
/* An interim SACK does not satisfy a request above the cumulative horizon. */
void suet_rel_rx_request_ack(struct suet_rel_rx *rx, uint32_t psn);
bool suet_rel_rx_ack_needed(const struct suet_rel_rx *rx, uint32_t psn,
			    bool requested);
/* Call only after successful ACK submission to the datagram layer. */
void suet_rel_rx_ack_sent(struct suet_rel_rx *rx);
/* PDC lifecycle may discard a receive window; no close protocol lives here. */
void suet_rel_rx_reset(struct suet_rel_rx *rx, uint32_t base_psn);

#endif
