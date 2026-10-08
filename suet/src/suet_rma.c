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
#include <ofi_enosys.h>
#include <ofi_iov.h>
#include <ofi_mem.h>
#include <stdlib.h>
#include <string.h>

static struct suet_ses_tx_entry *
suet_ep_tx_entry_init_rma(struct suet_ep *ep, fi_addr_t addr, uint32_t op,
			  const struct iovec *iov, void **desc,
			  size_t iov_count, uint64_t data, uint32_t flags,
			  void *context, const struct fi_rma_iov *rma_iov,
			  size_t rma_count)
{
	struct suet_ses_tx_entry *tx_entry;
	struct suet_domain *suet_domain = suet_ep_domain(ep);

	tx_entry = suet_ses_tx_entry_init_common(
		ep, addr, op, iov, desc, iov_count, 0, data, flags, context);
	if (!tx_entry)
		return NULL;

	tx_entry->hdr_len = sizeof(struct suet_req_pkt);

	if (tx_entry->cq_entry.len > (size_t) suet_domain->max_pkt_sz)
		tx_entry->num_pkts = ofi_div_ceil(tx_entry->cq_entry.len,
						      suet_domain->max_pkt_sz);
	else
		tx_entry->num_pkts = 1;

	suet_ses_init_rma_iov(rma_iov, tx_entry);

	return tx_entry;
}

static ssize_t suet_ep_generic_write_inject(struct suet_ep *suet_ep,
		const struct iovec *iov, size_t iov_count,
		const struct fi_rma_iov *rma_iov, size_t rma_count,
		fi_addr_t addr, void *context, uint32_t op, uint64_t data,
		uint32_t suet_flags)
{
	struct suet_ses_tx_entry *tx_entry;
	ssize_t ret = -FI_EAGAIN;

	if (iov_count > SUET_IOV_LIMIT || rma_count > SUET_IOV_LIMIT)
		return -FI_EINVAL;
	if (ofi_total_iov_len(iov, iov_count) >
	    (size_t) suet_ep_domain(suet_ep)->max_inline_rma)
		return -FI_EMSGSIZE;

	ofi_genlock_lock(&suet_ep_domain(suet_ep)->fep_lock);

	tx_entry = suet_ep_tx_entry_init_rma(suet_ep, addr, op, iov, NULL,
					  iov_count, data, suet_flags, context,
					  rma_iov, rma_count);
	if (!tx_entry) {
		ret = -FI_EAGAIN;
		goto out;
	}

	suet_ses_submit(tx_entry);
	ret = 0;

out:
	ofi_genlock_unlock(&suet_ep_domain(suet_ep)->fep_lock);
	return ret;
}

static ssize_t
suet_ep_generic_rma(struct suet_ep *suet_ep, const struct iovec *iov,
	size_t iov_count, const struct fi_rma_iov *rma_iov, size_t rma_count,
	void **desc, fi_addr_t addr, void *context, uint32_t op, uint64_t data,
	uint32_t suet_flags)
{
	struct suet_ses_tx_entry *tx_entry;
	ssize_t ret = -FI_EAGAIN;

	if (suet_flags & SUET_INJECT)
		return suet_ep_generic_write_inject(suet_ep, iov, iov_count, rma_iov,
						rma_count, addr, context, op,
						data, suet_flags);

	if (iov_count > SUET_IOV_LIMIT || rma_count > SUET_IOV_LIMIT)
		return -FI_EINVAL;

	ret = suet_normalize_mr_desc(desc, iov_count, &desc);
	if (ret)
		return ret;
	ret = -FI_EAGAIN;

	ofi_genlock_lock(&suet_ep_domain(suet_ep)->fep_lock);

	if (ofi_cirque_isfull(suet_ep->util_ep.tx_cq->cirq))
		goto out;

	tx_entry = suet_ep_tx_entry_init_rma(suet_ep, addr, op, iov, desc,
					  iov_count, data, suet_flags, context,
					  rma_iov, rma_count);
	if (!tx_entry) {
		ret = -FI_EAGAIN;
		goto out;
	}

	suet_ses_submit(tx_entry);
	ret = 0;

out:
	ofi_genlock_unlock(&suet_ep_domain(suet_ep)->fep_lock);
	return ret;
}

