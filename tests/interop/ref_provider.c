/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/* Minimal FI_MSG adapter for stock fabtests; not a production provider. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <rdma/providers/fi_prov.h>
#include "uet_api.h"
#include "dgram_backend.h"
#include "ref_address.h"

#define NAME "uet_ref"
#define MAGIC REF_ADDRESS_MAGIC
struct fabric { struct fid_fabric fid; uet_handle_t uet; };
struct domain { struct fid_domain fid; struct fabric *fabric; struct fi_info *info; uet_domain_handle_t uet; };
struct cq { struct fid_cq fid; struct fi_cq_attr attr; uet_cq_handle_t uet; };
struct av { struct fid_av fid; struct domain *domain; uet_addr_handle_t peer; struct ref_address name; };
struct endpoint { struct fid_ep fid; struct domain *domain; struct av *av; struct fi_info *info; uet_ep_handle_t uet; };
static int no_bind(struct fid *a,struct fid *b,uint64_t f) { (void)a;(void)b;(void)f;return -FI_ENOSYS; }
static int no_control(struct fid *a,int cmd,void *p) { (void)a;(void)cmd;(void)p;return -FI_ENOSYS; }
static int no_open(struct fid *a,const char *n,uint64_t f,void **o,void *c) { (void)a;(void)n;(void)f;(void)o;(void)c;return -FI_ENOSYS; }
#define FID_OPS(fn) { .size=sizeof(struct fi_ops),.close=fn,.bind=no_bind,.control=no_control,.ops_open=no_open }

static int cq_close(fid_t fid) { struct cq *q=(void *)fid; free(q); return 0; }
static ssize_t cq_read(struct fid_cq *fid,void *buf,size_t count)
{
    struct cq *q=(void *)fid;
    if (!q->uet) return -FI_EAGAIN;
    ssize_t rc=uet_cq_read(q->uet,buf,count);
    return rc?rc:-FI_EAGAIN;
}
static ssize_t cq_readerr(struct fid_cq *fid,struct fi_cq_err_entry *err,uint64_t flags)
{ (void)flags;return uet_cq_readerr(((struct cq *)fid)->uet,err); }
static const char *cq_strerror(struct fid_cq *q,int e,const void *d,char *b,size_t n)
{ (void)q;(void)d;const char *s=fi_strerror(e);if(b&&n)snprintf(b,n,"%s",s);return s; }
static struct fi_ops cq_fid_ops=FID_OPS(cq_close);
static struct fi_ops_cq cq_ops={.size=sizeof(struct fi_ops_cq),.read=cq_read,.readerr=cq_readerr,.strerror=cq_strerror};
static int cq_open(struct fid_domain *d,struct fi_cq_attr *attr,struct fid_cq **out,void *ctx)
{
    (void)d;struct cq *q=calloc(1,sizeof(*q));if(!q)return -FI_ENOMEM;
    q->attr=*attr;q->attr.wait_obj=FI_WAIT_NONE;
    if(q->attr.format==FI_CQ_FORMAT_UNSPEC)q->attr.format=FI_CQ_FORMAT_CONTEXT;
    q->fid.fid=(struct fid){.fclass=FI_CLASS_CQ,.context=ctx,.ops=&cq_fid_ops};q->fid.ops=&cq_ops;
    *out=&q->fid;return 0;
}
static int av_close(fid_t fid)
{ struct av *a=(void *)fid;int rc=a->peer?uet_av_remove(a->peer):0;if(!rc)free(a);return rc; }
static int av_insert(struct fid_av *fid,const void *addresses,size_t count,fi_addr_t *handles,uint64_t flags,void *ctx)
{
    (void)flags;(void)ctx;struct av *a=(void *)fid;const struct ref_address *n=addresses;
    if(count!=1 || n->magic!=MAGIC || n->raw_len>sizeof(n->raw))return -FI_EINVAL;
    if(a->peer)return -FI_ENOSYS;
    int rc=ud_peer(n->raw,n->uet.fa.v4);if(rc)return rc;
    a->name=*n;
    rc=uet_av_insert(a->domain->uet,&a->name.uet,&a->peer);
    if(rc)return rc;handles[0]=0;return 1;
}
static int av_lookup(struct fid_av *fid,fi_addr_t h,void *buf,size_t *len)
{ (void)h;struct av *a=(void *)fid;if(*len<sizeof(a->name)){*len=sizeof(a->name);return -FI_ETOOSMALL;}memcpy(buf,&a->name,sizeof(a->name));*len=sizeof(a->name);return 0; }
static int av_remove(struct fid_av *fid,fi_addr_t *h,size_t n,uint64_t flags)
{ (void)h;(void)n;(void)flags;struct av *a=(void *)fid;int rc=a->peer?uet_av_remove(a->peer):0;if(!rc)a->peer=NULL;return rc; }
static const char *av_straddr(struct fid_av *a,const void *p,char *b,size_t *n)
{ (void)a;(void)p;snprintf(b,*n,"reference-over-UD");return b; }
static struct fi_ops av_fid_ops=FID_OPS(av_close);
static struct fi_ops_av av_ops={.size=sizeof(struct fi_ops_av),.insert=av_insert,.lookup=av_lookup,.remove=av_remove,.straddr=av_straddr};
static int av_open(struct fid_domain *fid,struct fi_av_attr *attr,struct fid_av **out,void *ctx)
{
    (void)attr;struct av *a=calloc(1,sizeof(*a));if(!a)return -FI_ENOMEM;
    a->domain=(void *)fid;a->fid.fid=(struct fid){.fclass=FI_CLASS_AV,.context=ctx,.ops=&av_fid_ops};a->fid.ops=&av_ops;*out=&a->fid;return 0;
}
static int ep_bind(struct fid *fid,struct fid *other,uint64_t flags)
{
    struct endpoint *e=(void *)fid;
    if(other->fclass==FI_CLASS_AV){e->av=(void *)other;return 0;}
    if(other->fclass==FI_CLASS_CQ){struct cq *q=(void *)other;return uet_ep_bind_cq(e->uet,&q->attr,&q->fid,flags,q->fid.fid.context,&q->uet);}
    return -FI_ENOSYS;
}
static int ep_control(struct fid *fid,int cmd,void *arg)
{ (void)arg;return cmd==FI_ENABLE?uet_ep_enable(((struct endpoint *)fid)->uet):-FI_ENOSYS; }
static int ep_close(fid_t fid)
{ struct endpoint *e=(void *)fid;int rc=uet_ep_close(e->uet);if(!rc){fi_freeinfo(e->info);free(e);}return rc; }
static ssize_t recv_(struct fid_ep *fid,void *buf,size_t len,void *desc,fi_addr_t addr,void *ctx)
{ (void)desc;(void)addr;return uet_recv(((struct endpoint *)fid)->uet,UET_JOB_ID_ANY,buf,len,NULL,NULL,ctx); }
static ssize_t recvv_(struct fid_ep *fid,const struct iovec *iov,void **desc,size_t n,fi_addr_t addr,void *ctx)
{ (void)desc;(void)addr;return uet_recvv(((struct endpoint *)fid)->uet,UET_JOB_ID_ANY,iov,n,NULL,NULL,ctx); }
static ssize_t recvmsg_(struct fid_ep *fid,const struct fi_msg *m,uint64_t flags)
{ if(flags&~FI_COMPLETION)return -FI_ENOSYS;return recvv_(fid,m->msg_iov,m->desc,m->iov_count,m->addr,m->context); }
static ssize_t sendv_(struct fid_ep *fid,const struct iovec *iov,void **desc,size_t n,fi_addr_t addr,void *ctx)
{
    (void)desc;struct endpoint *e=(void *)fid;
    if(addr!=0 || !e->av || !e->av->peer)return -FI_EADDRNOTAVAIL;
    return uet_sendv(e->uet,0,iov,n,NULL,e->av->peer,ctx,e->av->name.uet.start_index);
}
static ssize_t send_(struct fid_ep *fid,const void *b,size_t len,void *desc,fi_addr_t addr,void *ctx)
{ struct iovec iov={(void *)b,len};(void)desc;return sendv_(fid,&iov,NULL,1,addr,ctx); }
static ssize_t sendmsg_(struct fid_ep *fid,const struct fi_msg *m,uint64_t flags)
{
    if(flags&~(FI_COMPLETION|FI_TRANSMIT_COMPLETE|FI_DELIVERY_COMPLETE))return -FI_ENOSYS;
    struct endpoint *e=(void *)fid;uint64_t saved=e->info->tx_attr->op_flags;
    e->info->tx_attr->op_flags=FI_COMPLETION|flags;
    ssize_t rc=sendv_(fid,m->msg_iov,m->desc,m->iov_count,m->addr,m->context);
    e->info->tx_attr->op_flags=saved;return rc;
}
static int getname(fid_t fid,void *buf,size_t *len)
{
    if(*len<sizeof(struct ref_address)){*len=sizeof(struct ref_address);return -FI_ETOOSMALL;}
    struct ref_address a={.magic=MAGIC,.raw_len=sizeof(a.raw)};
    int rc=uet_getname(((struct endpoint *)fid)->uet,&a.uet);if(rc)return rc;
    size_t raw_len=sizeof(a.raw);rc=ud_name(a.raw,&raw_len);if(rc)return rc;a.raw_len=raw_len;
    memcpy(buf,&a,sizeof(a));*len=sizeof(a);return 0;
}
static int getopt_(fid_t f,int level,int name,void *value,size_t *len)
{ (void)f;(void)level;(void)name;(void)value;(void)len;return -FI_ENOPROTOOPT; }
static int setopt_(fid_t f,int level,int name,const void *value,size_t len)
{ (void)f;(void)level;(void)name;(void)value;(void)len;return -FI_ENOPROTOOPT; }
static struct fi_ops ep_fid_ops={.size=sizeof(struct fi_ops),.close=ep_close,.bind=ep_bind,.control=ep_control,.ops_open=no_open};
static struct fi_ops_ep ep_ops={.size=sizeof(struct fi_ops_ep),.getopt=getopt_,.setopt=setopt_};
static struct fi_ops_cm cm_ops={.size=sizeof(struct fi_ops_cm),.getname=getname};
static struct fi_ops_msg msg_ops={.size=sizeof(struct fi_ops_msg),.recv=recv_,.recvv=recvv_,.recvmsg=recvmsg_,.send=send_,.sendv=sendv_,.sendmsg=sendmsg_};
static int endpoint_open(struct fid_domain *fid,struct fi_info *info,struct fid_ep **out,void *ctx)
{
    struct domain *d=(void *)fid;struct endpoint *e=calloc(1,sizeof(*e));if(!e)return -FI_ENOMEM;
    e->domain=d;e->info=fi_dupinfo(d->info);if(!e->info){free(e);return -FI_ENOMEM;}
    e->info->tx_attr->size=info->tx_attr->size;e->info->rx_attr->size=info->rx_attr->size;
    e->info->tx_attr->msg_order=info->tx_attr->msg_order;e->info->rx_attr->msg_order=info->rx_attr->msg_order;
    e->info->tx_attr->op_flags=info->tx_attr->op_flags|FI_COMPLETION;
    e->info->rx_attr->op_flags=FI_COMPLETION;
    e->fid.fid=(struct fid){.fclass=FI_CLASS_EP,.context=ctx,.ops=&ep_fid_ops};
    e->fid.ops=&ep_ops;e->fid.msg=&msg_ops;e->fid.cm=&cm_ops;
    const char *setting=getenv("UET_BENCH_PAYLOAD_MTU");
    uint32_t payload=setting?(uint32_t)strtoul(setting,NULL,10):(uint32_t)uet_max_payload_len(d->uet);
    int rc=uet_endpoint(d->uet,e->info,&e->fid,ctx,&e->uet,0,0,16,0,false,false,payload);
    fprintf(stderr,"REF_PROFILE payload_mtu=%u\n",payload);
    if(rc){fi_freeinfo(e->info);free(e);return rc;}
    *out=&e->fid;return 0;
}
static int domain_close(fid_t fid)
{ struct domain *d=(void *)fid;int rc=uet_domain_close(d->uet);if(!rc){fi_freeinfo(d->info);free(d);}return rc; }
static struct fi_ops domain_fid_ops=FID_OPS(domain_close);
static struct fi_ops_domain domain_ops={.size=sizeof(struct fi_ops_domain),.av_open=av_open,.cq_open=cq_open,.endpoint=endpoint_open};
static int domain_open(struct fid_fabric *fid,struct fi_info *info,struct fid_domain **out,void *ctx)
{
    (void)info;struct fabric *f=(void *)fid;struct domain *d=calloc(1,sizeof(*d));if(!d)return -FI_ENOMEM;
    int rc=f->uet?-FI_EBUSY:uet_initialize(&f->uet);if(rc){free(d);return rc;}
    d->fabric=f;rc=uet_getinfo(f->uet,NULL,NULL,&d->info);if(rc){free(d);return rc;}
    d->fid.fid=(struct fid){.fclass=FI_CLASS_DOMAIN,.context=ctx,.ops=&domain_fid_ops};d->fid.ops=&domain_ops;
    rc=uet_domain(f->uet,fid,d->info,&d->fid,ctx,NULL,NULL,&d->uet);
    if(rc){fi_freeinfo(d->info);free(d);return rc;}*out=&d->fid;return 0;
}
static int fabric_close(fid_t fid)
{ struct fabric *f=(void *)fid;int rc=f->uet?uet_finalize(f->uet):0;if(!rc)free(f);return rc; }
static int eq_close(fid_t fid) { free(fid); return 0; }
static ssize_t eq_read(struct fid_eq *fid,uint32_t *event,void *buf,size_t len,uint64_t flags)
{ (void)fid;(void)event;(void)buf;(void)len;(void)flags;return -FI_EAGAIN; }
static struct fi_ops eq_fid_ops=FID_OPS(eq_close);
static struct fi_ops_eq eq_ops={.size=sizeof(struct fi_ops_eq),.read=eq_read};
static int eq_open(struct fid_fabric *f,struct fi_eq_attr *a,struct fid_eq **out,void *ctx)
{ (void)f;(void)a;struct fid_eq *q=calloc(1,sizeof(*q));if(!q)return -FI_ENOMEM;q->fid=(struct fid){.fclass=FI_CLASS_EQ,.context=ctx,.ops=&eq_fid_ops};q->ops=&eq_ops;*out=q;return 0; }
static struct fi_ops fabric_fid_ops=FID_OPS(fabric_close);
static struct fi_ops_fabric fabric_ops={.size=sizeof(struct fi_ops_fabric),.domain=domain_open,.eq_open=eq_open};
static int fabric_open(struct fi_fabric_attr *a,struct fid_fabric **out,void *ctx)
{
    (void)a;struct fabric *f=calloc(1,sizeof(*f));if(!f)return -FI_ENOMEM;
    f->fid.fid=(struct fid){.fclass=FI_CLASS_FABRIC,.context=ctx,.ops=&fabric_fid_ops};f->fid.ops=&fabric_ops;f->fid.api_version=FI_VERSION(1,20);*out=&f->fid;return 0;
}
static int getinfo_(uint32_t version,const char *node,const char *service,uint64_t flags,const struct fi_info *h,struct fi_info **out)
{
    (void)version;(void)node;(void)service;(void)flags;
    const uint64_t caps=FI_MSG|FI_SEND|FI_RECV|FI_LOCAL_COMM|FI_REMOTE_COMM;
    if(h && ((h->caps&~caps) || (h->ep_attr->type && h->ep_attr->type!=FI_EP_RDM)))return -FI_ENODATA;
    struct fi_info *i=fi_allocinfo();if(!i)return -FI_ENOMEM;
    i->caps=caps;i->mode=0;i->addr_format=FI_ADDR_UNSPEC;
    i->ep_attr->type=FI_EP_RDM;i->ep_attr->max_msg_size=1ULL<<30;i->ep_attr->tx_ctx_cnt=i->ep_attr->rx_ctx_cnt=1;
    i->tx_attr->caps=FI_MSG|FI_SEND;i->rx_attr->caps=FI_MSG|FI_RECV;
    i->tx_attr->size=i->rx_attr->size=1024;i->tx_attr->iov_limit=i->rx_attr->iov_limit=4;
    i->tx_attr->inject_size=0;
    const char *mode=getenv("UET_BENCH_MODE");uint64_t order=mode&&!strcmp(mode,"rud")?0:FI_ORDER_SAS;
    i->tx_attr->msg_order=i->rx_attr->msg_order=order;
    if(h)i->tx_attr->op_flags=h->tx_attr->op_flags;
    i->domain_attr->name=strdup("uet_ref_ud");i->domain_attr->threading=FI_THREAD_DOMAIN;
    i->domain_attr->control_progress=i->domain_attr->data_progress=FI_PROGRESS_MANUAL;
    i->domain_attr->resource_mgmt=FI_RM_ENABLED;i->domain_attr->av_type=FI_AV_TABLE;
    i->domain_attr->cq_cnt=2;i->domain_attr->ep_cnt=1;i->domain_attr->tx_ctx_cnt=i->domain_attr->rx_ctx_cnt=1;
    i->fabric_attr->name=strdup("uet_ref_ud");i->fabric_attr->prov_name=strdup(NAME);i->fabric_attr->prov_version=FI_VERSION(1,0);
    *out=i;return 0;
}
static struct fi_provider provider={.version=FI_VERSION(1,0),.fi_version=FI_VERSION(FI_MAJOR_VERSION,FI_MINOR_VERSION),.name=NAME,.getinfo=getinfo_,.fabric=fabric_open};
__attribute__((visibility("default"))) struct fi_provider *fi_prov_ini(void) { return &provider; }
