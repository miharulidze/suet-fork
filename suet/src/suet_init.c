/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2015-2016 Intel Corporation. All rights reserved.
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

#include <rdma/fi_errno.h>

#include <ofi.h>
#include <ofi_prov.h>

#include "suet.h"
#include "suet_proto.h"

struct suet_env suet_env = {
	.spin_count = 1000,
	.max_peers = 1024,
	.max_unacked = 128,
	.max_eps = 256,
	.rescan = -1,
	.print_domain_counters = 0,
	.max_tx_cq_errors = 1000,
	.max_rx_cq_errors = 1000,
	.cq_read_batch_size = 16,
	.max_pkt_retry = 100,
	.max_gtd_del_resp_pool_size = 16,
	.unexp_msg_pool_size = 1024,
	.zc_mr_reg_threshold = 16384,
};

static void suet_init_env(void)
{
	fi_param_get_int(&suet_prov, "spin_count", &suet_env.spin_count);
	fi_param_get_int(&suet_prov, "max_peers", &suet_env.max_peers);
	fi_param_get_int(&suet_prov, "max_unacked", &suet_env.max_unacked);
	if (suet_env.max_unacked < 1)
		suet_env.max_unacked = 1;
	if (suet_env.max_unacked > UINT16_MAX)
		suet_env.max_unacked = UINT16_MAX;
	fi_param_get_int(&suet_prov, "max_eps", &suet_env.max_eps);
	fi_param_get_bool(&suet_prov, "rescan", &suet_env.rescan);
	fi_param_get_bool(&suet_prov, "print_domain_counters",
			  &suet_env.print_domain_counters);
	fi_param_get_int(&suet_prov, "max_tx_cq_errors",
			 &suet_env.max_tx_cq_errors);
	fi_param_get_int(&suet_prov, "max_rx_cq_errors",
			 &suet_env.max_rx_cq_errors);
	fi_param_get_int(&suet_prov, "cq_read_batch_size",
			 &suet_env.cq_read_batch_size);
	if (suet_env.cq_read_batch_size < 1)
		suet_env.cq_read_batch_size = 1;
	if (suet_env.cq_read_batch_size > SUET_CQ_MAX_BATCH)
		suet_env.cq_read_batch_size = SUET_CQ_MAX_BATCH;
	fi_param_get_int(&suet_prov, "max_pkt_retry", &suet_env.max_pkt_retry);
	if (suet_env.max_pkt_retry < 0)
		suet_env.max_pkt_retry = 0;
	fi_param_get_int(&suet_prov, "max_gtd_del_resp_pool_size",
			 &suet_env.max_gtd_del_resp_pool_size);
	if (suet_env.max_gtd_del_resp_pool_size < 1)
		suet_env.max_gtd_del_resp_pool_size = 1;
	fi_param_get_int(&suet_prov, "unexp_msg_pool_size",
			 &suet_env.unexp_msg_pool_size);
	if (suet_env.unexp_msg_pool_size < 1)
		suet_env.unexp_msg_pool_size = 1;
	fi_param_get_size_t(&suet_prov, "zc_mr_reg_threshold",
			    &suet_env.zc_mr_reg_threshold);
}

/* SUET capabilities and framing limits stay independent of backend discovery.
 */
void suet_info_from_dgram(struct fi_info *info, uint64_t caps,
			  uint64_t domain_caps, size_t max_mtu,
			  size_t prefix_size)
{
	info->caps = ofi_pick_core_flags(suet_info.caps, caps,
					 FI_LOCAL_COMM | FI_REMOTE_COMM);
	info->mode = suet_info.mode;

	*info->tx_attr = *suet_info.tx_attr;
	info->tx_attr->inject_size = MIN(max_mtu, PDS_MAX_MTU_SIZE) -
				     (sizeof(struct pds_req_hdr) + prefix_size +
				      sizeof(struct ses_msg_amo_hdr));

	*info->rx_attr = *suet_info.rx_attr;
	*info->ep_attr = *suet_info.ep_attr;
	*info->domain_attr = *suet_info.domain_attr;
	info->domain_attr->caps =
		ofi_pick_core_flags(suet_info.domain_attr->caps, domain_caps,
				    FI_LOCAL_COMM | FI_REMOTE_COMM);
}

static int suet_wrap_addr(void **addr, size_t *addrlen)
{
	struct suet_av_addr *wrapped;

	if (!*addr || *addrlen >= sizeof(struct suet_av_addr))
		return 0;

	wrapped = calloc(1, sizeof(*wrapped));
	if (!wrapped)
		return -FI_ENOMEM;

	wrapped->version = SUET_ADDR_VERSION;
	wrapped->raw_dgram_addrlen =
		(uint16_t) MIN(*addrlen, SUET_DGRAM_AV_NAME_LENGTH);
	memcpy(wrapped->raw_dgram_addr, *addr, wrapped->raw_dgram_addrlen);
	free(*addr);
	*addr = wrapped;
	*addrlen = sizeof(struct suet_av_addr);
	return 0;
}

