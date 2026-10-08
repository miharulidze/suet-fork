/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include <config.h>
#if !ENABLE_DEBUG && !defined(NDEBUG)
#define NDEBUG
#endif
#include "suet.h"
#undef NDEBUG
#include <assert.h>
#include <pthread.h>

/* Exercise real frontend/backend ownership with a small provider double. */
#include "../suet/src/suet_av.c"
#include "../suet/src/suet_dgram.c"
#include "../suet/src/suet_mr.c"

struct test_av {
	struct fid_av fid;
	uint32_t addr[16];
	bool used[16];
};

static int av_open_count, av_close_count, mr_live, mr_calls, mr_fail_call;
static int mr_collisions;
static uint64_t mr_last_key;
static size_t mr_lengths[SUET_IOV_LIMIT];

static int test_av_close(struct fid *fid)
{
	av_close_count++;
	free(fid);
	return 0;
}

static int test_av_insert(struct fid_av *fid, const void *addr, size_t count,
			  fi_addr_t *out, uint64_t flags, void *context)
{
	struct test_av *av = container_of(fid, struct test_av, fid);
	uint32_t raw;
	size_t i;

	assert(count == 1);
	memcpy(&raw, addr, sizeof(raw));
	if (raw == UINT32_MAX) {
		if (flags & FI_SYNC_ERR)
			*(int *) context = FI_EINVAL;
		return 0;
	}
	for (i = 0; i < 16; i++) {
		if (av->used[i] && av->addr[i] == raw) {
			*out = i;
			return 1;
		}
	}
	for (i = 0; i < 16; i++) {
		if (!av->used[i]) {
			av->addr[i] = raw;
			av->used[i] = true;
			*out = i;
			return 1;
		}
	}
	return -FI_ENOMEM;
}

static int test_av_lookup(struct fid_av *fid, fi_addr_t addr, void *buf,
			  size_t *len)
{
	struct test_av *av = container_of(fid, struct test_av, fid);

	if (addr >= 16 || !av->used[addr])
		return -FI_EINVAL;
	assert(*len >= sizeof(uint32_t));
	memcpy(buf, &av->addr[addr], sizeof(uint32_t));
	*len = sizeof(uint32_t);
	return 0;
}

static int test_av_remove(struct fid_av *fid, fi_addr_t *addr, size_t count,
			  uint64_t flags)
{
	struct test_av *av = container_of(fid, struct test_av, fid);
	size_t i;

	for (i = 0; i < count; i++) {
		assert(addr[i] < 16 && av->used[addr[i]]);
		av->used[addr[i]] = false;
	}
	return 0;
}

static struct fi_ops test_av_fid_ops = {.close = test_av_close};
static struct fi_ops_av test_av_ops = {
	.insert = test_av_insert,
	.lookup = test_av_lookup,
	.remove = test_av_remove,
};

static int test_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
			struct fid_av **out, void *context)
{
	struct test_av *av = calloc(1, sizeof(*av));

	assert(av);
	av->fid.fid.ops = &test_av_fid_ops;
	av->fid.ops = &test_av_ops;
	*out = &av->fid;
	av_open_count++;
	return 0;
}

static int test_mr_close(struct fid *fid)
{
	assert(mr_live > 0);
	mr_live--;
	free(fid);
	return 0;
}

static struct fi_ops test_mr_fid_ops = {.close = test_mr_close};

static int test_mr_regattr(struct fid *fid, const struct fi_mr_attr *attr,
			   uint64_t flags, struct fid_mr **out)
{
	struct fid_mr *mr;

	mr_calls++;
	mr_last_key = attr->requested_key;
	if (mr_collisions) {
		mr_collisions--;
		return -FI_ENOKEY;
	}
	if (mr_calls == mr_fail_call)
		return -FI_ENOMEM;
	mr = calloc(1, sizeof(*mr));
	assert(mr && attr->iov_count == 1);
	mr->fid.ops = &test_mr_fid_ops;
	mr->mem_desc = mr;
	mr->key = attr->requested_key;
	assert(mr_live < SUET_IOV_LIMIT);
	mr_lengths[mr_live++] = attr->mr_iov[0].iov_len;
	*out = mr;
	return 0;
}

