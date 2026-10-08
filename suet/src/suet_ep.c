/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2013-2018 Intel Corporation. All rights reserved.
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
#include "suet_ses.h"
#include <ofi_iov.h>
#include <ofi_mem.h>
#include <stdlib.h>
#include <string.h>

static int suet_ep_match_ctx(struct dlist_entry *item, const void *arg)
{
	struct suet_ses_rx_entry *rx_entry;

	rx_entry = container_of(item, struct suet_ses_rx_entry, entry);

	return (rx_entry->cq_entry.op_context == arg);
}

static ssize_t suet_ep_cancel_recv(struct suet_ep *ep, struct dlist_entry *list,
				   void *context)
{
	struct dlist_entry *entry;
	struct suet_ses_rx_entry *rx_entry;
	struct fi_cq_err_entry err_entry;
	int ret = 0;

	ofi_genlock_lock(&suet_ep_domain(ep)->fep_lock);

	entry = dlist_remove_first_match(list, &suet_ep_match_ctx, context);
	if (!entry)
		goto out;

	rx_entry = container_of(entry, struct suet_ses_rx_entry, entry);
	memset(&err_entry, 0, sizeof(struct fi_cq_err_entry));
	err_entry.op_context = rx_entry->cq_entry.op_context;
	err_entry.flags = rx_entry->cq_entry.flags;
	err_entry.err = FI_ECANCELED;
	err_entry.prov_errno = 0;
	ret = ofi_cq_write_error(&suet_ep_rx_cq(ep)->util_cq, &err_entry);
	if (ret) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"could not write error entry\n");
		goto out;
	}
	suet_ses_rx_entry_free(rx_entry);
	ret = 1;
out:
	ofi_genlock_unlock(&suet_ep_domain(ep)->fep_lock);
	return ret;
}

static ssize_t suet_ep_cancel(fid_t fid, void *context)
{
	struct suet_ep *ep;
	ssize_t ret;

	ep = container_of(fid, struct suet_ep, util_ep.ep_fid);

	ret = suet_ep_cancel_recv(ep, &ep->rx_tag_list, context);
	if (ret)
		goto out;

	ret = suet_ep_cancel_recv(ep, &ep->rx_list, context);

out:
	return 0;
}

static int suet_ep_getopt(fid_t fid, int level, int optname, void *optval,
			  size_t *optlen)
{
	struct suet_ep *suet_ep =
		container_of(fid, struct suet_ep, util_ep.ep_fid);

	if ((level != FI_OPT_ENDPOINT) || (optname != FI_OPT_MIN_MULTI_RECV))
		return -FI_ENOPROTOOPT;

	*(size_t *) optval = suet_ep->min_multi_recv_size;
	*optlen = sizeof(size_t);

	return FI_SUCCESS;
}

static int suet_ep_setopt(fid_t fid, int level, int optname, const void *optval,
			  size_t optlen)
{
	struct suet_ep *suet_ep =
		container_of(fid, struct suet_ep, util_ep.ep_fid);

	if ((level != FI_OPT_ENDPOINT) || (optname != FI_OPT_MIN_MULTI_RECV))
		return -FI_ENOPROTOOPT;

	suet_ep->min_multi_recv_size = *(size_t *) optval;

	return FI_SUCCESS;
}

struct fi_ops_ep suet_ops_ep = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = suet_ep_cancel,
	.getopt = suet_ep_getopt,
	.setopt = suet_ep_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

static int suet_ep_bind(struct fid *ep_fid, struct fid *bfid, uint64_t flags)
{
	struct suet_ep *ep;
	struct suet_av *av;
	struct util_cq *cq;
	struct util_cntr *cntr;
	int ret = 0;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	switch (bfid->fclass) {
	case FI_CLASS_AV:
		av = container_of(bfid, struct suet_av, util_av.av_fid.fid);
		ret = ofi_ep_bind_av(&ep->util_ep, &av->util_av);
		break;
	case FI_CLASS_CQ:
		cq = container_of(bfid, struct util_cq, cq_fid.fid);
		ret = ofi_ep_bind_cq(&ep->util_ep, cq, flags);
		break;
	case FI_CLASS_EQ:
		break;
	case FI_CLASS_CNTR:
		cntr = container_of(bfid, struct util_cntr, cntr_fid.fid);
		ret = ofi_ep_bind_cntr(&ep->util_ep, cntr, flags);
		break;
	default:
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL, "invalid fid class\n");
		ret = -FI_EINVAL;
		break;
	}
	return ret;
}

