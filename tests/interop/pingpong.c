/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Independent client/server processes. Only endpoint setup uses TCP. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "uet_api.h"
#include "udp_backend.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (errno=%d)\n", \
    __FILE__, __LINE__, #x, errno); exit(1); } } while (0)
#define OK(x) do { ssize_t result_ = (x); if (result_ < 0) { \
    fprintf(stderr, "%s: %s (%zd)\n", #x, fi_strerror(-result_), result_); exit(1); } } while (0)

/* Named SUET address ABI. Translated only on the out-of-band channel. */
struct suet_address {
    uint16_t version, flags, caps, pid, ri, count;
    uint32_t initiator;
    uint8_t raw[32];
    uint16_t raw_len;
};
_Static_assert(sizeof(struct suet_address) == 52, "SUET address ABI changed");
struct hello { uint32_t magic, version, mode, bytes, rounds, reference, ip, port, pid, ri, initiator; };
struct op { bool received, done; size_t bytes; };
struct endpoint {
    bool reference;
    struct fi_info *info;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_ep *ep;
    struct fid_av *av;
    struct fid_cq *cq;
    fi_addr_t peer;
    struct fid_fabric rf;
    struct fid_domain rd;
    struct fid_ep re;
    struct fid_cq rc;
    uet_handle_t uet;
    uet_domain_handle_t ud;
    uet_ep_handle_t ue;
    uet_cq_handle_t txcq, rxcq;
    uet_addr_handle_t address;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void progress(struct endpoint *e)
{
    for (int i = 0; i < (e->reference ? 2 : 1); ++i) {
        struct fi_cq_data_entry entries[8];
        uet_cq_handle_t cq = i ? e->rxcq : e->txcq;
        ssize_t n = e->reference ? uet_cq_read(cq, entries, 8) : fi_cq_read(e->cq, entries, 8);
        if (n == -FI_EAVAIL) {
            struct fi_cq_err_entry err = {0};
            REQUIRE((e->reference ? uet_cq_readerr(cq, &err) : fi_cq_readerr(e->cq, &err, 0)) == 1);
            fprintf(stderr, "CQ_ERROR provider=%s err=%d prov_errno=%d %s\n",
                    e->reference ? "ref" : "suet", err.err, err.prov_errno, fi_strerror(err.err));
            exit(1);
        }
        REQUIRE(n >= 0 || n == -FI_EAGAIN);
        for (ssize_t j = 0; j < n; ++j) {
            struct op *op = entries[j].op_context;
            REQUIRE(op && !op->done);
            REQUIRE(op->received == !!(entries[j].flags & FI_RECV));
            REQUIRE(!op->received || entries[j].len == op->bytes);
            op->done = true;
        }
    }
}

/* TCP carries fixed-size identity and synchronization records, never payload. */
static void control_io(struct endpoint *e, int fd, void *buffer, size_t bytes, bool write_it)
{
    uint64_t deadline = now_ms() + 10000;
    size_t offset = 0;
    while (offset < bytes) {
        progress(e);
        REQUIRE(now_ms() < deadline);
        struct pollfd p = {.fd = fd, .events = write_it ? POLLOUT : POLLIN};
        REQUIRE(poll(&p, 1, 1) >= 0);
        if (!(p.revents & p.events)) { REQUIRE(!(p.revents & (POLLERR | POLLHUP | POLLNVAL))); continue; }
        ssize_t n = write_it ? send(fd, (char *)buffer + offset, bytes - offset, MSG_NOSIGNAL) :
                              recv(fd, (char *)buffer + offset, bytes - offset, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        REQUIRE(n > 0);
        offset += n;
    }
}

static int connect_control(bool server, const char *ip, const char *peer_ip, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), reuse = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(port)};
    REQUIRE(fd >= 0 && inet_pton(AF_INET, server ? ip : peer_ip, &address.sin_addr) == 1);
    REQUIRE(!setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
    if (server) {
        REQUIRE(!bind(fd, (void *)&address, sizeof(address)) && !listen(fd, 1));
        puts("CONTROL_LISTENING");
        struct pollfd p = {.fd = fd, .events = POLLIN};
        REQUIRE(poll(&p, 1, 10000) == 1);
        int connection = accept(fd, NULL, NULL);
        REQUIRE(connection >= 0);
        close(fd);
        return connection;
    }
    uint64_t deadline = now_ms() + 10000;
    while (connect(fd, (void *)&address, sizeof(address))) {
        REQUIRE(errno == ECONNREFUSED || errno == EINTR);
        REQUIRE(now_ms() < deadline);
        struct timespec delay = {.tv_nsec = 10000000}; nanosleep(&delay, NULL);
    }
    return fd;
}

static struct hello open_endpoint(struct endpoint *e, const char *ip, bool rod,
                                  uint32_t bytes, uint32_t rounds, const char *capture)
{
    struct hello h = {.magic = htonl(0x53554554), .version = htonl(1),
        .mode = htonl(rod), .bytes = htonl(bytes), .rounds = htonl(rounds),
        .reference = htonl(e->reference)};
    struct sockaddr_in socket_name;
    struct fi_info *hints = fi_allocinfo();
    REQUIRE(hints);
    hints->caps = FI_MSG;
    if (e->reference) {
        struct uet_addr source = {.flags = UET_ADDR_IPV4}, name;
        struct fi_cq_attr cqattr = {.format = FI_CQ_FORMAT_DATA, .size = 128};
        bench_udp_open(ip, capture);
        OK(uet_initialize(&e->uet));
        OK(uet_getinfo(e->uet, &source, hints, &e->info));
        e->info->tx_attr->size = e->info->rx_attr->size = 64;
        e->info->tx_attr->msg_order = e->info->rx_attr->msg_order = rod ? FI_ORDER_SAS : 0;
        e->info->tx_attr->op_flags = e->info->rx_attr->op_flags = FI_COMPLETION;
        OK(uet_domain(e->uet, &e->rf, e->info, &e->rd, NULL, NULL, NULL, &e->ud));
        /* Explicit API selects JobID 0, matching SUET. Still a UDP backend. */
        OK(uet_endpoint(e->ud, e->info, &e->re, NULL, &e->ue, 0, 15, 16, 0, false, false, 0));
        OK(uet_ep_bind_cq(e->ue, &cqattr, &e->rc, FI_SEND, NULL, &e->txcq));
        OK(uet_ep_bind_cq(e->ue, &cqattr, &e->rc, FI_RECV, NULL, &e->rxcq));
        OK(uet_ep_enable(e->ue));
        OK(uet_getname(e->ue, &name));
        socket_name = bench_udp_name();
        h.pid = htonl(name.pid_on_fep); h.ri = htonl(name.start_index); h.initiator = htonl(name.initiator_id);
    } else {
        struct fi_av_attr avattr = {.type = FI_AV_TABLE, .count = 4};
        struct fi_cq_attr cqattr = {.format = FI_CQ_FORMAT_DATA, .size = 128, .wait_obj = FI_WAIT_NONE};
        struct suet_address name = {0}; size_t length = sizeof(name);
        hints->fabric_attr->prov_name = strdup("udp;ofi_suet");
        hints->ep_attr->type = FI_EP_RDM;
        OK(fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), ip, "0", FI_SOURCE, hints, &e->info));
        e->info->tx_attr->msg_order = e->info->rx_attr->msg_order = rod ? FI_ORDER_SAS : 0;
        REQUIRE(!strcmp(e->info->fabric_attr->prov_name, "udp;ofi_suet"));
        OK(fi_fabric(e->info->fabric_attr, &e->fabric, NULL));
        OK(fi_domain(e->fabric, e->info, &e->domain, NULL));
        OK(fi_av_open(e->domain, &avattr, &e->av, NULL));
        OK(fi_cq_open(e->domain, &cqattr, &e->cq, NULL));
        OK(fi_endpoint(e->domain, e->info, &e->ep, NULL));
        OK(fi_ep_bind(e->ep, &e->av->fid, 0));
        OK(fi_ep_bind(e->ep, &e->cq->fid, FI_SEND | FI_RECV));
        OK(fi_enable(e->ep));
        OK(fi_getname(&e->ep->fid, &name, &length));
        REQUIRE(length == sizeof(name) && name.raw_len == sizeof(socket_name));
        memcpy(&socket_name, name.raw, sizeof(socket_name));
        h.pid = htonl(name.pid); h.ri = htonl(name.ri); h.initiator = htonl(name.initiator);
    }
    fi_freeinfo(hints);
    h.ip = socket_name.sin_addr.s_addr; h.port = htonl(ntohs(socket_name.sin_port));
    printf("ENDPOINT provider=%s udp=%s:%u pid=%u ri=%u initiator=%u mode=%s\n",
           e->reference ? "ref" : "suet", ip, ntohl(h.port), ntohl(h.pid), ntohl(h.ri), ntohl(h.initiator), rod ? "rod" : "rud");
    return h;
}

