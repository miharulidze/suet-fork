/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Shared reference NIC: PDS/SES packets over libfabric FI_EP_DGRAM. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "uet_api_private.h"
#include "uet_util.h"
#include "crc32c.h" /* CRC_LEN only; this backend performs no CRC calculation. */
#include "dgram_backend.h"

#define MAX_DEPTH 1024
struct slot { struct fi_context ctx; unsigned char *buf; int busy; };
static struct {
    struct fi_info *info;
    struct fid_fabric *fabric;
    struct fid_domain *domain;
    struct fid_ep *ep;
    struct fid_cq *txcq, *rxcq;
    struct fid_av *av;
    struct fid_mr *mr;
    void *desc;
    void *arena;
    size_t stride, prefix, tx_prefix, mtu;
    FILE *capture;
    struct slot tx[MAX_DEPTH], rx[MAX_DEPTH];
    struct fi_cq_data_entry ready[32];
    size_t cursor, available, next_tx, depth;
    fi_addr_t peer;
    uint32_t peer_ip;
    uint64_t tx_pkts, rx_pkts, tx_bytes, rx_bytes;
} ud;

static void require(int result, const char *op)
{
    if (result < 0) { fprintf(stderr,"UD backend %s: %s (%d)\n",op,fi_strerror(-result),result); abort(); }
}

static void tx_progress(void)
{
    struct fi_cq_entry entries[32];
    ssize_t count=fi_cq_read(ud.txcq,entries,32);
    if (count==-FI_EAGAIN) return;
    require(count,"TX CQ");
    for (ssize_t i=0;i<count;i++) ((struct slot *)entries[i].op_context)->busy=0;
}

static int rx_progress(void)
{
    if (ud.cursor<ud.available) return 1;
    ssize_t count=fi_cq_read(ud.rxcq,ud.ready,32);
    if (count==-FI_EAGAIN) return 0;
    require(count,"RX CQ");
    ud.cursor=0; ud.available=count;
    return count>0;
}

int ud_name(void *raw,size_t *length) { return fi_getname(&ud.ep->fid,raw,length); }
int ud_peer(const void *raw,uint32_t ipv4)
{
    int n=fi_av_insert(ud.av,raw,1,&ud.peer,0,NULL);
    if (n!=1) return n<0?n:-FI_EINVAL;
    ud.peer_ip=ipv4;
    return 0;
}

static void mac_for_ip(uint32_t ip,uint8_t *m);

static void record(const char *direction, const uint8_t *bytes, size_t length)
{
    if (!ud.capture) return;
    fprintf(ud.capture, "%s %zu ", direction, length);
    for (size_t i=0;i<length;i++) fprintf(ud.capture,"%02x",bytes[i]);
    fputc('\n',ud.capture);
    fflush(ud.capture);
}

static int tx(struct uet_nic *nic,void *frame,void *ip,size_t length)
{
    (void)nic;
    if (!ud.peer_ip || ((struct iphdr *)ip)->daddr!=htonl(ud.peer_ip)) return -EINVAL;
    /* FI_EP_DGRAM carries PDS/SES, exactly as the native SUET provider.
     * The reference NIC's internal IP/entropy/zero-CRC envelope stops here. */
    if ((const unsigned char *)ip-(const unsigned char *)frame != sizeof(struct ethhdr)) return -EINVAL;
    struct iphdr *hdr=ip;
    size_t head=sizeof(struct ethhdr)+sizeof(*hdr)+sizeof(struct uet_entropy);
    if (hdr->version!=4 || hdr->ihl!=5 || hdr->protocol!=UET_IPPROTO || length<head+CRC_LEN) return -EINVAL;
    length-=head+CRC_LEN;
    if (length>ud.mtu) return -EMSGSIZE;
    const uint8_t *payload=(const uint8_t *)frame+head;
    tx_progress();
    struct slot *s=&ud.tx[ud.next_tx];
    if (s->busy) return -EAGAIN;
    memcpy(s->buf+ud.tx_prefix,payload,length);
    int rc=fi_send(ud.ep,s->buf,length+ud.tx_prefix,ud.desc,ud.peer,&s->ctx);
    if (rc) return rc;
    s->busy=1; ud.next_tx=(ud.next_tx+1)%ud.depth;
    ud.tx_pkts++; ud.tx_bytes+=length;
    record("TX",payload,length);
    return 0;
}

