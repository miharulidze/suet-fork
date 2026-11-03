/* Test-only encapsulation adapter. PDS/SES bytes are never rewritten. */
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "uet_api_private.h"
#include "uet_util.h"
#include "crc32c.h"
#include "udp_backend.h"

static int udp_fd = -1;
static struct sockaddr_in local_addr, peer_addr;
static FILE *capture;

static void mac_for_ip(uint32_t ip, uint8_t *mac)
{
    mac[0] = 2; mac[1] = 0;
    mac[2] = ip >> 24; mac[3] = ip >> 16;
    mac[4] = ip >> 8; mac[5] = ip;
}

void bench_udp_open(const char *ip, const char *path)
{
    socklen_t len = sizeof(local_addr);
    udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (udp_fd < 0) { perror("UDP socket"); exit(1); }
    local_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &local_addr.sin_addr) != 1 ||
        bind(udp_fd, (void *)&local_addr, len) ||
        getsockname(udp_fd, (void *)&local_addr, &len)) {
        perror("UDP bind"); exit(1);
    }
    capture = fopen(path, "w");
    if (!capture) { perror(path); exit(1); }
}

struct sockaddr_in bench_udp_name(void) { return local_addr; }
void bench_udp_peer(struct sockaddr_in peer) { peer_addr = peer; }

static void record(const char *direction, const uint8_t *p, size_t size)
{
    fprintf(capture, "%s %zu ", direction, size);
    for (size_t i = 0; i < size; i++) fprintf(capture, "%02x", p[i]);
    fputc('\n', capture);
    fflush(capture);
}

static int tx(struct uet_nic *nic, void *pkt, void *ip, size_t size)
{
    (void)nic;
    const struct iphdr *hdr = ip;
    /* The reference IP backend places a 4-byte entropy header before
     * PDS. UDP carries entropy in its source port, so remove that outer
     * header along with Ethernet/IP. Never alter PDS or SES. */
    size_t head = (const uint8_t *)ip - (const uint8_t *)pkt +
                  sizeof(*hdr) + sizeof(struct uet_entropy);
    if (hdr->version != 4 || hdr->ihl != 5 || hdr->protocol != UET_IPPROTO ||
        size < head + CRC_LEN || !peer_addr.sin_port) return -EINVAL;
    uint32_t original_crc = crc32c((const uint8_t *)ip + 12,
                                   size - ((const uint8_t *)ip + 12 - (const uint8_t *)pkt) - CRC_LEN);
    if (memcmp(&original_crc, (const uint8_t *)pkt + size - CRC_LEN, CRC_LEN))
        return -EBADMSG;
    /* Reference's mandatory IP-profile CRC covers the IP addresses and
     * entropy envelope. Validate and remove it for this explicitly
     * checksum-free UDP PDS/SES test profile. UDP supplies its checksum. */
    size -= CRC_LEN;
    const uint8_t *payload = (const uint8_t *)pkt + head;
    ssize_t sent = sendto(udp_fd, payload, size - head, 0,
                          (void *)&peer_addr, sizeof(peer_addr));
    if (sent < 0) return -errno;
    if ((size_t)sent != size - head) return -EIO;
    record("TX", payload, size - head);
    return 0;
}

