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

ssize_t suet_ep_generic_recvmsg(struct suet_ep *suet_ep,
				const struct iovec *iov, size_t iov_count,
				fi_addr_t addr, uint64_t tag, uint64_t ignore,
				void *context, uint32_t op, uint32_t suet_flags,
				uint64_t flags)
{
	ssize_t ret = 0;
	struct suet_ses_rx_entry *rx_entry;
	struct dlist_entry *unexp_list, *rx_list;
	struct suet_ses_unexp_msg *unexp_msg;
	int peer_idx = SUET_ADDR_INVALID;

	if (iov_count > SUET_IOV_LIMIT)
		return -FI_EINVAL;
	if ((suet_flags & SUET_MULTI_RECV) && iov_count != 1)
		return -FI_EINVAL;
	if ((flags & FI_PEEK) && op != ofi_op_tagged)
		return -FI_EINVAL;

	ofi_genlock_lock(&suet_ep_domain(suet_ep)->fep_lock);

	if (ofi_cirque_isfull(suet_ep->util_ep.rx_cq->cirq)) {
		ret = -FI_EAGAIN;
		goto out;
	}

	if (op == ofi_op_tagged) {
		unexp_list = &suet_ep->unexp_tag_list;
		rx_list = &suet_ep->rx_tag_list;
	} else {
		unexp_list = &suet_ep->unexp_list;
		rx_list = &suet_ep->rx_list;
	}

	if (suet_ep->util_ep.caps & FI_DIRECTED_RECV &&
	    addr != FI_ADDR_UNSPEC) {
		peer_idx = suet_av_peer_idx_from_usr_av_addr(
			suet_ep_av(suet_ep), addr);
	}

	if (flags & FI_PEEK) {
		ret = suet_ses_peek_recv(suet_ep, peer_idx, tag, ignore, context,
					flags, unexp_list);
		goto out;
	}
	if (!(flags & FI_DISCARD)) {
		rx_entry =
			suet_ses_rx_entry_init(suet_ep, iov, iov_count, tag, ignore,
					   context, peer_idx, op, suet_flags);
		if (!rx_entry) {
			ret = -FI_EAGAIN;
		} else if (flags & FI_CLAIM) {
			FI_DBG(&suet_prov, FI_LOG_EP_CTRL,
			       "Claiming message\n");
			unexp_msg = (struct suet_ses_unexp_msg
					     *) (((struct fi_context *) context)
							 ->internal[0]);
			suet_ses_complete_unexp_msg(suet_ep, rx_entry,
						   unexp_msg);
		} else if (!suet_ses_progress_unexp_list(suet_ep, unexp_list,
							rx_list, rx_entry)) {
			dlist_insert_tail(&rx_entry->entry, rx_list);
		}
		goto out;
	}

	/* FI_DISCARD requires FI_CLAIM per libfabric semantics. */
	if (!(flags & FI_CLAIM)) {
		ret = -FI_EINVAL;
		goto out;
	}
	FI_DBG(&suet_prov, FI_LOG_EP_CTRL, "Discarding message\n");
	unexp_msg =
		(struct suet_ses_unexp_msg *) (((struct fi_context *) context)
						       ->internal[0]);
	ret = suet_ses_discard_recv(suet_ep, context, unexp_msg);

out:
	ofi_genlock_unlock(&suet_ep_domain(suet_ep)->fep_lock);
	return ret;
}

static ssize_t suet_ep_recvmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			       uint64_t flags)
{
	struct suet_ep *ep;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	return suet_ep_generic_recvmsg(
		ep, msg->msg_iov, msg->iov_count, msg->addr, 0, ~0ULL,
		msg->context, ofi_op_msg,
		suet_ep_rx_flags(flags | ep->util_ep.rx_msg_flags), flags);
}

static ssize_t suet_ep_recv(struct fid_ep *ep_fid, void *buf, size_t len,
			    void *desc, fi_addr_t src_addr, void *context)
{
	struct suet_ep *ep;
	struct iovec msg_iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	msg_iov.iov_base = buf;
	msg_iov.iov_len = len;

	return suet_ep_generic_recvmsg(ep, &msg_iov, 1, src_addr, 0, ~0ULL,
				       context, ofi_op_msg, ep->rx_flags, 0);
}

static ssize_t suet_ep_recvv(struct fid_ep *ep_fid, const struct iovec *iov,
			     void **desc, size_t count, fi_addr_t src_addr,
			     void *context)
{
	struct suet_ep *ep;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	return suet_ep_generic_recvmsg(ep, iov, count, src_addr, 0, ~0ULL,
				       context, ofi_op_msg, ep->rx_flags, 0);
}

static struct suet_ses_tx_entry *
suet_ep_tx_entry_init_msg(struct suet_ep *ep, fi_addr_t addr, uint32_t op,
			  const struct iovec *iov, void **desc,
			  size_t iov_count, uint64_t tag, uint64_t data,
			  uint32_t suet_flags, void *context)
{
	struct suet_ses_tx_entry *tx_entry;
	struct suet_domain *suet_domain = suet_ep_domain(ep);

	tx_entry = suet_ses_tx_entry_init_common(ep, addr, op, iov, desc,
						 iov_count, tag, data,
						 suet_flags, context);
	if (!tx_entry)
		return NULL;

	tx_entry->hdr_len = sizeof(struct ses_req_hdr);

	if (tx_entry->cq_entry.len > (size_t) suet_domain->max_pkt_sz) {
		tx_entry->num_pkts = ofi_div_ceil(tx_entry->cq_entry.len,
						      suet_domain->max_pkt_sz);
	} else {
		tx_entry->num_pkts = 1;
	}

	return tx_entry;
}