static struct fi_ops_domain test_domain_ops = {.av_open = test_av_open};
static struct fi_ops_mr test_mr_ops = {.regattr = test_mr_regattr};

static void init_public_av(struct suet_av *av, struct suet_domain *domain)
{
	av->util_av.domain = &domain->util_domain;
	av->util_av.prov = &suet_prov;
	assert(!ofi_genlock_init(&av->util_av.lock, OFI_LOCK_MUTEX));
}

static void cleanup_public_av(struct suet_av *av)
{
	ofi_idx_reset(&av->usr_av_addr_to_peer_idx);
	ofi_idm_reset(&av->peer_idx_to_usr_av_addr, NULL);
	ofi_genlock_destroy(&av->util_av.lock);
}

struct insert_args {
	struct suet_av *av;
	struct suet_av_addr addr;
	int peer;
};

static void *insert_peer(void *context)
{
	struct insert_args *args = context;
	fi_addr_t addr;
	int i, peer;

	for (i = 0; i < 100; i++) {
		assert(suet_av_insert(&args->av->util_av.av_fid, &args->addr, 1,
				      &addr, 0, NULL) == 1);
		peer = suet_av_peer_idx_from_usr_av_addr(args->av, addr);
		assert(peer && (!args->peer || args->peer == peer));
		args->peer = peer;
		assert(!suet_av_remove(&args->av->util_av.av_fid, &addr, 1, 0));
	}
	return NULL;
}

static void check_peers(struct suet_domain *domain)
{
	struct suet_av av[2] = {0};
	struct suet_av_addr raw[3] = {0};
	struct insert_args args[2] = {0};
	struct fi_cq_err_entry err = {0};
	struct fi_cq_data_entry comp = {0};
	fi_addr_t addr[3], other, backend;
	pthread_t threads[2];
	int peer, errors[3], i;
	uint32_t values[] = {11, UINT32_MAX, 22};

	for (i = 0; i < 3; i++)
		memcpy(raw[i].raw_dgram_addr, &values[i], sizeof(values[i]));
	for (i = 0; i < 2; i++)
		init_public_av(&av[i], domain);
	assert(!suet_av_insert(&av[0].util_av.av_fid, NULL, 0, NULL, 0, NULL));
	assert(suet_av_insert(&av[0].util_av.av_fid, raw, 3, addr, 0, NULL) ==
	       1);
	assert(addr[1] == FI_ADDR_NOTAVAIL && addr[2] == FI_ADDR_NOTAVAIL);
	assert(domain->dgram.addrlen == sizeof(uint32_t));
	peer = suet_av_peer_idx_from_usr_av_addr(&av[0], addr[0]);
	assert(peer);
	backend = suet_dgram_av_get_addr_by_peer_idx(domain, peer);
	assert(backend == 0); /* Zero is a valid provider address. */
	assert(suet_dgram_av_get_peer_idx_by_addr(domain, backend) == peer);
	assert(suet_av_insert(&av[1].util_av.av_fid, raw, 1, &other, 0, NULL) ==
	       1);
	assert(suet_av_peer_idx_from_usr_av_addr(&av[1], other) == peer);
	assert(!suet_av_remove(&av[0].util_av.av_fid, addr, 1, 0));
	assert(!suet_av_peer_idx_from_usr_av_addr(&av[0], addr[0]));
	assert(suet_dgram_av_get_peer_idx_by_addr(domain, backend) == peer);
	assert(suet_av_peer_idx_from_usr_av_addr(&av[1], other) == peer);
	assert(suet_av_insert(&av[0].util_av.av_fid, raw, 3, NULL, FI_SYNC_ERR,
			      errors) == 1);
	assert(!errors[0] && errors[1] == FI_EINVAL &&
	       errors[2] == FI_ECANCELED);
	assert(suet_av_peer_idx_from_usr_av_addr(&av[0], addr[0]) == peer);

	/* Discover an RX source before an application has inserted it. */
	err.err_data = raw[2].raw_dgram_addr;
	err.op_context = &err;
	err.len = 73;
	err.flags = FI_RECV;
	err.data = 54321;
	assert(!suet_dgram_av_handle_addr_notavail(domain, &err, &comp,
						   &backend));
	assert(comp.op_context == &err && comp.len == 73 &&
	       comp.flags == FI_RECV && comp.data == 54321);
	peer = suet_dgram_av_get_peer_idx_by_addr(domain, backend);
	assert(peer);
	assert(suet_av_insert(&av[0].util_av.av_fid, &raw[2], 1, &other, 0,
			      NULL) == 1);
	assert(suet_av_peer_idx_from_usr_av_addr(&av[0], other) == peer);

	/* Independent public AVs share one stable backend peer under
	 * contention. */
	for (i = 0; i < 2; i++) {
		args[i].av = &av[i];
		args[i].addr.raw_dgram_addr[0] = 44;
		assert(!pthread_create(&threads[i], NULL, insert_peer,
				       &args[i]));
	}
	for (i = 0; i < 2; i++)
		assert(!pthread_join(threads[i], NULL));
	assert(args[0].peer == args[1].peer);
	for (i = 0; i < 2; i++)
		cleanup_public_av(&av[i]);
}

