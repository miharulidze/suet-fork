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

#ifndef _SUET_PDS_H_
#define _SUET_PDS_H_

#include "suet_cc.h"
#include "suet_pds_dgram_api.h"
#include "suet_rel.h"
#include "suet_ses_pds_api.h"

/* PDS-only reliability state and in-flight queue membership. */
struct suet_pds_pkt_entry {
	struct dlist_entry entry;
	struct suet_pkt_entry *pkt;
	uint32_t psn;
	uint16_t local_pdcid;
	bool acked;
};

struct suet_pds_resources {
	uint16_t next_pdcid;
	uint32_t psn_seed;
	struct ofi_bufpool *pds_tx_pool;
	struct ofi_bufpool *ipdc_pool;
	struct ofi_bufpool *tpdc_pool;
	struct ofi_bufpool *gtd_del_resp_pool;
	struct dlist_entry active_ipdc_list;
	struct dlist_entry active_tpdc_list;
	struct index_map local_pdcid_to_ipdc_idm;
	struct index_map local_pdcid_to_tpdc_idm;
	struct suet_ipdc *ipdc_by_dgram_av_addr_ht;
	struct suet_tpdc *tpdc_by_syn_key_ht;
};

/*
 * Spec-compatible PDC state (Section 3.5.8.2 / 3.5.8.3 / Figure 3-43, 3-44).
 *
 * Minimal subset without TSS and target-initiated close.
 * The initiator drives PDC lifecycle;
 * the target only ever observes ESTABLISHED -> CLOSED on receipt of a Close
 * Command CP.
 */
enum suet_pdc_state {
	SUET_PDC_OPENING, /* initiator: SYN attached to first request, no ACK
			     yet */
	SUET_PDC_ESTABLISHED, /* normal data flow */
	SUET_PDC_QUIESCE, /* initiator: closing requested, draining tx/in-flight
			   */
	SUET_PDC_CLOSE_ACK_WAIT, /* initiator: Close Cmd CP sent, awaiting Close
				    ACK */
	SUET_PDC_CLOSED, /* terminal, free pending */
};

struct suet_ipdc_key {
	fi_addr_t dgram_av_addr;
	enum suet_pdc_type type;
} __attribute__((packed));

/*
 * Initiator-side PDC - created on first send to a remote FEP.
 * Tracks TX reliability state: PSN sequence, in_flight packets, tx_list.
 * One per remote FEP and delivery type, shared by EPs on this domain.
 */
struct suet_ipdc {
	struct dlist_entry entry;
	UT_hash_handle ipdc_dgram_av_addr_handle; /* handle for
						     ipdc_by_dgram_av_addr_ht */
	fi_addr_t dgram_av_addr; /* first field of suet_ipdc_key */
	enum suet_pdc_type type;
	uint16_t local_pdcid;
	uint16_t tpdcid; /* learned from first ACK.spdcid */
	uint32_t start_psn;
	uint32_t tx_seq_no;
	uint32_t last_tx_clear_psn; /* CLEAR_PSN this initiator transmits
				     * Eager-clear: advanced with CACK_PSN when
				     * the SES response has been delivered. */
	uint32_t close_psn;
	struct suet_rel_tx rel;
	struct suet_cc cc;
	uint32_t peer_window;
	uint32_t mpr_cack;
	struct suet_pds_pkt_entry **tx_pkts; /* indexed by reliability slot */
	bool teardown_pending; /* close requested; defer QUIESCE until
				  ESTABLISHED */
	enum suet_pdc_state state;
	struct dlist_entry tx_list; /* PDS transmissions pending completion */
	struct dlist_entry
		in_flight_pkts; /* sent, awaiting NIC completion + remote ACK */
};

/*
 * Target-side PDC — created on first SYN from a remote FEP.
 * Tracks RX reliability state: expected PSN.
 * One per remote FEP and delivery type, shared by EPs on this domain.
 */
struct suet_tpdc {
	struct dlist_entry entry;
	UT_hash_handle tpdc_syn_key_handle; /* handle for tpdc_by_syn_key_ht */
	/* --- tpdc_syn_key_handle: key start --- */
	fi_addr_t dgram_av_addr; /* DGRAM-layer address of remote */
	uint16_t ipdcid; /* from SYN.spdcid */
	/* --- tpdc_syn_key_handle: key end --- */
	uint16_t local_pdcid;
	enum suet_pdc_type type;
	uint8_t ack_flags; /* feedback for the current dispatch */
	uint32_t expected_rx_psn; /* next expected PSN */
	uint32_t last_rx_clear_psn; /* highest CLEAR_PSN received in
				     * forward direction */
	struct suet_rel_rx rel;
	enum suet_pdc_state state;
	void *ses_ctx; /* opaque SES receive context */
	struct dlist_entry
		gtd_del_list; /* saved SES responses awaiting CLEAR_PSN;
			       * entries may be created out of PSN order. */
};

struct suet_tpdc_syn_key {
	fi_addr_t dgram_av_addr;
	uint16_t ipdcid;
} __attribute__((packed));

struct suet_pds_tx_entry {
	struct dlist_entry entry;
	struct suet_domain *domain;
	struct suet_ipdc *ipdc;
	void *context;
	uint32_t start_psn;
	uint32_t num_pkts;
	uint32_t next_segment;
	bool started;
};

struct suet_pds_ses_resp_entry {
	struct suet_ses_resp resp;
	struct suet_domain *domain;
	struct suet_tpdc *tpdc;
	struct dlist_entry entry;
	uint32_t psn; /* first request PSN covered by this response */
	uint16_t num_pkts; /* replay range; one packet unless reserved at SOM */
	bool reserved;
};

#endif
