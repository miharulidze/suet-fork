/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef HTSIM_DGRAM_H
#define HTSIM_DGRAM_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTSIM_DGRAM_MTU 4096
/* Simulator-owned byte copy. No provider/transport pointers cross the network. */
struct htsim_dgram_frame {
    struct sockaddr_in src, dst;
    size_t size;
    uint64_t rx_metadata;
    unsigned char data[HTSIM_DGRAM_MTU];
};

/* Single-threaded simulator bridge. Configure before fi_getinfo. Taking a
 * frame completes its local send; network delivery is a separate event.
 * No progress operation advances simulated time or uses real sockets.
 */
int htsim_dgram_configure(uint32_t hosts, size_t queue_size);
int htsim_dgram_take_tx(struct htsim_dgram_frame *frame);
int htsim_dgram_deliver(const struct htsim_dgram_frame *frame);
size_t htsim_dgram_pending(void);
uint32_t htsim_dgram_host(const struct sockaddr_in *addr);

#ifdef __cplusplus
}
#endif
#endif
