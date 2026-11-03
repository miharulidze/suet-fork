/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#ifndef INTEROP_UD_BACKEND_H
#define INTEROP_UD_BACKEND_H
#include <stdint.h>
#include <stddef.h>
/* One peer per process. The NIC transports the reference's complete frames. */
int ud_name(void *raw, size_t *length);
int ud_peer(const void *raw, uint32_t ipv4);
#endif
