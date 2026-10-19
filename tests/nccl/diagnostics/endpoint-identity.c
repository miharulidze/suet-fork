#define _GNU_SOURCE
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { int rc = (call); if (rc) { \
    fprintf(stderr, "%s: %s (%d)\n", #call, fi_strerror(-rc), rc); return 2; \
} } while (0)

int main(void)
{
    struct fi_info *hints = fi_allocinfo(), *info = NULL;
    struct fid_fabric *fabric = NULL;
    struct fid_domain *domain = NULL;
    struct fid_av *av = NULL;
    struct fid_cq *cq = NULL;
    struct fid_ep *ep[2] = {NULL, NULL};
    struct fi_av_attr av_attr = {.type = FI_AV_TABLE, .count = 8};
    struct fi_cq_attr cq_attr = {.size = 128, .format = FI_CQ_FORMAT_TAGGED,
                                .wait_obj = FI_WAIT_NONE};
    unsigned char addresses[2][128] = {{0}};
    size_t sizes[2] = {128, 128};
    fi_addr_t inserted[2] = {FI_ADDR_UNSPEC, FI_ADDR_UNSPEC};
    if (!hints) return 2;
    hints->fabric_attr->prov_name = strdup("udp;ofi_suet");
    hints->caps = FI_MSG | FI_TAGGED;
    hints->ep_attr->type = FI_EP_RDM;
    hints->tx_attr->msg_order = FI_ORDER_SAS;
    hints->rx_attr->msg_order = FI_ORDER_SAS;
    hints->domain_attr->threading = FI_THREAD_SAFE;
    CHECK(fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
                     NULL, NULL, 0, hints, &info));
    CHECK(fi_fabric(info->fabric_attr, &fabric, NULL));
    CHECK(fi_domain(fabric, info, &domain, NULL));
    CHECK(fi_av_open(domain, &av_attr, &av, NULL));
    CHECK(fi_cq_open(domain, &cq_attr, &cq, NULL));
    for (int n = 0; n < 2; n++) {
        CHECK(fi_endpoint(domain, info, &ep[n], NULL));
        CHECK(fi_ep_bind(ep[n], &av->fid, 0));
        CHECK(fi_ep_bind(ep[n], &cq->fid, FI_TRANSMIT | FI_RECV));
        CHECK(fi_enable(ep[n]));
        CHECK(fi_getname(&ep[n]->fid, addresses[n], &sizes[n]));
        printf("endpoint_%d_name (%zu bytes): ", n, sizes[n]);
        for (size_t b = 0; b < sizes[n]; b++) printf("%02x", addresses[n][b]);
        putchar('\n');
        ssize_t rc = fi_av_insert(av, addresses[n], 1, &inserted[n], 0, NULL);
        if (rc != 1) {
            fprintf(stderr, "fi_av_insert: %zd\n", rc);
            return 2;
        }
        printf("endpoint_%d_fi_addr=%" PRIu64 "\n", n, inserted[n]);
    }
    int names_differ = sizes[0] != sizes[1] ||
                       memcmp(addresses[0], addresses[1], sizes[0]) != 0;
    int aliases = inserted[0] == inserted[1];
    printf("distinct_endpoint_names=%s; address_vector_alias=%s\n",
           names_differ ? "YES" : "NO", aliases ? "YES" : "NO");
    for (int n = 0; n < 2; n++) CHECK(fi_close(&ep[n]->fid));
    CHECK(fi_close(&cq->fid));
    CHECK(fi_close(&av->fid));
    CHECK(fi_close(&domain->fid));
    CHECK(fi_close(&fabric->fid));
    fi_freeinfo(info);
    fi_freeinfo(hints);
    return names_differ && !aliases ? 0 : 1;
}