static void add_peer(struct endpoint *e, const struct hello *h)
{
    struct sockaddr_in udp = {.sin_family = AF_INET, .sin_addr.s_addr = h->ip, .sin_port = htons(ntohl(h->port))};
    if (e->reference) {
        struct uet_addr a = {.ver = UET_ADDR_VERSION,
            .flags = UET_ADDR_FA_V | UET_ADDR_FEP_CAP_V | UET_ADDR_PID_ON_FEP_V | UET_ADDR_INDEX_V | UET_ADDR_INITIATOR_V,
            .fep_cap = UET_FEP_CAP_AI_FULL, .fa.v4 = ntohl(h->ip), .pid_on_fep = ntohl(h->pid),
            .start_index = ntohl(h->ri), .num_indices = 1, .initiator_id = ntohl(h->initiator)};
        bench_udp_peer(udp);
        OK(uet_av_insert(e->ud, &a, &e->address));
    } else {
        struct suet_address a = {.version = 1, .pid = ntohl(h->pid), .ri = ntohl(h->ri),
            .count = 1, .initiator = ntohl(h->initiator), .raw_len = sizeof(udp)};
        memcpy(a.raw, &udp, sizeof(udp));
        REQUIRE(fi_av_insert(e->av, &a, 1, &e->peer, 0, NULL) == 1);
    }
}

