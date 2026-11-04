/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Mock only the transport, bitmap and teardown hooks. The tested replay
 * functions are extracted unchanged from the patched reference source. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#define UET_PDS_PSN_AFTER(a, b) ((int32_t)((a) - (b)) > 0)
#define UET_PDS_NACK_FLAGS_RETX 0x10
#define UET_PDS_WARN(...) ((void)0)
typedef void *uet_pkt_handle_t;
struct dlist_entry { unsigned unused; };
struct uet_pdc_pkt {
    uint32_t psn;
    bool tx_pkt_acked;
    unsigned tx_retry_cnt;
    uet_pkt_handle_t tx_pkt_handle;
    struct dlist_entry node;
};
struct bitmap { struct uet_pdc_pkt *packets[128]; };
struct uet_pdc {
    bool gbn_pending, gbn_active, gbn_retx_seen;
    uint32_t gbn_expected_psn, gbn_next_psn, gbn_end_psn;
    uint32_t tx_bm_base_psn, next_psn;
    unsigned pdc_id;
    struct bitmap *tx_bm;
    struct dlist_entry tx_pkt_list_head;
};
struct uet_instance { struct { unsigned max_tx_retries; } pds; };
struct uet_parsed_pkt { uint32_t pds_payload, pds_psn; unsigned pds_flags; };
static uint32_t wire[1024];
static unsigned wire_count, attempts;
static int fail_attempt, fail_errno, closed, dropped;
static bool bm_get(struct bitmap *bm, uint32_t index, void **out)
{
    assert(index < 128);
    *out = bm->packets[index];
    return *out != NULL;
}
static int uet_pds_rtx_pkt(struct uet_instance *uet, struct uet_pdc *pdc,
                           struct uet_pdc_pkt *pkt)
{
    (void)uet; (void)pdc;
    if ((int)attempts++ == fail_attempt) return fail_errno;
    assert(wire_count < 1024);
    wire[wire_count++] = pkt->psn;
    pkt->tx_retry_cnt++;
    return 0;
}
static void dlist_remove(struct dlist_entry *p) { (void)p; }
static void dlist_insert_tail(struct dlist_entry *p, struct dlist_entry *h)
{ (void)p; (void)h; }
static void uet_pds_close_pdc_in_error(struct uet_instance *u, struct uet_pdc *p)
{ (void)u; (void)p; closed++; }
static void uet_pds_drop_tx_pkts(void *unused, void *failed)
{ (void)unused; assert(failed == (void *)1); dropped++; }

#include "reference_gbn_functions.h"

static struct uet_instance instance = {.pds.max_tx_retries = 5};
static struct uet_pdc_pkt packets[128];
static struct bitmap bitmap;
static struct uet_pdc pdc;
static void reset(uint32_t base, unsigned count)
{
    memset(packets, 0, sizeof(packets));
    memset(&bitmap, 0, sizeof(bitmap));
    pdc = (struct uet_pdc){.tx_bm = &bitmap, .tx_bm_base_psn = base,
                           .next_psn = base + count};
    for (unsigned i = 0; i < count; i++) {
        packets[i].psn = base + i;
        packets[i].tx_pkt_handle = (void *)1;
        bitmap.packets[i] = &packets[i];
    }
    wire_count = attempts = closed = dropped = 0;
    fail_attempt = -1;
    fail_errno = -EAGAIN;
}
static void drain(void)
{
    for (unsigned i = 0; pdc.gbn_pending; i++) {
        assert(i < 10);
        assert(!uet_pds_progress_gbn(&instance, &pdc));
    }
}
static void ordered(uint32_t base)
{
    reset(base, 80);
    struct uet_parsed_pkt nack = {.pds_payload = base, .pds_psn = base + 60};
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    assert(pdc.gbn_pending && pdc.gbn_active && wire_count == 32);
    unsigned cursor = pdc.gbn_next_psn;
    /* Duplicate NACKs must not rewind a pending replay. */
    for (int i = 0; i < 10; i++) assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    assert(pdc.gbn_next_psn == cursor && wire_count == 32);
    drain();
    assert(wire_count == 80 && pdc.gbn_active);
    for (unsigned i = 0; i < 80; i++) assert(wire[i] == base + i);
    /* One retransmit-loss event may restart; repeated RETX feedback coalesces. */
    nack.pds_flags = UET_PDS_NACK_FLAGS_RETX;
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    assert(wire_count == 112);
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    assert(wire_count == 112);
    drain();
    assert(wire_count == 160);
    for (unsigned i = 0; i < 80; i++) assert(wire[80 + i] == base + i);
}
int main(void)
{
    ordered(1000);
    ordered(UINT32_MAX - 40); /* replay across PSN wrap */
    reset(100, 8);
    struct uet_parsed_pkt nack = {.pds_payload = 100, .pds_psn = 107};
    fail_attempt = 2;
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    assert(wire_count == 2 && pdc.gbn_next_psn == 102 && pdc.gbn_pending);
    drain();
    assert(wire_count == 8);
    for (unsigned i = 0; i < 8; i++) assert(wire[i] == 100 + i);
    reset(100, 8);
    bitmap.packets[2] = NULL;
    packets[4].tx_pkt_acked = true;
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    drain();
    const uint32_t wanted[] = {100, 101, 103, 105, 106, 107};
    assert(wire_count == 6 && !memcmp(wire, wanted, sizeof(wanted)));
    /* ACK/window advancement during backpressure skips freed descriptors. */
    reset(100, 80);
    nack = (struct uet_parsed_pkt){.pds_payload = 100, .pds_psn = 179};
    fail_attempt = 2;
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    pdc.tx_bm_base_psn = 104;
    for (unsigned i = 0; i < 128; i++)
        bitmap.packets[i] = i < 76 ? &packets[i + 4] : NULL;
    drain();
    assert(wire_count == 78 && wire[0] == 100 && wire[1] == 101);
    for (unsigned i = 2; i < wire_count; i++) assert(wire[i] == 102 + i);
    /* A higher Expected_PSN reports a distinct loss before any ACK. */
    reset(100, 8);
    nack = (struct uet_parsed_pkt){.pds_payload = 100, .pds_psn = 107};
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    nack.pds_payload = 104;
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack));
    assert(wire_count == 12);
    for (unsigned i = 8; i < 12; i++) assert(wire[i] == 104 + i - 8);
    /* Reject stale and future expected PSNs without transmitting. */
    reset(100, 8);
    nack.pds_payload = 99;
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack) && !wire_count);
    nack.pds_payload = 108;
    assert(!uet_pds_start_gbn(&instance, &pdc, &nack) && !wire_count);
    /* Retry limits and hard NIC errors must propagate, without raising limits. */
    for (int hard = 0; hard < 2; hard++) {
        reset(100, 8); nack.pds_payload = 100;
        if (hard) { fail_attempt = 0; fail_errno = -EIO; }
        else packets[0].tx_retry_cnt = 5;
        assert(uet_pds_start_gbn(&instance, &pdc, &nack) == -EPROTO);
        assert(closed == 1 && dropped == 1 && !wire_count);
    }
    puts("ROD GoBackN: ordered replay, wrap, coalescing, backpressure, holes and errors PASS");
}