static ssize_t
suet_write(struct fid_ep *ep_fid, const void *buf, size_t len, void *desc,
	fi_addr_t dest_addr, uint64_t addr, uint64_t key, void *context)
{
	struct suet_ep *ep;
	struct iovec msg_iov;
	struct fi_rma_iov rma_iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	msg_iov.iov_base = (void *) buf;
	msg_iov.iov_len = len;
	rma_iov.addr = addr;
	rma_iov.len = len;
	rma_iov.key = key;

	return suet_ep_generic_rma(ep, &msg_iov, 1, &rma_iov, 1, &desc,
			       dest_addr, context, ofi_op_write, 0,
			       ep->tx_flags);
}

static ssize_t
suet_writev(struct fid_ep *ep_fid, const struct iovec *iov, void **desc,
		size_t count, fi_addr_t dest_addr, uint64_t addr, uint64_t key,
		void *context)
{
	struct suet_ep *ep;
	struct fi_rma_iov rma_iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	rma_iov.addr = addr;
	rma_iov.len  = ofi_total_iov_len(iov, count);
	rma_iov.key = key;

	return suet_ep_generic_rma(ep, iov, count, &rma_iov, 1, desc,
			       dest_addr, context, ofi_op_write, 0,
			       ep->tx_flags);
}

static ssize_t
suet_writemsg(struct fid_ep *ep_fid, const struct fi_msg_rma *msg,
	uint64_t flags)
{
	struct suet_ep *ep;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	return suet_ep_generic_rma(ep, msg->msg_iov, msg->iov_count,
			       msg->rma_iov, msg->rma_iov_count,
			       msg->desc, msg->addr, msg->context,
			       ofi_op_write, msg->data, suet_ep_tx_flags(flags |
			       ep->util_ep.tx_msg_flags));
}

static ssize_t
suet_writedata(struct fid_ep *ep_fid, const void *buf, size_t len,
		      void *desc, uint64_t data, fi_addr_t dest_addr,
		      uint64_t addr, uint64_t key, void *context)
{
	struct suet_ep *ep;
	struct iovec iov;
	struct fi_rma_iov rma_iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;
	rma_iov.addr = addr;
	rma_iov.len  = len;
	rma_iov.key = key;

	return suet_ep_generic_rma(ep, &iov, 1, &rma_iov, 1, &desc,
			       dest_addr, context, ofi_op_write, data,
			       ep->tx_flags | SUET_REMOTE_CQ_DATA);
}

static ssize_t
suet_inject_write(struct fid_ep *ep_fid, const void *buf,
	size_t len, fi_addr_t dest_addr, uint64_t addr, uint64_t key)
{
	struct suet_ep *suet_ep;
	struct iovec iov;
	struct fi_rma_iov rma_iov;

	suet_ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;
	rma_iov.addr = addr;
	rma_iov.len = len;
	rma_iov.key = key;

	return suet_ep_generic_write_inject(suet_ep, &iov, 1, &rma_iov, 1,
					dest_addr, NULL, ofi_op_write, 0,
					SUET_NO_TX_COMP | SUET_INJECT);
}

static ssize_t
suet_inject_writedata(struct fid_ep *ep_fid, const void *buf, size_t len,
		     uint64_t data, fi_addr_t dest_addr, uint64_t addr,
		     uint64_t key)
{
	struct suet_ep *suet_ep;
	struct iovec iov;
	struct fi_rma_iov rma_iov;

	suet_ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;
	rma_iov.addr = addr;
	rma_iov.len = len;
	rma_iov.key = key;

	return suet_ep_generic_write_inject(suet_ep, &iov, 1, &rma_iov, 1,
					dest_addr, NULL, ofi_op_write,
					data, SUET_NO_TX_COMP | SUET_INJECT |
					SUET_REMOTE_CQ_DATA);
}

struct fi_ops_rma suet_ops_rma = {
	.size = sizeof(struct fi_ops_rma),
	.read = fi_no_rma_read,
	.readv = fi_no_rma_readv,
	.readmsg = fi_no_rma_readmsg,
	.write = suet_write,
	.writev = suet_writev,
	.writemsg = suet_writemsg,
	.inject = suet_inject_write,
	.writedata = suet_writedata,
	.injectdata = suet_inject_writedata,
};
