/*
 * SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
 *
 * Copyright (c) 2016 Intel Corporation. All rights reserved.
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

#include "suet.h"

/*
 * TODO: FI_WAIT_FD support
 *
 * Until that is implemented, we return -FI_EAGAIN unconditionally so that
 * applications fall back to polling, which is correct for PROGRESS_MANUAL.
 */
static int suet_trywait(struct fid_fabric *fabric, struct fid **fids, int count)
{
	return -FI_EAGAIN;
}

static struct fi_ops_fabric suet_fabric_ops = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = &suet_domain_open,
	.passive_ep = fi_no_passive_ep,
	.eq_open = ofi_eq_create,
	.wait_open = ofi_wait_fd_open,
	.trywait = suet_trywait
};

static int suet_fabric_close(fid_t fid)
{
	int ret;
	struct suet_fabric *suet_fabric;

	suet_fabric = container_of(fid, struct suet_fabric, util_fabric.fabric_fid.fid);
	ret = suet_dgram_fabric_close(suet_fabric->dgram_ctx);
	if (ret)
		return ret;

	ret = ofi_fabric_close(&suet_fabric->util_fabric);
	if (ret)
		return ret;

	free(suet_fabric);
	return 0;
}

static struct fi_ops suet_fabric_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = suet_fabric_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

int suet_fabric(struct fi_fabric_attr *attr, struct fid_fabric **fabric,
		void *context)
{
	struct suet_fabric *suet_fabric;
	int ret;

	suet_fabric = calloc(1, sizeof(*suet_fabric));
	if (!suet_fabric)
		return -FI_ENOMEM;

	ret = ofi_fabric_init(&suet_prov, &suet_fabric_attr, attr,
			      &suet_fabric->util_fabric, context);
	if (ret)
		goto err1;

	ret = suet_dgram_fabric_open(attr, context, &suet_fabric->dgram_ctx);
	if (ret)
		goto err2;

	*fabric = &suet_fabric->util_fabric.fabric_fid;
	(*fabric)->fid.ops = &suet_fabric_fi_ops;
	(*fabric)->ops = &suet_fabric_ops;

	return 0;
err2:
	(void) ofi_fabric_close(&suet_fabric->util_fabric);
err1:
	free(suet_fabric);
	return ret;
}
