/* Exercise provider initialization without a peer or fabtests TCP setup. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>

int main(void)
{
    struct fi_info *hints = fi_allocinfo(), *info = NULL;
    struct fid_fabric *fabric = NULL;
    struct fid_domain *domain = NULL;
    if (!hints) return 1;
    hints->caps = FI_MSG;
    hints->ep_attr->type = FI_EP_RDM;
    hints->fabric_attr->prov_name = strdup("uet_ref");
    int rc = fi_getinfo(FI_VERSION(2, 7), NULL, NULL, 0, hints, &info);
    if (!rc) rc = fi_fabric(info->fabric_attr, &fabric, NULL);
    if (!rc) rc = fi_domain(fabric, info, &domain, NULL);
    printf("DOMAIN_RESULT=%d\n", rc);
    if (domain && fi_close(&domain->fid)) return 1;
    if (fabric && fi_close(&fabric->fid)) return 1;
    fi_freeinfo(info);
    fi_freeinfo(hints);
    return rc ? 1 : 0;
}