static int rx(struct uet_nic *nic,void *frame,size_t capacity,size_t *length)
{
    (void)nic;
    if (!rx_progress()) return 0;
    struct fi_cq_data_entry *e=&ud.ready[ud.cursor++];
    struct slot *s=e->op_context;
    const size_t head=sizeof(struct ethhdr)+sizeof(struct iphdr)+sizeof(struct uet_entropy);
    if (e->len<ud.prefix || e->len-ud.prefix+head+CRC_LEN>capacity) return -EMSGSIZE;
    size_t bytes=e->len-ud.prefix;
    *length=bytes+head+CRC_LEN;
    memset(frame,0,head);
    struct ethhdr *eth=frame;
    memcpy(eth->h_dest,nic->mac_addr,ETH_ALEN);
    mac_for_ip(ud.peer_ip,eth->h_source);
    eth->h_proto=htons(ETH_P_IP);
    struct iphdr *ip=(void *)(eth+1);
    struct uet_entropy *entropy=(void *)(ip+1);
    ip->version=4;ip->ihl=5;ip->ttl=64;ip->protocol=UET_IPPROTO;
    ip->saddr=htonl(ud.peer_ip);ip->daddr=htonl(nic->ipv4_addr);
    ip->tot_len=htons(sizeof(*ip)+sizeof(*entropy)+bytes+CRC_LEN);
    ip->check=uet_ipv4_csum(ip);
    entropy->entropy=htons(0x4242);
    memcpy((uint8_t *)frame+head,s->buf+ud.prefix,bytes);
    memset((uint8_t *)frame+head+bytes,0,CRC_LEN);
    record("RX",s->buf+ud.prefix,bytes);
    require(fi_recv(ud.ep,s->buf,ud.stride,ud.desc,FI_ADDR_UNSPEC,&s->ctx),"repost RX");
    ud.rx_pkts++; ud.rx_bytes+=bytes;
    return 1;
}
static int poll_rx(struct uet_nic *nic) { (void)nic; tx_progress(); return rx_progress(); }
static int getinfo(struct uet_nic *nic,struct uet_nic_info *i)
{
    i->ifname=nic->ifname; i->network_type=nic->network_type;
    i->mac_addr_str=nic->mac_addr_str; i->mtu=nic->mtu;
    i->link_state=UET_NIC_LINK_STATE_UP; return 0;
}
static void finalize(struct uet_nic *nic)
{
    (void)nic;
    fprintf(stderr,"UD_BACKEND tx=%"PRIu64" rx=%"PRIu64" tx_bytes=%"PRIu64" rx_bytes=%"PRIu64"\n",ud.tx_pkts,ud.rx_pkts,ud.tx_bytes,ud.rx_bytes);
    if (ud.ep) require(fi_close(&ud.ep->fid),"close endpoint");
    if (ud.mr) require(fi_close(&ud.mr->fid),"close MR");
    if (ud.av) require(fi_close(&ud.av->fid),"close AV");
    if (ud.txcq) require(fi_close(&ud.txcq->fid),"close TX CQ");
    if (ud.rxcq) require(fi_close(&ud.rxcq->fid),"close RX CQ");
    if (ud.domain) require(fi_close(&ud.domain->fid),"close domain");
    if (ud.fabric) require(fi_close(&ud.fabric->fid),"close fabric");
    if (ud.capture) fclose(ud.capture);
    fi_freeinfo(ud.info); free(ud.arena); memset(&ud,0,sizeof(ud));
}
static void mac_for_ip(uint32_t ip,uint8_t *m)
{ m[0]=2; m[1]=0; m[2]=ip>>24; m[3]=ip>>16; m[4]=ip>>8; m[5]=ip; }

