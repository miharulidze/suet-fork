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

#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "ofi_iov.h"
#include "suet.h"
#include "suet_ses.h"
#include <ofi_enosys.h>

static struct suet_ses_tx_entry *
suet_tx_entry_init_atomic(struct suet_ep *ep, fi_addr_t addr, uint32_t op,
			  const struct iovec *iov, size_t iov_count,
			  uint64_t data, uint32_t flags, void *context,
			  const struct fi_rma_iov *rma_iov, size_t rma_count,
			  enum fi_datatype datatype, enum fi_op atomic_op)
{
	struct suet_ses_tx_entry *tx_entry;

	tx_entry = suet_ses_tx_entry_init_common(
		ep, addr, op, iov, NULL, iov_count, 0, data, flags, context);
	if (!tx_entry)
		return NULL;

	tx_entry->hdr_len = sizeof(struct suet_amo_pkt);
	tx_entry->num_pkts = 1;

	suet_ses_init_rma_iov(rma_iov, tx_entry);

	tx_entry->cached_hdr.amo.atomic_opcode = fi_op_to_ses_amo(atomic_op);
	tx_entry->cached_hdr.amo.atomic_datatype =
		fi_datatype_to_ses_type(datatype);
	tx_entry->cached_hdr.amo.semantic_control = 0;
	tx_entry->cached_hdr.amo.reserved = 0;

	return tx_entry;
}

static ssize_t suet_ep_generic_atomic(struct suet_ep *suet_ep,
			const struct fi_ioc *ioc, void **desc, size_t count,
			fi_addr_t addr, const struct fi_rma_ioc *rma_ioc,
			size_t rma_count, uint64_t data, enum fi_datatype datatype,
			enum fi_op atomic_op, void *context, uint32_t op,
			uint32_t suet_flags)
{
	struct suet_ses_tx_entry *tx_entry;
	struct iovec iov[SUET_IOV_LIMIT];
	struct fi_rma_iov rma_iov[SUET_IOV_LIMIT];
	size_t max_inline_atom;
	ssize_t ret = -FI_EAGAIN;

	if (count > SUET_IOV_LIMIT || rma_count > SUET_IOV_LIMIT)
		return -FI_EINVAL;

	ofi_ioc_to_iov(ioc, iov, count, ofi_datatype_size(datatype));

	max_inline_atom = (size_t) suet_ep_domain(suet_ep)->max_inline_atom;
	if (ofi_total_iov_len(iov, count) > max_inline_atom)
		return -FI_EMSGSIZE;

	ofi_rma_ioc_to_iov(rma_ioc, rma_iov, rma_count, ofi_datatype_size(datatype));

	ofi_genlock_lock(&suet_ep_domain(suet_ep)->fep_lock);

	if (ofi_cirque_isfull(suet_ep->util_ep.tx_cq->cirq))
		goto out;

	tx_entry = suet_tx_entry_init_atomic(suet_ep, addr, op, iov, count,
			data, suet_flags, context, rma_iov, rma_count,
			datatype, atomic_op);
	if (!tx_entry)
		goto out;

	suet_ses_submit(tx_entry);
	ret = 0;

out:
	ofi_genlock_unlock(&suet_ep_domain(suet_ep)->fep_lock);
	return ret;
}

static ssize_t suet_ep_atomic_writemsg(struct fid_ep *ep_fid,
			const struct fi_msg_atomic *msg, uint64_t flags)
{
	struct suet_ep *ep;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	return suet_ep_generic_atomic(ep, msg->msg_iov, msg->desc, msg->iov_count,
				  msg->addr, msg->rma_iov, msg->rma_iov_count,
				  msg->data, msg->datatype, msg->op, msg->context,
				  ofi_op_atomic, suet_ep_tx_flags(flags |
				  ep->util_ep.tx_msg_flags));
}

static ssize_t suet_ep_atomic_writev(struct fid_ep *ep_fid,
			const struct fi_ioc *iov, void **desc, size_t count,
			fi_addr_t dest_addr, uint64_t addr, uint64_t key,
			enum fi_datatype datatype, enum fi_op op, void *context)
{
	struct suet_ep *ep;
	struct fi_rma_ioc rma_iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	rma_iov.addr = addr;
	rma_iov.count = ofi_total_ioc_cnt(iov, count);
	rma_iov.key = key;

	return suet_ep_generic_atomic(ep, iov, desc, count, dest_addr, &rma_iov,
				  1, 0, datatype, op, context, ofi_op_atomic,
				  ep->tx_flags);
}

