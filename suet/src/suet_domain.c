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

#include <unistd.h>
#include "suet.h"

static inline uint16_t suet_domain_get_pid_on_fep(void)
{
	return (uint16_t) (getpid() & SUET_PID_ON_FEP_MASK);
}

static struct fi_ops_domain suet_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = suet_av_create,
	.cq_open = suet_cq_open,
	.endpoint = suet_endpoint,
	.scalable_ep = fi_no_scalable_ep,
	.cntr_open = suet_cntr_open,
	.poll_open = fi_poll_create,
	.stx_ctx = fi_no_stx_context,
	.srx_ctx = fi_no_srx_context,
	.query_atomic = suet_query_atomic,
	.query_collective = fi_no_query_collective,
};

/*
 * Broadcast a synthetic fi_cq_err_entry to every EP currently bound to
 * this domain. Returns the number of CQs the error was successfully
 * written to.
 *
 * Used as the primary surface for unrecoverable dgram-level CQ errors
 * that have lost their per-op association by the time they reach us
 * (e.g. an errored pre-posted RX buffer carries no EP context). Per
 * libfabric idiom (cf. prov/efa/src/efa_base_ep.c, prov/tcp/src/xnet_ep.c),
 * abort() is the fallback only when no surface is reachable.
 */
int suet_domain_broadcast_cq_err(struct suet_domain *domain, int err,
				 bool is_tx)
{
	struct suet_ep *ep;
	struct util_cq *util_cq;
	struct fi_cq_err_entry err_entry = {
		.err = err,
		.prov_errno = err,
	};
	int written = 0;
	int i;

	if (!domain->ep_table)
		return 0;

	for (i = 0; i < suet_env.max_eps; i++) {
		ep = domain->ep_table[i];
		if (!ep)
			continue;
		util_cq = is_tx ? &suet_ep_tx_cq(ep)->util_cq :
				  &suet_ep_rx_cq(ep)->util_cq;
		if (ofi_cq_write_error(util_cq, &err_entry) == 0)
			written++;
	}
	return written;
}

void suet_domain_progress(struct suet_domain *domain)
{
	suet_dgram_progress(domain);
	suet_pds_progress(domain);
}

static int suet_domain_close(fid_t fid)
{
	int ret;
	struct suet_domain *suet_domain;

	suet_domain = container_of(fid, struct suet_domain,
				   util_domain.domain_fid.fid);

	suet_pds_drain(suet_domain);

	if (suet_env.print_domain_counters)
		FI_INFO(&suet_prov, FI_LOG_DOMAIN,
			"[SUET domain counters]: nacks_tx=%lu nacks_rx=%lu "
			"stale_acks_rx=%lu error_acks_rx=%lu tx_cq_errors=%lu "
			"rx_cq_errors=%lu pdc_closes_ok=%lu "
			"pdc_close_in_err=%lu pdc_closes_rx=%lu "
			"dup_drops=%lu ses_err_completions=%lu\n",
			(unsigned long) suet_domain->counters.nacks_tx,
			(unsigned long) suet_domain->counters.nacks_rx,
			(unsigned long) suet_domain->counters.stale_acks_rx,
			(unsigned long) suet_domain->counters.error_acks_rx,
			(unsigned long) suet_domain->counters.tx_cq_errors,
			(unsigned long) suet_domain->counters.rx_cq_errors,
			(unsigned long) suet_domain->counters.pdc_closes_ok,
			(unsigned long) suet_domain->counters.pdc_close_in_err,
			(unsigned long) suet_domain->counters.pdc_closes_rx,
			(unsigned long) suet_domain->counters.dup_drops,
			(unsigned long) suet_domain->counters.ses_err_completions);
	ret = suet_dgram_stop(suet_domain);
	if (ret)
		return ret;
	suet_pds_cleanup(suet_domain);
	suet_ses_cleanup(suet_domain);
	ret = suet_dgram_cleanup(suet_domain);
	if (ret)
		return ret;

	ret = ofi_domain_close(&suet_domain->util_domain);
	if (ret)
		return ret;

	ofi_genlock_destroy(&suet_domain->fep_lock);
	free(suet_domain->ep_table);
	free(suet_domain);
	return 0;
}

static struct fi_ops suet_domain_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = suet_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int suet_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		     struct fid_domain **domain, void *context)
{
	struct suet_domain *suet_domain;
	int ret;

	suet_domain = calloc(1, sizeof(*suet_domain));
	if (!suet_domain)
		return -FI_ENOMEM;
	ret = suet_dgram_init(suet_domain, fabric, info, context,
			      PDS_MAX_MTU_SIZE,
			      sizeof(struct suet_pds_pkt_entry),
			      sizeof(struct suet_ses_pkt_entry));
	if (ret)
		goto err;

	suet_domain->max_inline_msg =
		suet_domain->dgram.max_pkt_size - sizeof(struct pds_req_hdr);
	suet_domain->max_inline_rma = suet_domain->max_inline_msg;
	suet_domain->max_inline_atom =
		suet_domain->max_inline_rma - sizeof(struct ses_msg_amo_hdr);
	suet_domain->max_pkt_sz =
		suet_domain->dgram.max_pkt_size - sizeof(struct suet_req_pkt);
	ret = suet_ses_init(suet_domain);
	if (ret)
		goto err_dgram;

	ret = ofi_domain_init(fabric, info, &suet_domain->util_domain, context,
			      OFI_LOCK_MUTEX);
	if (ret)
		goto err_ses;

	ret = ofi_genlock_init(&suet_domain->fep_lock,
			       suet_domain->util_domain.threading !=
					       FI_THREAD_SAFE ?
				       OFI_LOCK_NOOP :
				       OFI_LOCK_MUTEX);
	if (ret)
		goto err_util;

	suet_domain->ep_table =
		calloc(suet_env.max_eps, sizeof(*suet_domain->ep_table));
	if (!suet_domain->ep_table) {
		ret = -FI_ENOMEM;
		goto err_lock;
	}

	suet_domain->pid_on_fep = suet_domain_get_pid_on_fep();
	suet_domain->next_ri = 0;

	ret = suet_pds_init(suet_domain);
	if (ret)
		goto err_lock;

	suet_domain->zc_mr_reg_threshold = suet_env.zc_mr_reg_threshold;

	*domain = &suet_domain->util_domain.domain_fid;
	(*domain)->fid.ops = &suet_domain_fi_ops;
	(*domain)->ops = &suet_domain_ops;
	(*domain)->mr = &suet_domain_mr_ops;
	return 0;

err_lock:
	free(suet_domain->ep_table);
	ofi_genlock_destroy(&suet_domain->fep_lock);
err_util:
	if (ofi_domain_close(&suet_domain->util_domain))
		FI_WARN(&suet_prov, FI_LOG_DOMAIN, "ofi_domain_close failed");
err_ses:
	suet_ses_cleanup(suet_domain);
err_dgram:
	if (suet_dgram_cleanup(suet_domain))
		FI_WARN(&suet_prov, FI_LOG_DOMAIN, "datagram cleanup failed\n");
err:
	free(suet_domain);
	return ret;
}
