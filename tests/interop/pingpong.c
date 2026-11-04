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
#include "ref_address.h"

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
struct hello { uint32_t magic, version, mode, bytes, rounds, reference, ip, backend, pid, ri, initiator, raw_len; uint8_t raw[64]; };
struct op { bool received, done; size_t bytes; };
struct endpoint {
    bool reference;
    struct fi_info *info;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_ep *ep;
    struct fid_av *av;
    struct fid_cq *txcq, *rxcq;
    fi_addr_t peer;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &ts));
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void progress(struct endpoint *e)
{
    for (int i=0;i<2;i++) {
        struct fid_cq *cq=i?e->rxcq:e->txcq;
        struct fi_cq_data_entry entries[8];
        ssize_t n=fi_cq_read(cq,entries,8);
        if (n==-FI_EAVAIL) {
            struct fi_cq_err_entry err={0};
            REQUIRE(fi_cq_readerr(cq,&err,0)==1);
            fprintf(stderr,"CQ_ERROR provider=%s err=%d prov_errno=%d %s\n",
                    e->reference?"ref":"suet",err.err,err.prov_errno,fi_strerror(err.err));
            exit(1);
        }
        REQUIRE(n>=0 || n==-FI_EAGAIN);
        for (ssize_t j=0;j<n;j++) {
            struct op *op=entries[j].op_context;
            REQUIRE(op && !op->done);
            REQUIRE(op->received==!!(entries[j].flags&FI_RECV));
            REQUIRE(!op->received || entries[j].len==op->bytes);
            op->done=true;
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

static struct hello open_endpoint(struct endpoint *e,const char *ip,bool rod,
                                  uint32_t bytes,uint32_t rounds,const char *capture,const char *backend)
{
    struct hello h={.magic=htonl(0x53554554),.version=htonl(2),.mode=htonl(rod),
        .bytes=htonl(bytes),.rounds=htonl(rounds),.reference=htonl(e->reference),
        .backend=htonl(!strcmp(backend,"verbs"))};
    REQUIRE(inet_pton(AF_INET,ip,&h.ip)==1);
    REQUIRE(!setenv("UET_UD_SOURCE",ip,1));
    REQUIRE(!setenv("UET_DGRAM_PROVIDER",backend,1));
    if (e->reference) {
        if (strcmp(capture,"-")) REQUIRE(!setenv("UET_DGRAM_CAPTURE",capture,1));
        else REQUIRE(!unsetenv("UET_DGRAM_CAPTURE"));
    }
    struct fi_info *hints=fi_allocinfo();
    struct fi_av_attr avattr={.type=FI_AV_TABLE,.count=4};
    struct fi_cq_attr cqattr={.format=FI_CQ_FORMAT_DATA,.size=128,.wait_obj=FI_WAIT_NONE};
    REQUIRE(hints);
    hints->caps=FI_MSG;hints->ep_attr->type=FI_EP_RDM;
    char provider[64];snprintf(provider,sizeof(provider),"%s;ofi_suet",backend);
    hints->fabric_attr->prov_name=strdup(e->reference?"uet_ref":provider);
    if (!e->reference && !strcmp(backend,"verbs")) hints->domain_attr->name=strdup("mlx5_0-dgram");
    OK(fi_getinfo(FI_VERSION(FI_MAJOR_VERSION,FI_MINOR_VERSION),ip,"0",FI_SOURCE,hints,&e->info));
    e->info->tx_attr->msg_order=e->info->rx_attr->msg_order=rod?FI_ORDER_SAS:0;
    OK(fi_fabric(e->info->fabric_attr,&e->fabric,NULL));
    OK(fi_domain(e->fabric,e->info,&e->domain,NULL));
    OK(fi_av_open(e->domain,&avattr,&e->av,NULL));
    OK(fi_cq_open(e->domain,&cqattr,&e->txcq,NULL));
    OK(fi_cq_open(e->domain,&cqattr,&e->rxcq,NULL));
    OK(fi_endpoint(e->domain,e->info,&e->ep,NULL));
    OK(fi_ep_bind(e->ep,&e->av->fid,0));
    OK(fi_ep_bind(e->ep,&e->txcq->fid,FI_SEND));
    OK(fi_ep_bind(e->ep,&e->rxcq->fid,FI_RECV));
    OK(fi_enable(e->ep));
    if (e->reference) {
        struct ref_address name={0};size_t length=sizeof(name);
        OK(fi_getname(&e->ep->fid,&name,&length));
        REQUIRE(length==sizeof(name) && name.magic==REF_ADDRESS_MAGIC && name.raw_len<=sizeof(h.raw));
        h.pid=htonl(name.uet.pid_on_fep);h.ri=htonl(name.uet.start_index);h.initiator=htonl(name.uet.initiator_id);
        h.raw_len=htonl(name.raw_len);memcpy(h.raw,name.raw,name.raw_len);
    } else {
        struct suet_address name={0};size_t length=sizeof(name);
        OK(fi_getname(&e->ep->fid,&name,&length));
        REQUIRE(length==sizeof(name) && name.raw_len<=sizeof(name.raw));
        h.pid=htonl(name.pid);h.ri=htonl(name.ri);h.initiator=htonl(name.initiator);
        h.raw_len=htonl(name.raw_len);memcpy(h.raw,name.raw,name.raw_len);
    }
    fi_freeinfo(hints);
    printf("ENDPOINT provider=%s backend=%s type=FI_EP_DGRAM raw_len=%u pid=%u ri=%u initiator=%u mode=%s\n",
        e->reference?"ref":"suet",backend,ntohl(h.raw_len),ntohl(h.pid),ntohl(h.ri),ntohl(h.initiator),rod?"rod":"rud");
    return h;
}

static void add_peer(struct endpoint *e,const struct hello *h)
{
    size_t raw_len=ntohl(h->raw_len);
    if (e->reference) {
        struct ref_address a={.magic=REF_ADDRESS_MAGIC,.raw_len=raw_len,
            .uet={.ver=UET_ADDR_VERSION,
                .flags=UET_ADDR_FA_V|UET_ADDR_FEP_CAP_V|UET_ADDR_PID_ON_FEP_V|UET_ADDR_INDEX_V|UET_ADDR_INITIATOR_V,
                .fep_cap=UET_FEP_CAP_AI_FULL,.fa.v4=ntohl(h->ip),.pid_on_fep=ntohl(h->pid),
                .start_index=ntohl(h->ri),.num_indices=1,.initiator_id=ntohl(h->initiator)}};
        REQUIRE(raw_len && raw_len<=sizeof(a.raw));memcpy(a.raw,h->raw,raw_len);
        REQUIRE(fi_av_insert(e->av,&a,1,&e->peer,0,NULL)==1);
    } else {
        struct suet_address a={.version=1,.pid=ntohl(h->pid),.ri=ntohl(h->ri),.count=1,
            .initiator=ntohl(h->initiator),.raw_len=raw_len};
        REQUIRE(raw_len && raw_len<=sizeof(a.raw));memcpy(a.raw,h->raw,raw_len);
        REQUIRE(fi_av_insert(e->av,&a,1,&e->peer,0,NULL)==1);
    }
}

static void post(struct endpoint *e, void *buffer, struct op *op)
{
    uint64_t deadline = now_ms() + 5000;
    ssize_t rc;
    do {
        if (op->received)
            rc = fi_recv(e->ep, buffer, op->bytes, NULL, FI_ADDR_UNSPEC, op);
        else
            rc = fi_send(e->ep, buffer, op->bytes, NULL, e->peer, op);
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
    if (argc != 11) {
        fprintf(stderr, "Usage: %s suet|ref server|client LOCAL_IP SERVER_IP CONTROL_PORT rod|rud BYTES ROUNDS CAPTURE verbs|udp\n", argv[0]);
        return 2;
    }
    REQUIRE(!strcmp(argv[1], "suet") || !strcmp(argv[1], "ref"));
    REQUIRE(!strcmp(argv[2], "server") || !strcmp(argv[2], "client"));
    REQUIRE(!strcmp(argv[6], "rod") || !strcmp(argv[6], "rud"));
    REQUIRE(!strcmp(argv[10],"verbs") || !strcmp(argv[10],"udp"));
    bool server = !strcmp(argv[2], "server"), rod = !strcmp(argv[6], "rod");
    uint32_t bytes = strtoul(argv[7], NULL, 10), rounds = strtoul(argv[8], NULL, 10);
    int port = atoi(argv[5]);
    REQUIRE(bytes >= 8 && bytes <= 1048576 && rounds && rounds <= 10000 && port > 0 && port < 65536);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct endpoint e = {.reference = !strcmp(argv[1], "ref")};
    struct hello local = open_endpoint(&e, argv[3], rod, bytes, rounds, argv[9], argv[10]), remote;
    int control = connect_control(server, argv[3], argv[4], port);
    control_io(&e, control, &local, sizeof(local), true);
    control_io(&e, control, &remote, sizeof(remote), false);
    REQUIRE(remote.magic == local.magic && remote.version == local.version && remote.mode == local.mode &&
            remote.bytes == local.bytes && remote.rounds == local.rounds && remote.backend == local.backend);
    add_peer(&e, &remote);
    uint8_t *tx = malloc(bytes), *rx = malloc(bytes), *expected = malloc(bytes);
    struct op *receives = calloc(rounds, sizeof(*receives));
    REQUIRE(tx && rx && expected && receives);
    for (unsigned i = 0; i < rounds; ++i)
        receives[i] = (struct op) {.received = true, .bytes = bytes};
    post(&e, rx, &receives[0]);
    for (unsigned round = 0; round < rounds; ++round) {
        struct op *receive = &receives[round], send = {.bytes = bytes};
        if (!server && round)
            post(&e, rx, receive);
        if (!round) {
            uint8_t ready = 1, other;
            control_io(&e, control, &ready, 1, true);
            control_io(&e, control, &other, 1, false); REQUIRE(other == ready);
        }
        if (server) {
            wait_op(&e, receive, NULL);
            payload(expected, bytes, round, false); REQUIRE(!memcmp(rx, expected, bytes));
            printf("server RX ping round=%u bytes=%u PASS\n", round, bytes);
            /* The reference has no overflow/unexpected queue. Publish the
             * next receive before pong permits the client's next ping. */
            if (round + 1 < rounds)
                post(&e, rx, &receives[round + 1]);
            payload(tx, bytes, round, true); post(&e, tx, &send);
            printf("server TX pong round=%u bytes=%u\n", round, bytes);
            wait_op(&e, &send, NULL);
        } else {
            payload(tx, bytes, round, false); post(&e, tx, &send);
            printf("client TX ping round=%u bytes=%u\n", round, bytes);
            wait_op(&e, receive, &send);
            payload(expected, bytes, round, true); REQUIRE(!memcmp(rx, expected, bytes));
            printf("client RX pong round=%u bytes=%u PASS\n", round, bytes);
        }
    }
    uint8_t done = 2, other;
    control_io(&e, control, &done, 1, true); control_io(&e, control, &other, 1, false); REQUIRE(other == done);
    uint64_t until = now_ms() + 200;
    while (now_ms() < until) progress(&e);
    puts("CLOSING");
    OK(fi_close(&e.ep->fid));OK(fi_close(&e.txcq->fid));OK(fi_close(&e.rxcq->fid));
    OK(fi_close(&e.av->fid));OK(fi_close(&e.domain->fid));OK(fi_close(&e.fabric->fid));
    fi_freeinfo(e.info); free(tx); free(rx); free(expected); free(receives); close(control);
    printf("RESULT provider=%s role=%s mode=%s bytes=%u rounds=%u PASS\n", argv[1], argv[2], argv[6], bytes, rounds);
    return 0;
}
