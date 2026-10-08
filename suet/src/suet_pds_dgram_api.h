/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2016-2017 Intel Corporation. All rights reserved.
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

#ifndef _SUET_PDS_DGRAM_API_H_
#define _SUET_PDS_DGRAM_API_H_

#include "suet_dgram.h"

/* Shared packet storage and I/O view. Layer contexts occupy separate aligned
 * areas in the same pool slot; only the owning layer initializes/interprets
 * each context. Their lifetime matches the packet allocation.
 * pkt and pkt_size always exclude the provider prefix.
 */
struct suet_pkt_entry {
	size_t pkt_size;
	fi_addr_t dgram_av_addr;
	void *pkt;
	void *pds_ctx;
	void *ses_ctx;
	size_t zc_pld_iov_count;
	struct iovec zc_pld_iov[SUET_IOV_LIMIT + 1];
	void *zc_pld_desc[SUET_IOV_LIMIT + 1];
};

/* Direct calls under the domain FEP lock (or exclusive setup/close).
 * Allocation returns a caller-owned packet. Send borrows it until tx_done;
 * failure leaves it caller-owned and schedules no completion. Local TX
 * completion does not imply remote acknowledgment; PDS may retain the packet
 * for retry. Free requires no provider access and no outstanding PDS/SES
 * retention; the caller must unlink it first. SES may retain an RX packet and
 * later release it through the same API.
 */
struct suet_pkt_entry *suet_dgram_pkt_alloc(struct suet_domain *domain);
void suet_dgram_pkt_free(struct suet_pkt_entry *pkt);
bool suet_dgram_pkt_in_use(const struct suet_pkt_entry *pkt);
ssize_t suet_dgram_send(struct suet_domain *domain, struct suet_pkt_entry *pkt);
size_t suet_dgram_max_pkt_size(const struct suet_domain *domain);
fi_addr_t suet_dgram_av_get_addr_by_peer_idx(struct suet_domain *domain,
					     int peer_idx);
int suet_dgram_av_get_peer_idx_by_addr(struct suet_domain *domain,
				       fi_addr_t addr);

/* Datagram removes RX packets from its posted list and normalizes their
 * length before transferring ownership to PDS. TX completion ends provider
 * access, including on error; PDS decides whether to retain/retry/free.
 * status is zero on success or a negative FI error on failure.
 */
void suet_pds_receive(struct suet_domain *domain, struct suet_pkt_entry *pkt,
		      fi_addr_t src_addr);
void suet_pds_tx_done(struct suet_domain *domain, struct suet_pkt_entry *pkt,
		      int status);

#endif