static int suet_ep_control(struct fid *fid, int command, void *arg)
{
	struct suet_ep *ep;
	switch (command) {
	case FI_ENABLE:
		ep = container_of(fid, struct suet_ep, util_ep.ep_fid.fid);
		ep->tx_flags = suet_ep_tx_flags(ep->util_ep.tx_op_flags);
		ep->rx_flags = suet_ep_rx_flags(ep->util_ep.rx_op_flags);
		return 0;
	default:
		return -FI_ENOSYS;
	}
}

static int suet_ep_cm_setname(fid_t fid, void *raw_addr, size_t addrlen)
{
	struct suet_ep *ep =
		container_of(fid, struct suet_ep, util_ep.ep_fid.fid);
	const struct suet_av_addr *addr =
		(const struct suet_av_addr *) raw_addr;

	if (addrlen < sizeof(struct suet_av_addr))
		return -FI_ETOOSMALL;

	return suet_dgram_setname(suet_ep_domain(ep),
				  (void *) addr->raw_dgram_addr,
				  addr->raw_dgram_addrlen);
}

static int suet_ep_cm_getname(fid_t fid, void *raw_addr, size_t *addrlen)
{
	struct suet_ep *ep =
		container_of(fid, struct suet_ep, util_ep.ep_fid.fid);
	struct suet_av_addr *addr = (struct suet_av_addr *) raw_addr;
	uint8_t raw_dgram_addr[SUET_DGRAM_AV_NAME_LENGTH];
	size_t raw_dgram_addrlen = sizeof(raw_dgram_addr);
	int ret;

	if (*addrlen < sizeof(struct suet_av_addr)) {
		*addrlen = sizeof(struct suet_av_addr);
		return -FI_ETOOSMALL;
	}

	ret = suet_dgram_getname(suet_ep_domain(ep), raw_dgram_addr,
				 &raw_dgram_addrlen);
	if (ret && ret != -FI_ETOOSMALL)
		return ret;

	memset(addr, 0, sizeof(*addr));
	addr->version = SUET_ADDR_VERSION;
	addr->pid_on_fep = ep->pid_on_fep & SUET_PID_ON_FEP_MASK;
	addr->start_ri = ep->resource_index & SUET_RI_MASK;
	addr->num_ri = 1;
	addr->raw_dgram_addrlen =
		(uint16_t) MIN(raw_dgram_addrlen, sizeof(raw_dgram_addr));
	memcpy(addr->raw_dgram_addr, raw_dgram_addr, addr->raw_dgram_addrlen);

	*addrlen = sizeof(struct suet_av_addr);
	return 0;
}

struct fi_ops_cm suet_ep_cm = {
	.size = sizeof(struct fi_ops_cm),
	.setname = suet_ep_cm_setname,
	.getname = suet_ep_cm_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_no_listen,
	.accept = fi_no_accept,
	.reject = fi_no_reject,
	.shutdown = fi_no_shutdown,
	.join = fi_no_join,
};

static void suet_ep_progress(struct util_ep *util_ep)
{
	struct suet_ep *ep = container_of(util_ep, struct suet_ep, util_ep);
	struct suet_domain *domain = suet_ep_domain(ep);

	ofi_genlock_lock(&domain->fep_lock);
	suet_domain_progress(domain);
	ofi_genlock_unlock(&domain->fep_lock);
}

