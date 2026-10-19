/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Public API regression: four logical endpoints share one UDP socket. */
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(call) do { int ret_ = (call); if (ret_) { \
	fprintf(stderr, "%s: %s (%d)\n", #call, fi_strerror(-ret_), ret_); \
	exit(1); } } while (0)
#define REQUIRE(cond) do { if (!(cond)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)

struct operation {
	int done;
	int receive;
	size_t len;
};

struct endpoints {
	struct fid_ep *ep[4];
	struct fid_cq *cq[4];
	fi_addr_t addr[4];
};

static uint64_t now_ms(void)
{
	struct timespec ts;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &ts));
	return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void progress(struct endpoints *eps)
{
	struct fi_cq_tagged_entry entries[8];
	struct fi_cq_err_entry err;
	struct operation *op;
	ssize_t n, i;
	int ep;

	for (ep = 0; ep < 4; ep++) {
		n = fi_cq_read(eps->cq[ep], entries, 8);
		if (n == -FI_EAVAIL) {
			REQUIRE(fi_cq_readerr(eps->cq[ep], &err, 0) == 1);
			fprintf(stderr, "CQ %d: %s, provider error %d\n", ep,
				fi_strerror(err.err), err.prov_errno);
			exit(1);
		}
		REQUIRE(n >= 0 || n == -FI_EAGAIN);
		for (i = 0; i < n; i++) {
			op = entries[i].op_context;
			REQUIRE(op && !op->done);
			REQUIRE(!!(entries[i].flags & FI_RECV) == op->receive);
			if (op->receive)
				REQUIRE(entries[i].len == op->len);
			op->done = 1;
		}
	}
}

static void deliver(struct endpoints *eps, int target, int tagged,
		    int unexpected, int wildcard, size_t len)
{
	struct operation rx[2] = {{.receive = 1, .len = len},
				 {.receive = 1, .len = len}};
	struct operation tx[2] = {0};
	unsigned char *input[2], *output[2];
	uint64_t deadline;
	ssize_t ret;
	int source, seen[2] = {0};
	size_t i;

	for (source = 0; source < 2; source++) {
		input[source] = malloc(len ? len : 1);
		output[source] = malloc(len ? len : 1);
		REQUIRE(input[source] && output[source]);
		for (i = 0; i < len; i++)
			input[source][i] = (unsigned char) (i * 37 + source * 71 + 11);
		memset(output[source], 0xa5, len);
	}
	/* Post source 0 first, but send source 1 first. Both share the same
	 * datagram peer and tag; source selection must use initiator identity. */
	for (int phase = 0; phase < 3; phase++) {
		if (phase == (unexpected ? 2 : 0)) {
			for (source = 0; source < 2; source++) {
				fi_addr_t addr = wildcard ? FI_ADDR_UNSPEC : eps->addr[source];
				if (tagged)
					CHECK(fi_trecv(eps->ep[target], output[source], len,
						       NULL, addr, 42, 0, &rx[source]));
				else
					CHECK(fi_recv(eps->ep[target], output[source], len,
						      NULL, addr, &rx[source]));
			}
		}
		if (phase == 1) {
			for (source = 1; source >= 0; source--) {
				deadline = now_ms() + 10000;
				do {
					ret = tagged ?
						fi_tsend(eps->ep[source], input[source], len,
							 NULL, eps->addr[target], 42, &tx[source]) :
						fi_send(eps->ep[source], input[source], len,
							NULL, eps->addr[target], &tx[source]);
					if (ret == -FI_EAGAIN)
						progress(eps);
					REQUIRE(now_ms() < deadline);
				} while (ret == -FI_EAGAIN);
				CHECK(ret);
			}
			if (unexpected) {
				deadline = now_ms() + 100;
				do { progress(eps); } while (now_ms() < deadline);
			}
		}
	}
	deadline = now_ms() + 10000;
	while (!rx[0].done || !rx[1].done || !tx[0].done || !tx[1].done) {
		progress(eps);
		REQUIRE(now_ms() < deadline);
		usleep(100);
	}
	for (source = 0; source < 2; source++) {
		/* Wildcard receives accept either arrival order, including RUD. */
		int expected = source;
		if (wildcard && len) {
			expected = memcmp(input[0], output[source], len) ? 1 : 0;
			REQUIRE(!seen[expected]);
			seen[expected] = 1;
		}
		REQUIRE(!memcmp(input[expected], output[source], len));
	}
	for (source = 0; source < 2; source++) {
		free(input[source]);
		free(output[source]);
	}
	printf("RI 0/1 -> RI %d: %s %s %s, %zu bytes: PASS\n", target,
	       tagged ? "tagged" : "message", unexpected ? "unexpected" : "posted",
	       wildcard ? "wildcard" : "directed", len);
}