static void post(struct endpoint *e, void *buffer, struct op *op)
{
    uint64_t deadline = now_ms() + 5000;
    ssize_t rc;
    do {
        if (op->received)
            rc = e->reference ? uet_recv(e->ue, UET_JOB_ID_ANY, buffer, op->bytes, NULL, NULL, op) :
                                fi_recv(e->ep, buffer, op->bytes, NULL, FI_ADDR_UNSPEC, op);
        else
            rc = e->reference ? uet_send(e->ue, 0, buffer, op->bytes, NULL, e->address, op, 0) :
                                fi_send(e->ep, buffer, op->bytes, NULL, e->peer, op);
        if (rc != -FI_EAGAIN) { OK(rc); return; }
        progress(e); REQUIRE(now_ms() < deadline);
    } while (true);
}

static void wait_op(struct endpoint *e, struct op *a, struct op *b)
{
    uint64_t deadline = now_ms() + 5000;
    while (!a->done || (b && !b->done)) {
        progress(e);
        if (now_ms() >= deadline) {
            fprintf(stderr, "TIMEOUT receive=%d first_done=%d second_done=%d\n", a->received, a->done, b ? b->done : -1);
            exit(2);
        }
    }
}

static void payload(uint8_t *buffer, size_t bytes, unsigned round, bool pong)
{
    for (size_t i = 0; i < bytes; ++i) buffer[i] = (uint8_t)(i * 37 + round * 17 + pong * 71);
    memcpy(buffer, pong ? "pong" : "ping", 4);
    uint32_t sequence = htonl(round); memcpy(buffer + 4, &sequence, sizeof(sequence));
}