static ssize_t suet_ep_atomic_write(struct fid_ep *ep_fid, const void *buf, size_t count,
			void *desc, fi_addr_t dest_addr, uint64_t addr,
			uint64_t key, enum fi_datatype datatype, enum fi_op op,
			void *context)
{
	struct fi_ioc iov;
	struct fi_rma_ioc rma_iov;
	struct suet_ep *ep;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.addr = (void *) buf;
	iov.count = count;

	rma_iov.addr = addr;
	rma_iov.count = count;
	rma_iov.key = key;

	return suet_ep_generic_atomic(ep, &iov, &desc, 1, dest_addr, &rma_iov, 1,
				  0, datatype, op, context, ofi_op_atomic,
				  ep->tx_flags);
}

static ssize_t suet_ep_atomic_inject(struct fid_ep *ep_fid, const void *buf,
			size_t count, fi_addr_t dest_addr, uint64_t addr,
			uint64_t key, enum fi_datatype datatype, enum fi_op op)
{
	struct suet_ep *suet_ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);
	struct suet_ses_tx_entry *tx_entry;
	struct iovec iov;
	struct fi_rma_iov rma_iov;
	ssize_t ret = -FI_EAGAIN;

	iov.iov_base = (void *) buf;
	iov.iov_len = count * ofi_datatype_size(datatype);
	if (iov.iov_len > (size_t) suet_ep_domain(suet_ep)->max_inline_atom)
		return -FI_EMSGSIZE;

	rma_iov.addr = addr;
	rma_iov.len = count * ofi_datatype_size(datatype);
	rma_iov.key = key;

	ofi_genlock_lock(&suet_ep_domain(suet_ep)->fep_lock);

	if (ofi_cirque_isfull(suet_ep->util_ep.tx_cq->cirq))
		goto out;

	tx_entry = suet_tx_entry_init_atomic(suet_ep, dest_addr, ofi_op_atomic, &iov, 1,
			0, SUET_INJECT | SUET_NO_TX_COMP, NULL, &rma_iov, 1,
			datatype, op);
	if (!tx_entry)
		goto out;

	suet_ses_submit(tx_entry);
	ret = 0;
out:
	ofi_genlock_unlock(&suet_ep_domain(suet_ep)->fep_lock);
	return ret;
}

int suet_query_atomic(struct fid_domain *domain, enum fi_datatype datatype,
		     enum fi_op op, struct fi_atomic_attr *attr, uint64_t flags)
{
	struct suet_domain *suet_domain;
	int ret;
	size_t total_size;

	if (flags & FI_TAGGED) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"tagged atomic op not supported\n");
		return -FI_EOPNOTSUPP;
	}

	if ((datatype == FI_INT128) || (datatype == FI_UINT128)) {
		FI_WARN(&suet_prov, FI_LOG_EP_CTRL,
			"128-bit integers not supported\n");
		return -FI_EOPNOTSUPP;
	}

	ret = ofi_atomic_valid(&suet_prov, datatype, op, flags);
	if (ret || !attr)
		return ret;

	suet_domain = container_of(domain, struct suet_domain,
				  util_domain.domain_fid);
	attr->size = ofi_datatype_size(datatype);
	if (!attr->size)
		return -FI_EOPNOTSUPP;

	total_size = (flags & FI_COMPARE_ATOMIC) ?
		     suet_domain->max_inline_atom / 2 :
		     suet_domain->max_inline_atom;
	attr->count = total_size / attr->size;

	return ret;
}

static int suet_ep_atomic_valid(struct fid_ep *ep, enum fi_datatype datatype,
			    enum fi_op op, size_t *count)
{
	struct fi_atomic_attr attr;
	int ret;

	ret = suet_query_atomic(&(container_of(ep,
			struct util_ep, ep_fid))->domain->domain_fid,
			datatype, op, &attr, 0);
	if (!ret)
		*count = attr.count;

	return ret;
}

struct fi_ops_atomic suet_ops_atomic = {
	.size = sizeof(struct fi_ops_atomic),
	.write = suet_ep_atomic_write,
	.writev = suet_ep_atomic_writev,
	.writemsg = suet_ep_atomic_writemsg,
	.inject = suet_ep_atomic_inject,
	.readwrite = fi_no_atomic_readwrite,
	.readwritev = fi_no_atomic_readwritev,
	.readwritemsg = fi_no_atomic_readwritemsg,
	.compwrite = fi_no_atomic_compwrite,
	.compwritev = fi_no_atomic_compwritev,
	.compwritemsg = fi_no_atomic_compwritemsg,
	.writevalid = suet_ep_atomic_valid,
	.readwritevalid = fi_no_atomic_readwritevalid,
	.compwritevalid = fi_no_atomic_compwritevalid,
};
