/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Stock-fabtests setup helper: select an unordered FI_MSG endpoint contract.
 * This intercepts discovery only; provider code and data-path calls are untouched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/fabric.h>

typedef int (*getinfo_fn)(uint32_t, const char *, const char *, uint64_t,
                          const struct fi_info *, struct fi_info **);

int fi_getinfo(uint32_t version, const char *node, const char *service,
               uint64_t flags, const struct fi_info *hints, struct fi_info **out)
{
    getinfo_fn real_getinfo = (getinfo_fn)dlsym(RTLD_NEXT, "fi_getinfo");
    if (!real_getinfo)
        return -FI_ENOSYS;
    uint64_t requested_tx = hints && hints->tx_attr ? hints->tx_attr->msg_order : 0;
    uint64_t requested_rx = hints && hints->rx_attr ? hints->rx_attr->msg_order : 0;
    int rc = real_getinfo(version, node, service, flags, hints, out);
    if (rc)
        return rc;
    const char *mode = getenv("SUET_BENCH_ENDPOINT_MODE");
    if (!mode || strcmp(mode, "rud"))
        return 0;
    for (struct fi_info *info = *out; info; info = info->next) {
        const char *name = info->fabric_attr ? info->fabric_attr->prov_name : NULL;
        if (!name || (!strstr(name, "ofi_suet") && !strstr(name, "uet_ref")))
            continue;
        if (requested_tx || requested_rx || !info->ep_attr ||
            info->ep_attr->type != FI_EP_RDM || !hints || !(hints->caps & FI_MSG) ||
            (hints->caps & (FI_RMA | FI_ATOMIC))) {
            fprintf(stderr, "BENCH_ENDPOINT_ORDER refused incompatible endpoint contract\n");
            return -FI_EINVAL;
        }
        info->tx_attr->msg_order = info->rx_attr->msg_order = 0;
        fprintf(stderr, "BENCH_ENDPOINT_ORDER provider=%s mode=rud tx=0 rx=0 requested_tx=0 requested_rx=0\n", name);
    }
    return 0;
}
