/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2013-2020 Intel Corporation. All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc. All rights reserved.
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
#include <inttypes.h>
#include <ofi_iov.h>
#include <stdlib.h>
#include <string.h>

/*
 * All EPs use the same underlying datagram provider, so pick any and use its
 * associated CQ.
 */
static const char *suet_cq_strerror(struct fid_cq *cq_fid, int prov_errno,
				    const void *err_data, char *buf, size_t len)
{
	struct fid_list_entry *fid_entry;
	struct util_ep *util_ep;
	struct suet_cq *cq;
	struct suet_ep *ep;
	const char *str;

	cq = container_of(cq_fid, struct suet_cq, util_cq.cq_fid);

	ofi_genlock_lock(&cq->util_cq.ep_list_lock);
	assert(!dlist_empty(&cq->util_cq.ep_list));
	fid_entry = container_of(cq->util_cq.ep_list.next,
				 struct fid_list_entry, entry);
	util_ep = container_of(fid_entry->fid, struct util_ep, ep_fid.fid);
	ep = container_of(util_ep, struct suet_ep, util_ep);

	str = fi_cq_strerror(suet_ep_domain(ep)->dgram.tx_cq, prov_errno,
			     err_data, buf, len);
	ofi_genlock_unlock(&cq->util_cq.ep_list_lock);
	return str;
}

static int suet_ses_cq_write(struct suet_cq *cq,
			     struct fi_cq_tagged_entry *cq_entry)
{
	return ofi_cq_write(&cq->util_cq, cq_entry->op_context, cq_entry->flags,
			    cq_entry->len, cq_entry->buf, cq_entry->data,
			    cq_entry->tag);
}

static int suet_ses_cq_write_signal(struct suet_cq *cq,
				    struct fi_cq_tagged_entry *cq_entry)
{
	int ret = suet_ses_cq_write(cq, cq_entry);
	cq->util_cq.wait->signal(cq->util_cq.wait);
	return ret;
}

static int suet_pds_cq_close(struct fid *fid)
{
	int ret;
	struct suet_cq *cq;

	cq = container_of(fid, struct suet_cq, util_cq.cq_fid.fid);
	ret = ofi_cq_cleanup(&cq->util_cq);
	if (ret)
		return ret;
	free(cq);
	return 0;
}

static struct fi_ops suet_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = suet_pds_cq_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

ssize_t suet_cq_sreadfrom(struct fid_cq *cq_fid, void *buf, size_t count,
			  fi_addr_t *src_addr, const void *cond, int timeout)
{
	struct util_cq *cq;
	uint64_t endtime;
	ssize_t ret;

	cq = container_of(cq_fid, struct util_cq, cq_fid);
	endtime = ofi_timeout_time(timeout);

	do {
		ret = ofi_cq_readfrom(cq_fid, buf, count, src_addr);
		if (ret != -FI_EAGAIN)
			break;

		if (ofi_adjust_timeout(endtime, &timeout))
			return -FI_EAGAIN;

		if (ofi_atomic_get32(&cq->wakeup)) {
			ofi_atomic_set32(&cq->wakeup, 0);
			return -FI_EAGAIN;
		}
	} while (1);

	return ret == -FI_ETIMEDOUT ? -FI_EAGAIN : ret;
}

ssize_t suet_cq_sread(struct fid_cq *cq_fid, void *buf, size_t count,
		      const void *cond, int timeout)
{
	return suet_cq_sreadfrom(cq_fid, buf, count, NULL, cond, timeout);
}

static struct fi_ops_cq suet_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = ofi_cq_read,
	.readfrom = ofi_cq_readfrom,
	.readerr = ofi_cq_readerr,
	.sread = suet_cq_sread,
	.sreadfrom = suet_cq_sreadfrom,
	.signal = ofi_cq_signal,
	.strerror = suet_cq_strerror,
};

int suet_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		 struct fid_cq **cq_fid, void *context)
{
	int ret;
	struct suet_cq *cq;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return -FI_ENOMEM;

	ret = ofi_cq_init(&suet_prov, domain, attr, &cq->util_cq,
			  &ofi_cq_progress, context);
	if (ret)
		goto free;

	cq->write_fn =
		cq->util_cq.wait ? suet_ses_cq_write_signal : suet_ses_cq_write;
	*cq_fid = &cq->util_cq.cq_fid;
	(*cq_fid)->fid.ops = &suet_cq_fi_ops;
	(*cq_fid)->ops = &suet_cq_ops;
	return 0;

free:
	free(cq);
	return ret;
}
