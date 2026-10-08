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

#ifndef _SUET_SES_PDS_API_H_
#define _SUET_SES_PDS_API_H_

#include <stdbool.h>
#include <stdint.h>
#include <rdma/fabric.h>

/* Direct-call SES/PDS contract. All calls run under the domain FEP lock
 * (or exclusive domain initialization/close). Neither layer may inspect
 * the other's opaque contexts. No callback table is required.
 */
struct suet_domain;
struct suet_pkt_entry;
struct ses_resp_hdr;
struct suet_pds_ses_resp_entry;

/* Semantic response data shared with PDS for transmission and replay. */
struct suet_ses_resp {
	uint8_t ses_opcode;
	uint8_t ses_rc;
	uint8_t list;
	uint16_t message_id;
	uint32_t modified_length;
};

/* Transient SES receive result. PDS consumes this without modifying it;
 * only resp is retained for guaranteed delivery, never the dispatch state.
 */
struct suet_ses_rx_dispatch_result {
	struct suet_ses_resp resp;
	int status; /* local dispatch error; PDS selects the wire NACK code */
	bool pkt_retained; /* SES owns the packet; PDS must not free it */
	bool ack_now;
	bool gtd_del; /* semantic response requires guaranteed delivery */
	void *completion; /* SES receive operation, consumed by commit */
	bool accepted; /* false leaves the receive PSN unchanged */
};

/* TX allocation reserves transport queue state, but does not send. Submit
 * follows SES header initialization. PDS borrows the SES
 * context until suet_ses_tx_done(), or explicit cancellation. PDS owns PSNs,
 * packet records and retry state; SES owns segmentation and completions.
 * Cancellation detaches the operation; it does not retire in-flight packets
 * or relax the existing datagram/zero-copy buffer lifetime requirements.
 */
void *suet_pds_tx_alloc(struct suet_domain *domain, fi_addr_t addr,
			void *context);
void suet_pds_tx_submit(void *pds_ctx, uint32_t num_pkts);
void suet_pds_tx_cancel(void *pds_ctx);
void suet_ses_prepare_tx(void *context, struct suet_pkt_entry *pkt,
			 uint32_t segment);
void suet_ses_tx_done(void *context, int err, int prov_errno);
bool suet_ses_response_is_error(const struct ses_resp_hdr *resp);
bool suet_ses_tx_response(void *context, const struct ses_resp_hdr *resp);

/* RX contexts belong to SES. pds_ctx is an opaque response route.
 * Receive may retain the packet by setting pkt_retained. PDS persists any
 * required response before commit; commit alone completes the SES operation.
 */
void *suet_ses_rx_open(struct suet_domain *domain, void *pds_ctx, int peer_idx);
void suet_ses_rx_close(void *ses_ctx);
bool suet_ses_rx_busy(void *ses_ctx);
bool suet_ses_rx_valid(struct suet_domain *domain, struct suet_pkt_entry *pkt);
void suet_ses_receive(void *ses_ctx, struct suet_pkt_entry *pkt,
		      struct suet_ses_rx_dispatch_result *resp);
void suet_ses_rx_commit(void *ses_ctx,
			const struct suet_ses_rx_dispatch_result *resp);
void suet_ses_default_response(struct suet_pkt_entry *pkt,
			       struct suet_ses_resp *resp);

/* Reserve is called during in-order SES dispatch. PDS records the current
 * request PSN and num_pkts as the replay range.
 * A reserved response has one SES reference and one PDS retention reference.
 * Complete consumes the SES reference and sends the response if the route is
 * still live. Cancel consumes it without sending. PDC close invalidates the
 * route but leaves an outstanding SES handle safe to complete/cancel.
 */
struct suet_pds_ses_resp_entry *
suet_pds_response_reserve(struct suet_domain *domain, void *pds_ctx,
			  const struct suet_ses_resp *resp, uint16_t num_pkts);
void suet_pds_response_complete(struct suet_pds_ses_resp_entry *response,
				const struct suet_ses_resp *resp);
void suet_pds_response_cancel(struct suet_pds_ses_resp_entry *response);

#endif