ssize_t suet_ep_generic_inject(struct suet_ep *suet_ep, const struct iovec *iov,
			       size_t iov_count, fi_addr_t addr, uint64_t tag,
			       uint64_t data, uint32_t op, uint32_t suet_flags)
{
	struct suet_ses_tx_entry *tx_entry;
	ssize_t ret = -FI_EAGAIN;

	if (iov_count > SUET_IOV_LIMIT)
		return -FI_EINVAL;
	if (ofi_total_iov_len(iov, iov_count) >
	    (size_t) suet_ep_domain(suet_ep)->max_inline_msg)
		return -FI_EMSGSIZE;

	ofi_genlock_lock(&suet_ep_domain(suet_ep)->fep_lock);

	tx_entry =
		suet_ep_tx_entry_init_msg(suet_ep, addr, op, iov, NULL, iov_count,
				       tag, data, suet_flags, NULL);
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

ssize_t suet_ep_generic_sendmsg(struct suet_ep *suet_ep,
				const struct iovec *iov, void **desc,
				size_t iov_count, fi_addr_t addr, uint64_t tag,
				uint64_t data, void *context, uint32_t op,
				uint32_t suet_flags)
{
	struct suet_ses_tx_entry *tx_entry;
	ssize_t ret = -FI_EAGAIN;

	if (iov_count > SUET_IOV_LIMIT)
		return -FI_EINVAL;

	if (suet_flags & SUET_INJECT)
		return suet_ep_generic_inject(suet_ep, iov, iov_count, addr,
					      tag, 0, op, suet_flags);

	ret = suet_normalize_mr_desc(desc, iov_count, &desc);
	if (ret)
		return ret;
	ret = -FI_EAGAIN;

	ofi_genlock_lock(&suet_ep_domain(suet_ep)->fep_lock);

	if (ofi_cirque_isfull(suet_ep->util_ep.tx_cq->cirq))
		goto out;

	tx_entry =
		suet_ep_tx_entry_init_msg(suet_ep, addr, op, iov, desc, iov_count,
				       tag, data, suet_flags, context);
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

static ssize_t suet_ep_sendmsg(struct fid_ep *ep_fid, const struct fi_msg *msg,
			       uint64_t flags)
{
	struct suet_ep *ep;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	return suet_ep_generic_sendmsg(
		ep, msg->msg_iov, msg->desc, msg->iov_count, msg->addr, 0,
		msg->data, msg->context, ofi_op_msg,
		suet_ep_tx_flags(flags | ep->util_ep.tx_msg_flags));
}

static ssize_t suet_ep_sendv(struct fid_ep *ep_fid, const struct iovec *iov,
			     void **desc, size_t count, fi_addr_t dest_addr,
			     void *context)
{
	struct suet_ep *ep;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	return suet_ep_generic_sendmsg(ep, iov, desc, count, dest_addr, 0, 0,
				       context, ofi_op_msg, ep->tx_flags);
}

static ssize_t suet_ep_send(struct fid_ep *ep_fid, const void *buf, size_t len,
			    void *desc, fi_addr_t dest_addr, void *context)
{
	struct suet_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return suet_ep_generic_sendmsg(ep, &iov, &desc, 1, dest_addr, 0, 0,
				       context, ofi_op_msg, ep->tx_flags);
}

static ssize_t suet_ep_inject(struct fid_ep *ep_fid, const void *buf,
			      size_t len, fi_addr_t dest_addr)
{
	struct suet_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return suet_ep_generic_inject(ep, &iov, 1, dest_addr, 0, 0, ofi_op_msg,
				      SUET_NO_TX_COMP | SUET_INJECT);
}

static ssize_t suet_ep_senddata(struct fid_ep *ep_fid, const void *buf,
				size_t len, void *desc, uint64_t data,
				fi_addr_t dest_addr, void *context)
{
	struct suet_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return suet_ep_generic_sendmsg(ep, &iov, &desc, 1, dest_addr, 0, data,
				       context, ofi_op_msg,
				       ep->tx_flags | SUET_REMOTE_CQ_DATA);
}

static ssize_t suet_ep_injectdata(struct fid_ep *ep_fid, const void *buf,
				  size_t len, uint64_t data,
				  fi_addr_t dest_addr)
{
	struct suet_ep *ep;
	struct iovec iov;

	ep = container_of(ep_fid, struct suet_ep, util_ep.ep_fid.fid);

	iov.iov_base = (void *) buf;
	iov.iov_len = len;

	return suet_ep_generic_inject(
		ep, &iov, 1, dest_addr, 0, data, ofi_op_msg,
		SUET_NO_TX_COMP | SUET_INJECT | SUET_REMOTE_CQ_DATA);
}

struct fi_ops_msg suet_ops_msg = {
	.size = sizeof(struct fi_ops_msg),
	.recv = suet_ep_recv,
	.recvv = suet_ep_recvv,
	.recvmsg = suet_ep_recvmsg,
	.send = suet_ep_send,
	.sendv = suet_ep_sendv,
	.sendmsg = suet_ep_sendmsg,
	.inject = suet_ep_inject,
	.senddata = suet_ep_senddata,
	.injectdata = suet_ep_injectdata,
};