int main(int argc, char **argv)
{
    if (argc != 10) {
        fprintf(stderr, "Usage: %s suet|ref server|client LOCAL_IP SERVER_IP CONTROL_PORT rod|rud BYTES ROUNDS CAPTURE\n", argv[0]);
        return 2;
    }
    REQUIRE(!strcmp(argv[1], "suet") || !strcmp(argv[1], "ref"));
    REQUIRE(!strcmp(argv[2], "server") || !strcmp(argv[2], "client"));
    REQUIRE(!strcmp(argv[6], "rod") || !strcmp(argv[6], "rud"));
    bool server = !strcmp(argv[2], "server"), rod = !strcmp(argv[6], "rod");
    uint32_t bytes = strtoul(argv[7], NULL, 10), rounds = strtoul(argv[8], NULL, 10);
    int port = atoi(argv[5]);
    REQUIRE(bytes >= 8 && bytes <= 1048576 && rounds && rounds <= 10000 && port > 0 && port < 65536);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct endpoint e = {.reference = !strcmp(argv[1], "ref")};
    struct hello local = open_endpoint(&e, argv[3], rod, bytes, rounds, argv[9]), remote;
    int control = connect_control(server, argv[3], argv[4], port);
    control_io(&e, control, &local, sizeof(local), true);
    control_io(&e, control, &remote, sizeof(remote), false);
    REQUIRE(remote.magic == local.magic && remote.version == local.version && remote.mode == local.mode &&
            remote.bytes == local.bytes && remote.rounds == local.rounds);
    add_peer(&e, &remote);
    uint8_t *tx = malloc(bytes), *rx = malloc(bytes), *expected = malloc(bytes);
    REQUIRE(tx && rx && expected);
    for (unsigned round = 0; round < rounds; ++round) {
        struct op receive = {.received = true, .bytes = bytes}, send = {.bytes = bytes};
        memset(rx, 0xa5, bytes);
        post(&e, rx, &receive);
        if (!round) {
            uint8_t ready = 1, other;
            control_io(&e, control, &ready, 1, true);
            control_io(&e, control, &other, 1, false); REQUIRE(other == ready);
        }
        if (server) {
            wait_op(&e, &receive, NULL);
            payload(expected, bytes, round, false); REQUIRE(!memcmp(rx, expected, bytes));
            printf("server RX ping round=%u bytes=%u PASS\n", round, bytes);
            payload(tx, bytes, round, true); post(&e, tx, &send);
            printf("server TX pong round=%u bytes=%u\n", round, bytes);
            wait_op(&e, &send, NULL);
        } else {
            payload(tx, bytes, round, false); post(&e, tx, &send);
            printf("client TX ping round=%u bytes=%u\n", round, bytes);
            wait_op(&e, &receive, &send);
            payload(expected, bytes, round, true); REQUIRE(!memcmp(rx, expected, bytes));
            printf("client RX pong round=%u bytes=%u PASS\n", round, bytes);
        }
    }
    uint8_t done = 2, other;
    control_io(&e, control, &done, 1, true); control_io(&e, control, &other, 1, false); REQUIRE(other == done);
    uint64_t until = now_ms() + 200;
    while (now_ms() < until) progress(&e);
    puts("CLOSING");
    if (e.reference) {
        OK(uet_ep_close(e.ue)); OK(uet_cq_close(e.txcq)); OK(uet_cq_close(e.rxcq));
        OK(uet_av_remove(e.address)); OK(uet_domain_close(e.ud)); OK(uet_finalize(e.uet));
    } else {
        OK(fi_close(&e.ep->fid)); OK(fi_close(&e.cq->fid)); OK(fi_close(&e.av->fid));
        OK(fi_close(&e.domain->fid)); OK(fi_close(&e.fabric->fid));
    }
    fi_freeinfo(e.info); free(tx); free(rx); free(expected); close(control);
    printf("RESULT provider=%s role=%s mode=%s bytes=%u rounds=%u PASS\n", argv[1], argv[2], argv[6], bytes, rounds);
    return 0;
}
