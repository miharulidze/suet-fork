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

#ifndef _SUET_DGRAM_H_
#define _SUET_DGRAM_H_

#include <assert.h>
#include <ofi_indexer.h>
#include <ofi_tree.h>
#include <ofi_util.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_eq.h>

#define SUET_CQ_MAX_BATCH 64
#define SUET_IOV_LIMIT	  4

struct suet_domain;

/* Underlying datagram-provider resources shared by the SUET domain. */
struct suet_dgram_resources {
	uint64_t rx_metadata;
	bool sendmsg_ev;
	struct fid_domain *domain;
	struct fid_ep *ep;
	struct fid_cq *tx_cq;
	struct fid_cq *rx_cq;
	int cq_read_batch_size;
	struct fi_cq_msg_entry tx_cq_entries[SUET_CQ_MAX_BATCH];
	union {
		struct fi_cq_msg_entry msg[SUET_CQ_MAX_BATCH];
		struct fi_cq_data_entry data[SUET_CQ_MAX_BATCH];
	} rx_cq_entries;
	fi_addr_t rx_cq_addrs[SUET_CQ_MAX_BATCH];

	struct fid_av *av;
	struct ofi_genlock av_lock;
	struct ofi_rbmap rbmap;
	struct indexer peer_idx_to_av_addr;
	size_t addrlen;

	size_t pds_pkt_size;
	size_t ses_pkt_size;
	size_t tx_prefix_size;
	size_t rx_prefix_size;
	int do_local_mr;
	uint64_t mr_key;

	struct ofi_bufpool *tx_pkt_entry_pool;
	struct ofi_bufpool *rx_pkt_entry_pool;
	struct dlist_entry rx_pkt_list;

	ssize_t max_mtu_sz;
	size_t max_pkt_size; /* maximum wire packet, excluding provider prefix
			      */
};

/* Setup/close are exclusive; progress runs under the domain FEP lock.
 * Stop releases provider access to packet storage before PDS/SES cleanup.
 */
int suet_dgram_init(struct suet_domain *domain, struct fid_fabric *fabric,
		    struct fi_info *info, void *context, size_t mtu_limit,
		    size_t pds_pkt_size, size_t ses_pkt_size);
int suet_dgram_stop(struct suet_domain *domain);
int suet_dgram_cleanup(struct suet_domain *domain);
void suet_dgram_progress(struct suet_domain *domain);

/* Backend management. Handles/descriptors are opaque to callers; no
 * backend fid is exposed. Peer IDs live until domain cleanup, even after
 * the corresponding address is removed from an application AV.
 * AV operations take their own lock and never acquire a public AV lock.
 */
int suet_dgram_av_insert(struct suet_domain *domain, const void *addr,
			 int *peer_idx, uint64_t flags, void *context);
const char *suet_dgram_av_straddr(struct suet_domain *domain, const void *addr,
				  char *buf, size_t *len);
int suet_dgram_mr_reg(struct suet_domain *domain, struct fi_mr_attr *attr,
		      uint64_t flags, bool gen_key, void **handle);
int suet_dgram_mr_close(void *handle);
void *suet_dgram_mr_desc(void *handle);
int suet_dgram_setname(struct suet_domain *domain, void *addr, size_t len);
int suet_dgram_getname(struct suet_domain *domain, void *addr, size_t *len);
const char *suet_dgram_cq_strerror(struct suet_domain *domain, int err,
				   const void *data, char *buf, size_t len);
int suet_dgram_fabric_open(struct fi_fabric_attr *attr, void *context,
			   void **handle);
int suet_dgram_fabric_close(void *handle);
int suet_dgram_getinfo(uint32_t version, const char *node, const char *service,
		       uint64_t flags, const struct fi_info *hints,
		       struct fi_info **info);
int suet_dgram_ep_sizes(struct suet_domain *domain, struct fi_info *info,
			size_t *tx_size, size_t *rx_size);

#endif