static int rx(struct uet_nic *nic, void *pkt, size_t capacity, size_t *size)
{
    const size_t head = sizeof(struct ethhdr) + sizeof(struct iphdr) +
                        sizeof(struct uet_entropy);
    struct sockaddr_in source;
    socklen_t source_len = sizeof(source);
    if (capacity <= head + CRC_LEN) return -EINVAL;
    ssize_t count = recvfrom(udp_fd, (uint8_t *)pkt + head, capacity - head - CRC_LEN,
                             MSG_TRUNC, (void *)&source, &source_len);
    if (count < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
    if ((size_t)count > capacity - head - CRC_LEN) return -EMSGSIZE;
    if (source.sin_addr.s_addr != peer_addr.sin_addr.s_addr ||
        source.sin_port != peer_addr.sin_port) return -EINVAL;
    record("RX", (uint8_t *)pkt + head, count);
    /* Recreate only the reference backend's outer IP encapsulation. */
    memset(pkt, 0, head);
    struct ethhdr *eth = pkt;
    struct iphdr *ip = (void *)(eth + 1);
    struct uet_entropy *entropy = (void *)(ip + 1);
    memcpy(eth->h_dest, nic->mac_addr, ETH_ALEN);
    mac_for_ip(ntohl(source.sin_addr.s_addr), eth->h_source);
    eth->h_proto = htons(ETH_P_IP);
    ip->version = 4; ip->ihl = 5; ip->ttl = 64;
    ip->protocol = UET_IPPROTO;
    ip->saddr = source.sin_addr.s_addr;
    ip->daddr = local_addr.sin_addr.s_addr;
    ip->tot_len = htons(sizeof(*ip) + sizeof(*entropy) + count + CRC_LEN);
    ip->check = uet_ipv4_csum(ip);
    entropy->entropy = source.sin_port;
    uint32_t crc = crc32c((const uint8_t *)ip + 12,
                          8 + sizeof(*entropy) + count);
    memcpy((uint8_t *)pkt + head + count, &crc, CRC_LEN);
    *size = head + count + CRC_LEN;
    return 1;
}

static int rx_poll(struct uet_nic *nic)
{
    (void)nic;
    struct pollfd pfd = {.fd = udp_fd, .events = POLLIN};
    int rc = poll(&pfd, 1, 0);
    return rc < 0 ? -errno : !!(pfd.revents & POLLIN);
}

static int getinfo(struct uet_nic *nic, struct uet_nic_info *info)
{
    info->ifname = nic->ifname;
    info->network_type = nic->network_type;
    info->mac_addr_str = nic->mac_addr_str;
    info->mtu = nic->mtu;
    info->link_state = UET_NIC_LINK_STATE_UP;
    return 0;
}

static void finalize(struct uet_nic *nic)
{
    (void)nic;
    if (udp_fd >= 0) close(udp_fd);
    udp_fd = -1;
    if (capture) fclose(capture);
    capture = NULL;
}

/* GNU ld wrapping affects only the separately compiled test executable.
 * Upstream reference-provider sources and SUET sources remain intact. */
int __wrap_uet_nic_initialize(struct uet_nic *nic)
{
    if (udp_fd < 0) return -EINVAL;
    strcpy(nic->ifname, "test-udp");
    strcpy(nic->network_type, "UDP test encapsulation");
    nic->ipv4_addr = ntohl(local_addr.sin_addr.s_addr);
    inet_ntop(AF_INET, &local_addr.sin_addr, nic->ipv4_addr_str,
              sizeof(nic->ipv4_addr_str));
    nic->has_ipv4 = true; nic->has_ipv6 = false;
    mac_for_ip(nic->ipv4_addr, nic->mac_addr);
    snprintf(nic->mac_addr_str, sizeof(nic->mac_addr_str),
             "02:00:%02x:%02x:%02x:%02x", nic->mac_addr[2], nic->mac_addr[3],
             nic->mac_addr[4], nic->mac_addr[5]);
    nic->l2_hdr_size = sizeof(struct ethhdr);
    nic->mtu = 8192;
    nic->max_pkt_size = nic->mtu + nic->l2_hdr_size;
    nic->min_pkt_size = UET_MIN_PKT_SIZE;
    nic->min_ip_pkt_size = nic->min_pkt_size - nic->l2_hdr_size;
    nic->sock_fd = udp_fd;
    nic->nic_getinfo = getinfo; nic->nic_tx_pkt = tx; nic->nic_rx_pkt = rx;
    nic->nic_rx_poll = rx_poll; nic->nic_finalize = finalize;
    return 0;
}

int __wrap_uet_nic_resolve_ipv4_nh(struct uet_nic *nic, int fd,
                                 uint32_t destination, uint8_t *mac)
{
    (void)nic; (void)fd;
    mac_for_ip(destination, mac);
    return 0;
}