static int suet_getinfo(uint32_t version, const char *node, const char *service,
			uint64_t flags, const struct fi_info *hints,
			struct fi_info **info)
{
	struct fi_info *cur;
	int ret;

	/* Temporarily unwrap suet_addr addresses in hints so the core
	 * provider sees raw datagram addresses. */
	struct fi_info *mut_hints = (struct fi_info *) hints;
	struct suet_av_addr_tmp_storage addr_save = {0};

	if (mut_hints)
		suet_av_info_unwrap_raw_dgram_addrs(mut_hints, &addr_save);

	if (suet_env.rescan > 0) /* Explicitly enabled */
		flags |= FI_RESCAN;
	else if (!suet_env.rescan) /* Explicitly disabled */
		flags &= ~FI_RESCAN;

	ret = suet_dgram_getinfo(version, node, service, flags, hints, info);

	if (mut_hints)
		suet_av_info_wrap_raw_dgram_addrs(mut_hints, &addr_save);

	if (ret)
		return ret;

	/* Wrap raw core-provider addresses in struct suet_addr so that
	 * fi_av_insert() and fi_getname() return a consistent format. */
	for (cur = *info; cur; cur = cur->next) {
		ret = suet_wrap_addr(&cur->src_addr, &cur->src_addrlen);
		if (ret)
			goto err;
		ret = suet_wrap_addr(&cur->dest_addr, &cur->dest_addrlen);
		if (ret)
			goto err;
	}
	return 0;

err:
	fi_freeinfo(*info);
	*info = NULL;
	return ret;
}

static void suet_fini(void)
{
	/* yawn */
}

struct fi_provider suet_prov = {.name = OFI_UTIL_PREFIX "suet",
				.version = OFI_VERSION_DEF_PROV,
				.fi_version = OFI_VERSION_LATEST,
				.getinfo = suet_getinfo,
				.fabric = suet_fabric,
				.cleanup = suet_fini};

SUET_INI
{
	fi_param_define(
		&suet_prov, "spin_count", FI_PARAM_INT,
		"Number of iterations to receive packets (0 - infinite)");
	fi_param_define(&suet_prov, "max_peers", FI_PARAM_INT,
			"Maximum number of peers to track (default: 1024)");
	fi_param_define(
		&suet_prov, "max_unacked", FI_PARAM_INT,
		"Maximum number of packets to send at once (clamped to 1..65535; default: 128)");
	fi_param_define(
		&suet_prov, "max_eps", FI_PARAM_INT,
		"Maximum number of endpoints per domain (default: 256)");
	fi_param_define(
		&suet_prov, "rescan", FI_PARAM_BOOL,
		"Force or disable rescanning for network interface changes. "
		"Setting this to true will force rescanning on each "
		"fi_getinfo() invocation; "
		"setting it to false will disable rescanning. (default: "
		"unset)");
	fi_param_define(
		&suet_prov, "print_domain_counters", FI_PARAM_BOOL,
		"Print per-domain counters (nacks/stale-acks/error-acks) "
		"to stderr when a domain is destroyed. (default: false)");
	fi_param_define(&suet_prov, "max_tx_cq_errors", FI_PARAM_INT,
			"Maximum number of TX CQ errors per domain before "
			"aborting the process with a fatal error. "
			"(default: 1000)");
	fi_param_define(
		&suet_prov, "max_rx_cq_errors", FI_PARAM_INT,
		"Maximum number of non-recoverable RX CQ errors per "
		"domain before aborting the process with a fatal error. "
		"(default: 1000)");
	fi_param_define(
		&suet_prov, "cq_read_batch_size", FI_PARAM_INT,
		"Maximum number of CQ entries to read per poll call "
		"(clamped to [1, SUET_CQ_MAX_BATCH=64]). (default: 16)");
	fi_param_define(&suet_prov, "max_pkt_retry", FI_PARAM_INT,
			"Maximum GBN retry rounds without ACK progress before the owning ipdc "
			"is force-closed. (default: 100)");
	fi_param_define(
		&suet_prov, "max_gtd_del_resp_pool_size", FI_PARAM_INT,
		"Maximum number of guaranteed-delivery SES responses the "
		"target may persist per domain pending CLEAR_PSN "
		"(Spec 3.5.11.4.4 / 3.5.16.3). When the pool is exhausted "
		"the target sets REQ_CLEAR on outbound ACKs. (default: 16)");
	fi_param_define(
		&suet_prov, "unexp_msg_pool_size", FI_PARAM_INT,
		"Maximum number of unexpected-message descriptors that may "
		"be buffered per domain. When the pool is exhausted the "
		"target NACKs the SOM with NO_RESOURCE. (default: 1024)");
	fi_param_define(
		&suet_prov, "zc_mr_reg_threshold", FI_PARAM_SIZE_T,
		"Send buffer size threshold (bytes) above which the TX "
		"path registers the user buffer with the underlying "
		"datagram domain for zero-copy instead of staging through "
		"internal packet buffers. (default: 16384)");

	suet_init_env();

	return &suet_prov;
}
