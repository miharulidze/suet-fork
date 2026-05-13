/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2015-2018 Intel Corporation. All rights reserved.
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

#include "suet.h"

#define SUET_TX_CAPS (OFI_TX_MSG_CAPS | FI_TAGGED | FI_RMA | FI_WRITE | FI_ATOMICS)
#define SUET_RX_CAPS (FI_SOURCE | FI_RMA_EVENT | OFI_RX_MSG_CAPS | FI_TAGGED | \
		     FI_RMA | FI_REMOTE_WRITE | FI_ATOMICS |              \
		     FI_DIRECTED_RECV | FI_MULTI_RECV)
#define SUET_TX_OP_FLAGS (FI_INJECT | FI_INJECT_COMPLETE | FI_COMPLETION	|   \
			 FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE)
#define SUET_RX_OP_FLAGS (FI_MULTI_RECV | FI_COMPLETION)
#define SUET_DOMAIN_CAPS (FI_LOCAL_COMM | FI_REMOTE_COMM)

#define SUET_MSG_ORDER (FI_ORDER_ATOMIC_RAR | FI_ORDER_ATOMIC_RAW |	\
		       FI_ORDER_ATOMIC_WAR | FI_ORDER_ATOMIC_WAW |	\
		       FI_ORDER_RAR | FI_ORDER_RAS | FI_ORDER_RAW |	\
		       FI_ORDER_RMA_RAR | FI_ORDER_RMA_RAW |		\
		       FI_ORDER_RMA_WAW | FI_ORDER_SAS | FI_ORDER_SAW |	\
		       FI_ORDER_WAS | FI_ORDER_WAW)

struct fi_tx_attr suet_tx_attr = {
	.caps = SUET_TX_CAPS,
	.op_flags = SUET_TX_OP_FLAGS,
	.comp_order = 0,
	.msg_order = SUET_MSG_ORDER,
	.inject_size = PDS_MAX_MTU_SIZE - sizeof(struct pds_req_hdr),
	.size = (1ULL << SUET_MAX_TX_BITS),
	.iov_limit = SUET_IOV_LIMIT,
	.rma_iov_limit = 1,
};

struct fi_rx_attr suet_rx_attr = {
	.caps = SUET_RX_CAPS,
	.op_flags = SUET_RX_OP_FLAGS,
	.comp_order = 0,
	.msg_order = SUET_MSG_ORDER,
	.size = (1ULL << SUET_MAX_RX_BITS),
	.iov_limit = SUET_IOV_LIMIT
};

struct fi_ep_attr suet_ep_attr = {
	.type = FI_EP_RDM,
	.protocol = FI_PROTO_UET,
	.protocol_version = 1,
	.max_msg_size = UINT32_MAX, /* ses_req_hdr.request_length is uint32_t */
	.tx_ctx_cnt = 1,
	.rx_ctx_cnt = 1,
	.max_order_raw_size = UINT32_MAX,
	.max_order_waw_size = UINT32_MAX,
	.mem_tag_format = FI_TAG_GENERIC,
};

struct fi_domain_attr suet_domain_attr = {
	.caps = SUET_DOMAIN_CAPS,
	.threading = FI_THREAD_SAFE,
	.control_progress = FI_PROGRESS_MANUAL,
	.data_progress = FI_PROGRESS_MANUAL,
	.resource_mgmt = FI_RM_ENABLED,
	.av_type = FI_AV_UNSPEC,
	.mr_mode = OFI_MR_BASIC | OFI_MR_SCALABLE,
	.cq_data_size = sizeof(uint64_t),
	.mr_key_size = sizeof(uint64_t),
	.cq_cnt = 128,
	.ep_cnt = 128,
	.tx_ctx_cnt = 1,
	.rx_ctx_cnt = 1,
	.max_ep_tx_ctx = 1,
	.max_ep_rx_ctx = 1,
	.mr_iov_limit = 1,
};

struct fi_fabric_attr suet_fabric_attr = {
	.prov_version = OFI_VERSION_DEF_PROV,
};

struct fi_info suet_info = {
	.caps = SUET_DOMAIN_CAPS | SUET_TX_CAPS | SUET_RX_CAPS,
	.addr_format = FI_FORMAT_UNSPEC,
	.tx_attr = &suet_tx_attr,
	.rx_attr = &suet_rx_attr,
	.ep_attr = &suet_ep_attr,
	.domain_attr = &suet_domain_attr,
	.fabric_attr = &suet_fabric_attr
};

struct util_prov suet_util_prov = {
	.prov = &suet_prov,
	.info = &suet_info,
	.flags = 0,
};