static void run(uint64_t order)
{
	struct fi_info *hints = fi_allocinfo(), *info = NULL;
	struct fid_fabric *fabric;
	struct fid_domain *domain;
	struct fid_av *av;
	struct endpoints eps = {0};
	struct fi_av_attr av_attr = {.type = FI_AV_TABLE, .count = 8};
	struct fi_cq_attr cq_attr = {.size = 128, .format = FI_CQ_FORMAT_TAGGED,
				    .wait_obj = FI_WAIT_NONE};
	unsigned char names[4][128] = {{0}}, lookup[128];
	size_t sizes[4], size;
	int i, j;

	REQUIRE(hints);
	hints->fabric_attr->prov_name = strdup("udp;ofi_suet");
	hints->caps = FI_MSG | FI_TAGGED | FI_DIRECTED_RECV;
	hints->ep_attr->type = FI_EP_RDM;
	hints->tx_attr->msg_order = hints->rx_attr->msg_order = order;
	hints->domain_attr->threading = FI_THREAD_SAFE;
	CHECK(fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
			 "127.0.0.1", "0", FI_SOURCE, hints, &info));
	/* Zero in hints means unconstrained, so explicitly select unordered
	 * delivery rather than inheriting the provider's advertised orders. */
	REQUIRE((info->tx_attr->msg_order & order) == order);
	info->tx_attr->msg_order = info->rx_attr->msg_order = order;
	CHECK(fi_fabric(info->fabric_attr, &fabric, NULL));
	CHECK(fi_domain(fabric, info, &domain, NULL));
	CHECK(fi_av_open(domain, &av_attr, &av, NULL));
	for (i = 0; i < 4; i++) {
		CHECK(fi_cq_open(domain, &cq_attr, &eps.cq[i], NULL));
		CHECK(fi_endpoint(domain, info, &eps.ep[i], NULL));
		CHECK(fi_ep_bind(eps.ep[i], &av->fid, 0));
		CHECK(fi_ep_bind(eps.ep[i], &eps.cq[i]->fid, FI_TRANSMIT | FI_RECV));
		CHECK(fi_enable(eps.ep[i]));
		sizes[i] = sizeof(names[i]);
		CHECK(fi_getname(&eps.ep[i]->fid, names[i], &sizes[i]));
		REQUIRE(fi_av_insert(av, names[i], 1, &eps.addr[i], 0, NULL) == 1);
		for (j = 0; j < i; j++)
			REQUIRE(eps.addr[i] != eps.addr[j]);
		size = sizeof(lookup);
		CHECK(fi_av_lookup(av, eps.addr[i], lookup, &size));
		REQUIRE(size == sizes[i] && !memcmp(lookup, names[i], size));
	}
	printf("%s: distinct endpoint AV entries: PASS\n", order ? "ROD" : "RUD");
	deliver(&eps, 2, 0, 0, 0, 65537);
	deliver(&eps, 3, 1, 0, 0, 17);
	deliver(&eps, 2, 1, 1, 0, 65537);
	deliver(&eps, 3, 1, 0, 1, 17);
	deliver(&eps, 3, 1, 0, 1, 0);
	for (i = 0; i < 4; i++) {
		CHECK(fi_close(&eps.ep[i]->fid));
		CHECK(fi_close(&eps.cq[i]->fid));
	}
	CHECK(fi_close(&av->fid));
	CHECK(fi_close(&domain->fid));
	CHECK(fi_close(&fabric->fid));
	fi_freeinfo(info);
	fi_freeinfo(hints);
}

int main(void)
{
	run(FI_ORDER_SAS);
	run(0);
	return 0;
}