int __wrap_uet_nic_initialize(struct uet_nic *nic)
{
    const char *source=getenv("UET_UD_SOURCE");
    const char *provider=getenv("UET_DGRAM_PROVIDER");
    const char *domain=getenv("UET_DGRAM_DOMAIN");
    const char *disabled=getenv("UET_BENCH_DISABLE_CRC");
    if (!provider) provider="verbs";
    if (strcmp(provider,"verbs") && strcmp(provider,"udp")) return -EINVAL;
    if (!disabled || strcmp(disabled,"1")) {
        fprintf(stderr,"Shared PDS/SES DGRAM backend requires UET_BENCH_DISABLE_CRC=1\n");
        return -EINVAL;
    }
    struct in_addr ip;
    if (!source || inet_pton(AF_INET,source,&ip)!=1) return -EINVAL;
    struct fi_info *h=fi_allocinfo();
    if (!h) return -ENOMEM;
    h->caps=FI_MSG; h->mode=FI_CONTEXT|FI_MSG_PREFIX|FI_LOCAL_MR;
    h->ep_attr->type=FI_EP_DGRAM;
    h->domain_attr->threading=FI_THREAD_DOMAIN;
    h->domain_attr->mr_mode=FI_MR_LOCAL|FI_MR_VIRT_ADDR|FI_MR_ALLOCATED|FI_MR_PROV_KEY;
    h->fabric_attr->prov_name=strdup(provider);
    if (domain || !strcmp(provider,"verbs")) h->domain_attr->name=strdup(domain?domain:"mlx5_0-dgram");
    h->tx_attr->size=h->rx_attr->size=384;
    int rc=fi_getinfo(FI_VERSION(1,20),source,!strcmp(provider,"udp")?"0":NULL,FI_SOURCE,h,&ud.info);
    fi_freeinfo(h);
    if (rc) return rc;
    if (strcmp(ud.info->fabric_attr->prov_name,provider) || ud.info->ep_attr->type!=FI_EP_DGRAM) return -EINVAL;
    require(fi_fabric(ud.info->fabric_attr,&ud.fabric,NULL),"fabric");
    require(fi_domain(ud.fabric,ud.info,&ud.domain,NULL),"domain");
    ud.depth=ud.info->tx_attr->size<ud.info->rx_attr->size?ud.info->tx_attr->size:ud.info->rx_attr->size;
    if (!ud.depth || ud.depth>MAX_DEPTH) return -EINVAL;
    struct fi_cq_attr cq={.format=FI_CQ_FORMAT_CONTEXT,.size=ud.depth,.wait_obj=FI_WAIT_NONE};
    require(fi_cq_open(ud.domain,&cq,&ud.txcq,NULL),"TX CQ open");
    cq.format=FI_CQ_FORMAT_DATA;
    require(fi_cq_open(ud.domain,&cq,&ud.rxcq,NULL),"RX CQ open");
    struct fi_av_attr av={.type=FI_AV_TABLE,.count=4};
    require(fi_av_open(ud.domain,&av,&ud.av,NULL),"AV open");
    ud.info->tx_attr->size=ud.info->rx_attr->size=ud.depth;
    require(fi_endpoint(ud.domain,ud.info,&ud.ep,NULL),"endpoint");
    require(fi_ep_bind(ud.ep,&ud.av->fid,0),"bind AV");
    require(fi_ep_bind(ud.ep,&ud.txcq->fid,FI_SEND),"bind TX CQ");
    require(fi_ep_bind(ud.ep,&ud.rxcq->fid,FI_RECV),"bind RX CQ");
    require(fi_enable(ud.ep),"enable");
    ud.prefix=(ud.info->rx_attr->mode&FI_MSG_PREFIX)?ud.info->ep_attr->msg_prefix_size:0;
    ud.tx_prefix=(ud.info->tx_attr->mode&FI_MSG_PREFIX)?ud.info->ep_attr->msg_prefix_size:0;
    ud.stride=(ud.info->ep_attr->max_msg_size+ud.prefix+63)&~63ULL;
    ud.mtu=ud.info->ep_attr->max_msg_size;
    if (ud.mtu>4096) ud.mtu=4096;
    if (posix_memalign(&ud.arena,64,2*ud.depth*ud.stride)) return -ENOMEM;
    if (ud.info->domain_attr->mr_mode&FI_MR_LOCAL) {
        require(fi_mr_reg(ud.domain,ud.arena,2*ud.depth*ud.stride,FI_SEND|FI_RECV,0,0,0,&ud.mr,NULL),"MR");
        ud.desc=fi_mr_desc(ud.mr);
    }
    for (size_t i=0;i<ud.depth;i++) {
        ud.tx[i].buf=(unsigned char *)ud.arena+i*ud.stride;
        ud.rx[i].buf=(unsigned char *)ud.arena+(ud.depth+i)*ud.stride;
        require(fi_recv(ud.ep,ud.rx[i].buf,ud.stride,ud.desc,FI_ADDR_UNSPEC,&ud.rx[i].ctx),"post RX");
    }
    snprintf(nic->ifname,sizeof(nic->ifname),"dgram-%s",provider);
    snprintf(nic->network_type,sizeof(nic->network_type),"libfabric %s FI_EP_DGRAM",provider);
    nic->ipv4_addr=ntohl(ip.s_addr); nic->has_ipv4=true; nic->has_ipv6=false;
    snprintf(nic->ipv4_addr_str,sizeof(nic->ipv4_addr_str),"%s",source);
    mac_for_ip(nic->ipv4_addr,nic->mac_addr);
    snprintf(nic->mac_addr_str,sizeof(nic->mac_addr_str),"02:00:%02x:%02x:%02x:%02x",nic->mac_addr[2],nic->mac_addr[3],nic->mac_addr[4],nic->mac_addr[5]);
    nic->l2_hdr_size=sizeof(struct ethhdr);
    nic->mtu=ud.mtu+sizeof(struct iphdr)+sizeof(struct uet_entropy)+CRC_LEN;
    nic->max_pkt_size=nic->mtu+nic->l2_hdr_size; nic->min_pkt_size=UET_MIN_PKT_SIZE;
    nic->min_ip_pkt_size=nic->min_pkt_size-nic->l2_hdr_size; nic->sock_fd=-1;
    nic->nic_getinfo=getinfo; nic->nic_tx_pkt=tx; nic->nic_rx_pkt=rx;
    nic->nic_rx_poll=poll_rx; nic->nic_finalize=finalize;
    const char *capture=getenv("UET_DGRAM_CAPTURE");
    if (capture && *capture) { ud.capture=fopen(capture,"w"); if (!ud.capture) return -errno; }
    fprintf(stderr,"UD_BACKEND provider=%s domain=%s type=FI_EP_DGRAM prefix=%zu source=%s queue_depth=%zu wire=pds-ses mtu=%zu\n",ud.info->fabric_attr->prov_name,ud.info->domain_attr->name,ud.prefix,source,ud.depth,ud.mtu);
    return 0;
}
int __wrap_uet_nic_resolve_ipv4_nh(struct uet_nic *nic,int fd,uint32_t ip,uint8_t *mac)
{ (void)nic; (void)fd; mac_for_ip(ip,mac); return 0; }
