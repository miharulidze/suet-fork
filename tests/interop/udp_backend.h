/* Test-only socket transport for the reference provider NIC interface. */
#ifndef INTEROP_UDP_BACKEND_H
#define INTEROP_UDP_BACKEND_H
#include <netinet/in.h>
void bench_udp_open(const char *ip, const char *capture);
struct sockaddr_in bench_udp_name(void);
void bench_udp_peer(struct sockaddr_in peer);
#endif
