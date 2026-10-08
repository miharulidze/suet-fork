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

#ifndef _SUET_SES_H_
#define _SUET_SES_H_

#include <rdma/fi_rma.h>

#include "suet_pds_dgram_api.h"
#include "suet_proto.h"
#include "suet_ses_pds_api.h"

/* SES-only membership for packets retained by an unexpected message. */
struct suet_ses_pkt_entry {
	struct dlist_entry entry;
	struct suet_pkt_entry *pkt;
};

struct suet_ses_tx_entry {
	struct dlist_entry entry;
	uint16_t tx_id;
	uint32_t num_pkts;
	uint32_t op;
	uint32_t flags;
	uint8_t iov_count;

	struct iovec iov[SUET_IOV_LIMIT];
	void *zc_desc[SUET_IOV_LIMIT];
	struct fid_mr *zc_internal_mrs[SUET_IOV_LIMIT];
	struct fi_cq_tagged_entry cq_entry;

	struct suet_ep *ep;
	void *pds_ctx; /* opaque PDS transmission */
	size_t hdr_len;

	/* Cached semantic headers; PDS headers live only in packet records. */
	struct {
		struct ses_req_hdr ses;
		struct ses_msg_amo_hdr amo;
	} cached_hdr;
};

struct suet_ses_rx_entry {
	struct dlist_entry entry;
	uint16_t rx_id;
	uint64_t bytes_copied;
	uint32_t pkts_received;
	uint32_t num_pkts;
	uint32_t op;
	uint32_t flags;
	uint64_t ignore;
	uint8_t iov_count;

	struct iovec iov[SUET_IOV_LIMIT];
	struct fi_cq_tagged_entry cq_entry;
	struct suet_ep *ep;
	int peer_idx;
};

struct suet_ses_unexp_msg {
	struct dlist_entry entry;
	struct dlist_entry pkt_list;
	int peer_idx; /* AV peer index for directed-recv matching */
	struct suet_pds_ses_resp_entry *gtd_del_resp;
	void *ses_ctx;
};

struct suet_ses_resources {
	struct ofi_bufpool *tx_entry_pool;
	struct ofi_bufpool *rx_entry_pool;
	struct ofi_bufpool *unexp_msg_pool;
	struct dlist_entry rx_ctx_list;
};

/* Ordered receive state belongs to SES, not to a PDC. Unexpected messages
 * hold references so a buffered message remains safe after transport close.
 */
struct suet_ses_rx_ctx {
	struct dlist_entry entry;
	struct suet_domain *domain;
	void *pds_ctx; /* opaque PDS response route */
	unsigned int refs;
	int peer_idx;
	uint16_t curr_rx_id;
	struct suet_ses_unexp_msg *curr_unexp;
	struct dlist_entry rx_list;
};

struct suet_ses_msg_match_attr {
	int peer_idx;
	uint64_t tag;
	uint64_t ignore;
};

void suet_ses_submit(struct suet_ses_tx_entry *entry);
void suet_ses_init_rma_iov(const struct fi_rma_iov *rma_iov,
			   struct suet_ses_tx_entry *tx_entry);
struct suet_ses_tx_entry *
suet_ses_tx_entry_init_common(struct suet_ep *ep, fi_addr_t addr, uint32_t op,
			      const struct iovec *iov, void **desc,
			      size_t iov_count, uint64_t tag, uint64_t data,
			      uint32_t flags, void *context);
int suet_ses_peek_recv(struct suet_ep *suet_ep, int peer_idx, uint64_t tag,
		       uint64_t ignore, void *context, uint64_t flags,
		       struct dlist_entry *unexp_list);
int suet_ses_discard_recv(struct suet_ep *suet_ep, void *context,
			  struct suet_ses_unexp_msg *unexp_msg);
int suet_ses_progress_unexp_list(struct suet_ep *ep,
				 struct dlist_entry *unexp_list,
				 struct dlist_entry *rx_list,
				 struct suet_ses_rx_entry *rx_entry);
struct suet_ses_rx_entry *suet_ses_rx_entry_init(struct suet_ep *ep,
						 const struct iovec *iov,
						 size_t iov_count, uint64_t tag,
						 uint64_t ignore, void *context,
						 int peer_idx, uint32_t op,
						 uint32_t flags);
void suet_ses_rx_entry_free(struct suet_ses_rx_entry *rx_entry);
void suet_ses_complete_unexp_msg(struct suet_ep *ep,
				 struct suet_ses_rx_entry *rx_entry,
				 struct suet_ses_unexp_msg *unexp_msg);

#endif
