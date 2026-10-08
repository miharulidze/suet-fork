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

struct suet_rel_tx {
	struct suet_rel_window window;
	uint64_t *attempt_time;
	uint32_t tracked;
	uint32_t retry_count;
	uint32_t max_retries;
};

struct suet_rel_rx {
	struct suet_rel_window window;
	uint64_t *accepted;
	uint32_t since_ack;
};

/* Slots occupied before ACK processing. Consume every pointer in this range
 * before submitting new packets or otherwise reusing the advanced window.
 * Packet storage may remain borrowed by datagram after its slot is detached.
 */
struct suet_rel_retired {
	uint32_t first_psn;
	uint32_t first_slot;
	uint32_t count;
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
		     uint32_t capacity, uint32_t max_retries);
void suet_rel_tx_cleanup(struct suet_rel_tx *tx);
int suet_rel_rx_init(struct suet_rel_rx *rx, uint32_t base_psn,
		     uint32_t capacity);
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
uint32_t suet_rel_tx_cack(const struct suet_rel_tx *tx);
/* NACK PSN validity is checked here. Current GBN recovers via timeout;
 * NACK reception intentionally does not schedule an immediate retry.
 */
bool suet_rel_tx_nack(struct suet_rel_tx *tx, uint32_t psn);
/* cursor starts at zero for each progress pass. Stop if local access prevents
 * resubmission, or if a submission fails. Finish the pass exactly once.
 */
bool suet_rel_tx_retry_next(const struct suet_rel_tx *tx, uint64_t now,
			    uint32_t *cursor, uint32_t *psn, uint32_t *slot);
void suet_rel_tx_retry_end(struct suet_rel_tx *tx, bool attempted);
bool suet_rel_tx_failed(const struct suet_rel_tx *tx);

enum suet_rel_rx_result suet_rel_rx_record(struct suet_rel_rx *rx, uint32_t psn,
					   bool retransmit);
void suet_rel_rx_commit(struct suet_rel_rx *rx, uint32_t psn);
void suet_rel_rx_cancel(struct suet_rel_rx *rx, uint32_t psn);
uint32_t suet_rel_rx_cack(const struct suet_rel_rx *rx);
bool suet_rel_rx_ack_needed(const struct suet_rel_rx *rx, bool requested);
void suet_rel_rx_ack_sent(struct suet_rel_rx *rx);
/* PDC lifecycle may discard a receive window; no close protocol lives here. */
void suet_rel_rx_reset(struct suet_rel_rx *rx, uint32_t base_psn);

#endif