static void check_registration(struct suet_domain *domain)
{
	char buf[32];
	struct iovec iov[] = {{buf, 8}, {buf + 8, 8}, {buf + 16, 16}};
	struct fi_mr_attr attr = {.mr_iov = iov,
				  .iov_count = 1,
				  .access = FI_SEND,
				  .requested_key = 17};
	struct suet_mr public_mr = {0};
	void *handles[3] = {0}, *desc[3] = {0};
	int i;

	assert(!suet_dgram_mr_reg(domain, &attr, 0, false,
				  &public_mr.dgram_ctx));
	assert(mr_last_key == 17 && !domain->dgram.mr_key);
	assert(suet_mr_desc(&public_mr) == public_mr.dgram_ctx);
	assert(!suet_mr_desc(NULL));
	assert(!suet_dgram_mr_close(public_mr.dgram_ctx));
	mr_collisions = 2;
	assert(!suet_mr_regv_internal(domain, iov, 3, 11, FI_SEND, handles,
				      desc));
	assert(mr_live == 2 && mr_lengths[0] == 8 && mr_lengths[1] == 3);
	assert(domain->dgram.mr_key == 4);
	assert(mr_last_key == ((1UL << 31) | 3));
	assert(desc[0] == handles[0] && desc[1] == handles[1]);
	assert(!desc[2] && !handles[2]);
	suet_mr_closev_internal(handles, 3);
	assert(!mr_live);
	memset(desc, 0, sizeof(desc));

	mr_fail_call = mr_calls + 2;
	assert(suet_mr_regv_internal(domain, iov, 3, 32, FI_SEND, handles,
				     desc) == -FI_ENOMEM);
	assert(!mr_live);
	for (i = 0; i < 3; i++)
		assert(!handles[i] && !desc[i]);
	mr_fail_call = 0;
	assert(!suet_mr_regv_internal(domain, iov, 3, 32, FI_SEND, handles,
				      desc));
	assert(mr_live == 3);
	suet_mr_closev_internal(handles, 3);
	assert(!mr_live);
}

int main(void)
{
	struct fid_domain backend = {.ops = &test_domain_ops,
				     .mr = &test_mr_ops};
	struct suet_domain domain = {0};

	ofi_mem_init();
	domain.dgram.domain = &backend;
	domain.util_domain.threading = FI_THREAD_SAFE;
	assert(!suet_dgram_av_init(&domain));
	check_peers(&domain);
	check_registration(&domain);
	suet_dgram_av_cleanup(&domain);
	assert(!domain.dgram.av && av_open_count == av_close_count);
	ofi_mem_fini();
	puts("Datagram resources: stable peers, concurrent AVs and MR "
	     "rollback: PASS");
	return 0;
}