static int suet_ep_close(struct fid *fid)
{
	struct suet_ep *ep;
	struct suet_domain *domain;
	struct suet_ses_rx_entry *rx_entry;
	struct dlist_entry *tmp;

	ep = container_of(fid, struct suet_ep, util_ep.ep_fid.fid);
	domain = suet_ep_domain(ep);

	ofi_genlock_lock(&domain->fep_lock);

	if (ep->resource_index < suet_env.max_eps)
		domain->ep_table[ep->resource_index] = NULL;

	suet_ses_ep_cleanup(ep);

	/* Posted-but-unmatched rx_entries on the EP's own lists also come
	 * from the SES RX entry pool. Free them before the pool is destroyed.
	 */
	dlist_foreach_container_safe (&ep->rx_list, struct suet_ses_rx_entry,
				      rx_entry, entry, tmp)
		suet_ses_rx_entry_free(rx_entry);
	dlist_foreach_container_safe (&ep->rx_tag_list,
				      struct suet_ses_rx_entry, rx_entry, entry,
				      tmp)
		suet_ses_rx_entry_free(rx_entry);

	ofi_genlock_unlock(&domain->fep_lock);

	ofi_endpoint_close(&ep->util_ep);

	suet_ses_unexp_msg_list_cleanup(&ep->unexp_list);
	suet_ses_unexp_msg_list_cleanup(&ep->unexp_tag_list);

	free(ep);
	return 0;
}

static int suet_ep_init_res(struct suet_ep *ep, struct fi_info *fi_info)
{
	dlist_init(&ep->tx_list);
	dlist_init(&ep->rx_list);
	dlist_init(&ep->rx_tag_list);
	dlist_init(&ep->unexp_list);
	dlist_init(&ep->unexp_tag_list);

	return 0;
}

static struct fi_ops suet_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = suet_ep_close,
	.bind = suet_ep_bind,
	.control = suet_ep_control,
	.ops_open = fi_no_ops_open,
};

int suet_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct fid_ep **ep, void *context)
{
	struct suet_domain *suet_domain;
	struct suet_ep *suet_ep;
	int ret;

	suet_ep = calloc(1, sizeof(*suet_ep));
	if (!suet_ep)
		return -FI_ENOMEM;

	suet_domain = container_of(domain, struct suet_domain,
				   util_domain.domain_fid);

	ret = ofi_endpoint_init(domain, &suet_util_prov, info,
				&suet_ep->util_ep, context, suet_ep_progress);
	if (ret)
		goto err1;

	ret = suet_dgram_ep_sizes(suet_domain, info, &suet_ep->tx_size,
				  &suet_ep->rx_size);
	if (ret)
		goto err2;

	/* Select delivery from the endpoint ordering contract, as in the
	 * reference provider. All currently advertised ordering bits require
	 * ordered delivery (including the legacy FI_ORDER_WAW bit).
	 */
	suet_ep->pdc_type =
		info->tx_attr->msg_order ? SUET_PDC_ROD : SUET_PDC_RUD;
	suet_ep->pid_on_fep = suet_domain->pid_on_fep;

	ofi_genlock_lock(&suet_domain->fep_lock);
	suet_ep->resource_index = suet_domain->next_ri++;
	if (suet_ep->resource_index >= suet_env.max_eps) {
		ofi_genlock_unlock(&suet_domain->fep_lock);
		ret = -FI_ENOSPC;
		goto err2;
	}
	suet_domain->ep_table[suet_ep->resource_index] = suet_ep;
	ofi_genlock_unlock(&suet_domain->fep_lock);

	ret = suet_ep_init_res(suet_ep, info);
	if (ret)
		goto err2;

	suet_ep->util_ep.ep_fid.fid.ops = &suet_ep_fi_ops;
	suet_ep->util_ep.ep_fid.cm = &suet_ep_cm;
	suet_ep->util_ep.ep_fid.ops = &suet_ops_ep;
	suet_ep->util_ep.ep_fid.msg = &suet_ops_msg;
	suet_ep->util_ep.ep_fid.tagged = &suet_ops_tagged;
	suet_ep->util_ep.ep_fid.rma = &suet_ops_rma;
	suet_ep->util_ep.ep_fid.atomic = &suet_ops_atomic;

	*ep = &suet_ep->util_ep.ep_fid;
	return 0;

err2:
	ofi_endpoint_close(&suet_ep->util_ep);
err1:
	free(suet_ep);
	return ret;
}
