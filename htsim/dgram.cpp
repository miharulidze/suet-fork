/* SPDX-License-Identifier: BSD-2-Clause */
#include "dgram.h"
#include "suet_ext.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <new>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/providers/fi_prov.h>
#include <vector>

namespace {
struct Fabric {
    fid_fabric fid{};
    size_t refs = 0;
};
struct Domain {
    fid_domain fid{};
    Fabric *fabric;
    size_t refs = 0;
    uint64_t key = 1;
};
struct Address {
    sockaddr_in addr;
    bool valid = true;
};
struct Av {
    fid_av fid{};
    Domain *domain;
    size_t refs = 0;
    std::vector<Address> addresses;
};
struct Completion {
    fi_cq_msg_entry entry{};
    uint16_t ev = 0;
    fi_addr_t source = FI_ADDR_NOTAVAIL;
    int error = 0;
    size_t overflow = 0;
    sockaddr_in raw_source{};
};
struct Cq {
    fid_cq fid{};
    Domain *domain;
    size_t refs = 0, capacity;
    fi_cq_format format;
    std::deque<Completion> entries;
    sockaddr_in last_source{};
};
struct Receive {
    std::vector<iovec> iov;
    void *context;
    fi_addr_t source;
};
struct Transmit {
    htsim_dgram_frame frame;
    void *context;
    bool completion;
};
struct Endpoint {
    fid_ep fid{};
    Domain *domain;
    Av *av = nullptr;
    Cq *tx_cq = nullptr;
    Cq *rx_cq = nullptr;
    sockaddr_in address{};
    bool enabled = false;
    uint64_t tx_flags = 0;
    std::deque<Receive> receives;
    std::deque<Transmit> sends;
};
struct Mr {
    fid_mr fid{};
    Domain *domain;
};

uint32_t host_count = 0;
size_t queue_size = 1024, next_endpoint = 0;
std::vector<Endpoint *> endpoints;
fi_provider provider{};
fi_ops common_ops{}, fabric_ops{}, domain_ops{}, av_fid_ops{}, cq_fid_ops{}, ep_fid_ops{}, mr_ops{};
fi_ops_fabric fabric_api{};
fi_ops_domain domain_api{};
fi_ops_av av_api{};
fi_ops_cq cq_api{};
fi_ops_ep ep_api{};
fi_ops_cm cm_api{};
fi_ops_msg msg_api{};
fi_ops_mr mr_api{};
const uint64_t caps =
    FI_MSG | FI_SEND | FI_RECV | FI_SOURCE | FI_SOURCE_ERR | FI_LOCAL_COMM |
    FI_REMOTE_COMM | FI_SENDMSG_EV;

template <class R, class... Args> R unsupported(Args...) { return -FI_ENOSYS; }

bool equal(const sockaddr_in &a, const sockaddr_in &b) {
    return a.sin_family == b.sin_family && a.sin_addr.s_addr == b.sin_addr.s_addr &&
           a.sin_port == b.sin_port;
}
bool valid(const sockaddr_in &addr) {
    return addr.sin_family == AF_INET && ntohl(addr.sin_addr.s_addr) > 0 &&
           htsim_dgram_host(&addr) < host_count && addr.sin_port;
}
fi_addr_t lookup(Av *av, const sockaddr_in &addr) {
    for (size_t i = 0; i < av->addresses.size(); i++)
        if (av->addresses[i].valid && equal(av->addresses[i].addr, addr))
            return i;
    return FI_ADDR_NOTAVAIL;
}
int close_fabric(fid *f) {
    auto *obj = reinterpret_cast<Fabric *>(f);
    if (obj->refs)
        return -FI_EBUSY;
    delete obj;
    return 0;
}
int close_domain(fid *f) {
    auto *obj = reinterpret_cast<Domain *>(f);
    if (obj->refs)
        return -FI_EBUSY;
    obj->fabric->refs--;
    delete obj;
    return 0;
}
int close_av(fid *f) {
    auto *obj = reinterpret_cast<Av *>(f);
    if (obj->refs)
        return -FI_EBUSY;
    obj->domain->refs--;
    delete obj;
    return 0;
}
int close_cq(fid *f) {
    auto *obj = reinterpret_cast<Cq *>(f);
    if (obj->refs)
        return -FI_EBUSY;
    obj->domain->refs--;
    delete obj;
    return 0;
}
int close_ep(fid *f) {
    auto *ep = reinterpret_cast<Endpoint *>(f);
    /* Accepted sends must enter the simulated network before close succeeds. */
    if (!ep->sends.empty())
        return -FI_EAGAIN;
    endpoints.erase(std::remove(endpoints.begin(), endpoints.end(), ep), endpoints.end());
    if (ep->av)
        ep->av->refs--;
    if (ep->tx_cq)
        ep->tx_cq->refs--;
    if (ep->rx_cq)
        ep->rx_cq->refs--;
    ep->domain->refs--;
    delete ep; // queued sends/receives no longer retain caller buffers
    return 0;
}
int close_mr(fid *f) {
    auto *obj = reinterpret_cast<Mr *>(f);
    obj->domain->refs--;
    delete obj;
    return 0;
}
int open_av(fid_domain *f, fi_av_attr *attr, fid_av **out, void *context) {
    if (!attr || (attr->type != FI_AV_UNSPEC && attr->type != FI_AV_TABLE) || attr->name ||
        attr->flags)
        return -FI_EINVAL;
    auto *av = new (std::nothrow) Av;
    if (!av)
        return -FI_ENOMEM;
    av->domain = reinterpret_cast<Domain *>(f);
    av->domain->refs++;
    av->fid.fid = {FI_CLASS_AV, context, &av_fid_ops};
    av->fid.ops = &av_api;
    *out = &av->fid;
    return 0;
}
int insert_av(fid_av *f, const void *raw, size_t count, fi_addr_t *out, uint64_t flags,
              void *context) {
    if (flags & ~(FI_SYNC_ERR | FI_MORE))
        return -FI_EBADFLAGS;
    if ((count && !raw) || ((flags & FI_SYNC_ERR) && !context))
        return -FI_EINVAL;
    auto *av = reinterpret_cast<Av *>(f);
    auto *addr = static_cast<const sockaddr_in *>(raw);
    auto *errors = flags & FI_SYNC_ERR ? static_cast<int *>(context) : nullptr;
    int inserted = 0;
    for (size_t i = 0; i < count; i++) {
        if (errors)
            errors[i] = 0;
        if (!valid(addr[i])) {
            if (errors)
                errors[i] = FI_EINVAL;
            if (out)
                out[i] = FI_ADDR_NOTAVAIL;
            continue;
        }
        fi_addr_t index = lookup(av, addr[i]);
        if (index == FI_ADDR_NOTAVAIL) {
            try {
                av->addresses.push_back({addr[i], true});
            } catch (const std::bad_alloc &) {
                return inserted ? inserted : -FI_ENOMEM;
            }
            index = av->addresses.size() - 1;
        }
        if (out)
            out[i] = index;
        inserted++;
    }
    return inserted;
}
int remove_av(fid_av *f, fi_addr_t *indices, size_t count, uint64_t flags) {
    auto *av = reinterpret_cast<Av *>(f);
    if (flags)
        return -FI_EBADFLAGS;
    for (size_t i = 0; i < count; i++) {
        if (indices[i] >= av->addresses.size() || !av->addresses[indices[i]].valid)
            return -FI_EINVAL;
        av->addresses[indices[i]].valid = false;
    }
    return 0;
}
int lookup_av(fid_av *f, fi_addr_t index, void *addr, size_t *len) {
    auto *av = reinterpret_cast<Av *>(f);
    if (!len || index >= av->addresses.size() || !av->addresses[index].valid)
        return -FI_EINVAL;
    size_t available = *len;
    *len = sizeof(sockaddr_in);
    if (available < *len)
        return -FI_ETOOSMALL;
    if (!addr)
        return -FI_EINVAL;
    memcpy(addr, &av->addresses[index].addr, *len);
    return 0;
}
const char *straddr_av(fid_av *, const void *raw, char *buf, size_t *len) {
    if (!raw || !buf || !len)
        return nullptr;
    const auto *addr = static_cast<const sockaddr_in *>(raw);
    int n = snprintf(buf, *len, "htsim:%u:%u", htsim_dgram_host(addr), ntohs(addr->sin_port));
    *len = static_cast<size_t>(n + 1);
    return buf;
}
int open_cq(fid_domain *f, fi_cq_attr *attr, fid_cq **out, void *context) {
    if (!attr || (attr->format != FI_CQ_FORMAT_MSG && attr->format != FI_CQ_FORMAT_DATA &&
                  attr->format != FI_CQ_FORMAT_UNSPEC) ||
        attr->wait_obj != FI_WAIT_NONE || attr->flags)
        return -FI_EINVAL;
    auto *cq = new (std::nothrow) Cq;
    if (!cq)
        return -FI_ENOMEM;
    cq->domain = reinterpret_cast<Domain *>(f);
    cq->domain->refs++;
    cq->capacity = attr->size ? attr->size : queue_size;
    cq->format = attr->format;
    cq->fid.fid = {FI_CLASS_CQ, context, &cq_fid_ops};
    cq->fid.ops = &cq_api;
    *out = &cq->fid;
    return 0;
}
ssize_t readfrom_cq(fid_cq *f, void *buf, size_t count, fi_addr_t *sources) {
    auto *cq = reinterpret_cast<Cq *>(f);
    auto *entries = static_cast<fi_cq_msg_entry *>(buf);
    if (!count)
        return 0;
    if (!buf)
        return -FI_EINVAL;
    size_t n = 0;
    while (n < count && !cq->entries.empty()) {
        auto &c = cq->entries.front();
        if (c.error)
            return n ? static_cast<ssize_t>(n) : -FI_EAVAIL;
        if (cq->format == FI_CQ_FORMAT_DATA) {
            auto *data = static_cast<fi_cq_data_entry *>(buf);
            data[n] = {c.entry.op_context, c.entry.flags, c.entry.len, nullptr, c.ev};
        } else {
            entries[n] = c.entry;
        }
        if (sources)
            sources[n] = c.source;
        cq->entries.pop_front();
        n++;
    }
    return n ? static_cast<ssize_t>(n) : -FI_EAGAIN;
}
ssize_t read_cq(fid_cq *f, void *buf, size_t count) { return readfrom_cq(f, buf, count, nullptr); }
ssize_t readerr_cq(fid_cq *f, fi_cq_err_entry *out, uint64_t flags) {
    auto *cq = reinterpret_cast<Cq *>(f);
    if (flags)
        return -FI_EBADFLAGS;
    if (!out)
        return -FI_EINVAL;
    if (cq->entries.empty() || !cq->entries.front().error)
        return -FI_EAGAIN;
    auto c = cq->entries.front();
    cq->entries.pop_front();
    *out = {};
    out->op_context = c.entry.op_context;
    out->flags = c.entry.flags;
    out->len = c.entry.len;
    out->data = c.ev;
    out->olen = c.overflow;
    out->err = c.error;
    out->prov_errno = c.error;
    if (c.error == FI_EADDRNOTAVAIL) {
        cq->last_source = c.raw_source;
        out->err_data = &cq->last_source;
        out->err_data_size = sizeof(cq->last_source);
    }
    return 1;
}
int bind_ep(fid *f, fid *target, uint64_t flags) {
    auto *ep = reinterpret_cast<Endpoint *>(f);
    if (ep->enabled)
        return -FI_EOPBADSTATE;
    if (target->fclass == FI_CLASS_AV) {
        auto *av = reinterpret_cast<Av *>(target);
        if (flags || ep->av || av->domain != ep->domain)
            return -FI_EINVAL;
        ep->av = av;
        av->refs++;
        return 0;
    }
    if (target->fclass != FI_CLASS_CQ || !(flags & (FI_TRANSMIT | FI_RECV)) ||
        (flags & ~(FI_TRANSMIT | FI_RECV)))
        return -FI_EINVAL;
    auto *cq = reinterpret_cast<Cq *>(target);
    if (cq->domain != ep->domain || ((flags & FI_TRANSMIT) && ep->tx_cq) ||
        ((flags & FI_RECV) && ep->rx_cq))
        return -FI_EINVAL;
    if (flags & FI_TRANSMIT) {
        ep->tx_cq = cq;
        cq->refs++;
    }
    if (flags & FI_RECV) {
        ep->rx_cq = cq;
        cq->refs++;
    }
    return 0;
}
int control_ep(fid *f, int command, void *) {
    auto *ep = reinterpret_cast<Endpoint *>(f);
    if (command != FI_ENABLE)
        return -FI_ENOSYS;
    if (!ep->av || !ep->tx_cq || !ep->rx_cq)
        return -FI_ENOCQ;
    for (auto *other : endpoints)
        if (other != ep && other->enabled && equal(other->address, ep->address))
            return -FI_EADDRINUSE;
    ep->enabled = true;
    return 0;
}
int getname_ep(fid *f, void *addr, size_t *len) {
    if (!len)
        return -FI_EINVAL;
    size_t available = *len;
    *len = sizeof(sockaddr_in);
    if (available < *len)
        return -FI_ETOOSMALL;
    if (!addr)
        return -FI_EINVAL;
    memcpy(addr, &reinterpret_cast<Endpoint *>(f)->address, *len);
    return 0;
}
int setname_ep(fid *f, void *addr, size_t len) {
    auto *ep = reinterpret_cast<Endpoint *>(f);
    if (ep->enabled)
        return -FI_EOPBADSTATE;
    if (!addr || len != sizeof(sockaddr_in) || !valid(*static_cast<sockaddr_in *>(addr)))
        return -FI_EINVAL;
    ep->address = *static_cast<sockaddr_in *>(addr);
    return 0;
}
ssize_t recvv_ep(fid_ep *f, const iovec *iov, void **, size_t count, fi_addr_t source,
                 void *context) {
    auto *ep = reinterpret_cast<Endpoint *>(f);
    if (!ep->enabled)
        return -FI_EOPBADSTATE;
    if (!iov || !count || count > 8)
        return -FI_EINVAL;
    for (size_t i = 0; i < count; i++)
        if (iov[i].iov_len && !iov[i].iov_base)
            return -FI_EINVAL;
    if (ep->receives.size() >= queue_size)
        return -FI_EAGAIN;
    try {
        ep->receives.push_back({std::vector<iovec>(iov, iov + count), context, source});
    } catch (const std::bad_alloc &) {
        return -FI_ENOMEM;
    }
    return 0;
}
ssize_t recv_ep(fid_ep *f, void *buf, size_t len, void *desc, fi_addr_t src, void *context) {
    iovec iov{buf, len};
    return recvv_ep(f, &iov, &desc, 1, src, context);
}
ssize_t sendmsg_common(fid_ep *f, const fi_msg *msg, uint64_t flags, uint16_t ev, bool has_ev) {
    auto *ep = reinterpret_cast<Endpoint *>(f);
    if (!ep->enabled)
        return -FI_EOPBADSTATE;
    if (flags & ~(FI_COMPLETION | FI_INJECT | FI_MORE | FI_TRANSMIT_COMPLETE))
        return -FI_EBADFLAGS;
    if (!msg || !msg->msg_iov || !msg->iov_count || msg->iov_count > 8 ||
        msg->addr >= ep->av->addresses.size() || !ep->av->addresses[msg->addr].valid)
        return -FI_EINVAL;
    if (ep->sends.size() >= queue_size || ep->tx_cq->entries.size() >= ep->tx_cq->capacity)
        return -FI_EAGAIN;
    Transmit tx{};
    tx.context = msg->context;
    tx.completion = !(flags & FI_INJECT) || (flags & FI_COMPLETION);
    tx.frame.ev = ev;
    tx.frame.rx_metadata = has_ev ? FI_SUET_DGRAM_EV : 0;
    tx.frame.src = ep->address;
    tx.frame.dst = ep->av->addresses[msg->addr].addr;
    for (size_t i = 0; i < msg->iov_count; i++) {
        size_t len = msg->msg_iov[i].iov_len;
        if (len > HTSIM_DGRAM_MTU - tx.frame.size)
            return -FI_EMSGSIZE;
        if (len && !msg->msg_iov[i].iov_base)
            return -FI_EINVAL;
        if (len)
            memcpy(tx.frame.data + tx.frame.size, msg->msg_iov[i].iov_base, len);
        tx.frame.size += len;
    }
    try {
        ep->sends.push_back(tx);
    } catch (const std::bad_alloc &) {
        return -FI_ENOMEM;
    }
    return 0;
}
ssize_t sendmsg_ep(fid_ep *f, const fi_msg *msg, uint64_t flags) {
    return sendmsg_common(f, msg, flags, 0, false);
}
ssize_t sendmsg_ev_ep(fid_ep *f, const fi_msg_ev *msg, uint64_t flags) {
    if (!msg)
        return -FI_EINVAL;
    return sendmsg_common(f, &msg->msg, flags, msg->ev, true);
}
ssize_t sendv_ep(fid_ep *f, const iovec *iov, void **desc, size_t count, fi_addr_t dst,
                 void *context) {
    fi_msg msg{};
    msg.msg_iov = iov;
    msg.desc = desc;
    msg.iov_count = count;
    msg.addr = dst;
    msg.context = context;
    return sendmsg_ep(f, &msg, reinterpret_cast<Endpoint *>(f)->tx_flags);
}
ssize_t send_ep(fid_ep *f, const void *buf, size_t len, void *desc, fi_addr_t dst, void *context) {
    iovec iov{const_cast<void *>(buf), len};
    return sendv_ep(f, &iov, &desc, 1, dst, context);
}
int open_ep(fid_domain *f, fi_info *info, fid_ep **out, void *context) {
    if (!info || !info->src_addr || info->src_addrlen != sizeof(sockaddr_in) ||
        !valid(*static_cast<sockaddr_in *>(info->src_addr)))
        return -FI_EINVAL;
    auto *ep = new (std::nothrow) Endpoint;
    if (!ep)
        return -FI_ENOMEM;
    ep->domain = reinterpret_cast<Domain *>(f);
    ep->address = *static_cast<sockaddr_in *>(info->src_addr);
    ep->tx_flags = info->tx_attr->op_flags;
    ep->fid.fid = {FI_CLASS_EP, context, &ep_fid_ops};
    ep->fid.ops = &ep_api;
    ep->fid.cm = &cm_api;
    ep->fid.msg = &msg_api;
    try {
        endpoints.push_back(ep);
    } catch (const std::bad_alloc &) {
        delete ep;
        return -FI_ENOMEM;
    }
    ep->domain->refs++;
    *out = &ep->fid;
    return 0;
}
int regattr_mr(fid *f, const fi_mr_attr *attr, uint64_t flags, fid_mr **out) {
    if (!attr || !attr->mr_iov || !attr->iov_count || flags)
        return -FI_EINVAL;
    auto *mr = new (std::nothrow) Mr;
    if (!mr)
        return -FI_ENOMEM;
    mr->domain = reinterpret_cast<Domain *>(f);
    mr->domain->refs++;
    mr->fid.fid = {FI_CLASS_MR, attr->context, &mr_ops};
    mr->fid.mem_desc = mr;
    mr->fid.key = mr->domain->key++;
    *out = &mr->fid;
    return 0;
}
int regv_mr(fid *f, const iovec *iov, size_t count, uint64_t access, uint64_t offset, uint64_t key,
            uint64_t flags, fid_mr **out, void *context) {
    fi_mr_attr attr{};
    attr.mr_iov = iov;
    attr.iov_count = count;
    attr.access = access;
    attr.offset = offset;
    attr.requested_key = key;
    attr.context = context;
    return regattr_mr(f, &attr, flags, out);
}
int reg_mr(fid *f, const void *buf, size_t len, uint64_t access, uint64_t offset, uint64_t key,
           uint64_t flags, fid_mr **out, void *context) {
    iovec iov{const_cast<void *>(buf), len};
    return regv_mr(f, &iov, 1, access, offset, key, flags, out, context);
}
int open_domain(fid_fabric *f, fi_info *, fid_domain **out, void *context) {
    auto *domain = new (std::nothrow) Domain;
    if (!domain)
        return -FI_ENOMEM;
    domain->fabric = reinterpret_cast<Fabric *>(f);
    domain->fabric->refs++;
    domain->fid.fid = {FI_CLASS_DOMAIN, context, &domain_ops};
    domain->fid.ops = &domain_api;
    domain->fid.mr = &mr_api;
    *out = &domain->fid;
    return 0;
}
int open_fabric(fi_fabric_attr *, fid_fabric **out, void *context) {
    auto *fabric = new (std::nothrow) Fabric;
    if (!fabric)
        return -FI_ENOMEM;
    fabric->fid.fid = {FI_CLASS_FABRIC, context, &fabric_ops};
    fabric->fid.ops = &fabric_api;
    *out = &fabric->fid;
    return 0;
}
int getinfo(uint32_t, const char *node, const char *service, uint64_t flags, const fi_info *hints,
            fi_info **out) {
    *out = nullptr;
    if (!host_count)
        return -FI_ENODATA;
    if (hints && ((hints->caps & ~caps) ||
                  (hints->ep_attr && hints->ep_attr->type != FI_EP_UNSPEC &&
                   hints->ep_attr->type != FI_EP_DGRAM) ||
                  (hints->addr_format != FI_FORMAT_UNSPEC && hints->addr_format != FI_SOCKADDR_IN &&
                   hints->addr_format != FI_SOCKADDR)))
        return -FI_ENODATA;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1);
    addr.sin_addr.s_addr = htonl(1);
    if (hints && hints->src_addr) {
        if (hints->src_addrlen != sizeof(addr))
            return -FI_ENODATA;
        memcpy(&addr, hints->src_addr, sizeof(addr));
    }
    if (node) {
        char *end;
        unsigned long host = strtoul(node, &end, 10);
        if (*end || host >= host_count)
            return -FI_ENODATA;
        addr.sin_addr.s_addr = htonl(static_cast<uint32_t>(host) + 1);
    }
    if (service) {
        char *end;
        unsigned long port = strtoul(service, &end, 10);
        if (*end || !port || port > 65535)
            return -FI_ENODATA;
        addr.sin_port = htons(static_cast<uint16_t>(port));
    }
    if (!valid(addr))
        return -FI_ENODATA;
    fi_info *info = fi_allocinfo();
    if (!info)
        return -FI_ENOMEM;
    info->caps = caps;
    info->addr_format = FI_SOCKADDR_IN;
    info->src_addr = malloc(sizeof(addr));
    info->src_addrlen = sizeof(addr);
    info->domain_attr->name = strdup("htsim");
    info->fabric_attr->name = strdup("htsim");
    if (!info->src_addr || !info->domain_attr->name || !info->fabric_attr->name) {
        fi_freeinfo(info);
        return -FI_ENOMEM;
    }
    memcpy(info->src_addr, &addr, sizeof(addr));
    if (hints && hints->dest_addr) {
        if (hints->dest_addrlen != sizeof(addr)) {
            fi_freeinfo(info);
            return -FI_ENODATA;
        }
        info->dest_addr = malloc(sizeof(addr));
        if (!info->dest_addr) {
            fi_freeinfo(info);
            return -FI_ENOMEM;
        }
        memcpy(info->dest_addr, hints->dest_addr, sizeof(addr));
        info->dest_addrlen = sizeof(addr);
    } else if (node && !(flags & FI_SOURCE)) {
        info->dest_addr = malloc(sizeof(addr));
        if (!info->dest_addr) {
            fi_freeinfo(info);
            return -FI_ENOMEM;
        }
        memcpy(info->dest_addr, &addr, sizeof(addr));
        info->dest_addrlen = sizeof(addr);
    }
    info->tx_attr->caps = FI_MSG | FI_SEND | FI_SENDMSG_EV;
    info->rx_attr->caps = FI_MSG | FI_RECV | FI_SOURCE | FI_SOURCE_ERR;
    info->tx_attr->size = info->rx_attr->size = queue_size;
    info->tx_attr->iov_limit = info->rx_attr->iov_limit = 8;
    info->tx_attr->inject_size = HTSIM_DGRAM_MTU;
    info->ep_attr->type = FI_EP_DGRAM;
    info->ep_attr->max_msg_size = HTSIM_DGRAM_MTU;
    info->ep_attr->tx_ctx_cnt = info->ep_attr->rx_ctx_cnt = 1;
    info->domain_attr->caps = FI_LOCAL_COMM | FI_REMOTE_COMM;
    info->domain_attr->threading = FI_THREAD_DOMAIN;
    info->domain_attr->control_progress = FI_PROGRESS_MANUAL;
    info->domain_attr->data_progress = FI_PROGRESS_MANUAL;
    info->domain_attr->resource_mgmt = FI_RM_ENABLED;
    info->domain_attr->av_type = FI_AV_TABLE;
    info->domain_attr->mr_key_size = sizeof(uint64_t);
    info->domain_attr->mr_iov_limit = 8;
    info->domain_attr->cq_cnt = 65536;
    info->domain_attr->ep_cnt = info->domain_attr->tx_ctx_cnt = info->domain_attr->rx_ctx_cnt =
        65536;
    info->domain_attr->max_ep_tx_ctx = info->domain_attr->max_ep_rx_ctx = 1;
    *out = info;
    return 0;
}
} // namespace

extern "C" uint32_t htsim_dgram_host(const sockaddr_in *addr) {
    return ntohl(addr->sin_addr.s_addr) - 1;
}
extern "C" int htsim_dgram_configure(uint32_t hosts, size_t capacity) {
    if (!endpoints.empty())
        return -FI_EBUSY;
    if (!hosts || hosts == UINT32_MAX || !capacity)
        return -FI_EINVAL;
    host_count = hosts;
    queue_size = capacity;
    next_endpoint = 0;
    return 0;
}
extern "C" size_t htsim_dgram_pending() {
    size_t n = 0;
    for (auto *ep : endpoints)
        n += ep->sends.size();
    return n;
}
extern "C" int htsim_dgram_take_tx(htsim_dgram_frame *frame) {
    if (!frame)
        return -FI_EINVAL;
    for (size_t i = 0; i < endpoints.size(); i++) {
        next_endpoint %= endpoints.size();
        auto *ep = endpoints[next_endpoint++];
        if (ep->sends.empty())
            continue;
        auto &tx = ep->sends.front();
        if (tx.completion && ep->tx_cq->entries.size() >= ep->tx_cq->capacity)
            continue;
        if (tx.completion) {
            Completion c;
            c.entry = {tx.context, FI_SEND | FI_MSG, tx.frame.size};
            ep->tx_cq->entries.push_back(c);
        }
        *frame = tx.frame;
        ep->sends.pop_front();
        return 1;
    }
    return 0;
}
extern "C" int htsim_dgram_deliver(const htsim_dgram_frame *frame) {
    if (!frame || frame->size > HTSIM_DGRAM_MTU)
        return -FI_EINVAL;
    for (auto *ep : endpoints) {
        if (!ep->enabled || !equal(ep->address, frame->dst))
            continue;
        if (ep->rx_cq->entries.size() >= ep->rx_cq->capacity)
            return -FI_EAGAIN;
        fi_addr_t source = lookup(ep->av, frame->src);
        auto rx =
            std::find_if(ep->receives.begin(), ep->receives.end(), [source](const Receive &r) {
                return r.source == FI_ADDR_UNSPEC || r.source == source;
            });
        if (rx == ep->receives.end())
            return -FI_EAGAIN;
        size_t copied = 0;
        for (auto &iov : rx->iov) {
            size_t n = std::min(iov.iov_len, frame->size - copied);
            if (n)
                memcpy(iov.iov_base, frame->data + copied, n);
            copied += n;
        }
        Completion c;
        c.entry = {rx->context, FI_RECV | FI_MSG |
                   (frame->rx_metadata & FI_SUET_DGRAM_METADATA_MASK), copied};
        if (ep->rx_cq->format == FI_CQ_FORMAT_DATA && (c.entry.flags & FI_SUET_DGRAM_EV))
            c.ev = frame->ev;
        else
            c.entry.flags &= ~FI_SUET_DGRAM_EV;
        c.source = source;
        if (copied != frame->size) {
            c.error = FI_ETRUNC;
            c.overflow = frame->size - copied;
        } else if (source == FI_ADDR_NOTAVAIL) {
            c.error = FI_EADDRNOTAVAIL;
            c.raw_source = frame->src;
        }
        ep->rx_cq->entries.push_back(c);
        ep->receives.erase(rx);
        return 0;
    }
    return -FI_EADDRNOTAVAIL;
}

extern "C" __attribute__((visibility("default"))) fi_provider *fi_prov_ini() {
    static bool initialized = false;
    if (initialized)
        return &provider;
    initialized = true;
    common_ops.size = sizeof(fi_ops);
    common_ops.tostr = unsupported;
    common_ops.ops_set = unsupported;
    common_ops.bind = [](fid *, fid *, uint64_t) { return -FI_ENOSYS; };
    common_ops.control = [](fid *, int, void *) { return -FI_ENOSYS; };
    common_ops.ops_open = [](fid *, const char *, uint64_t, void **, void *) { return -FI_ENOSYS; };
    fabric_ops = domain_ops = av_fid_ops = cq_fid_ops = ep_fid_ops = mr_ops = common_ops;
    fabric_ops.close = close_fabric;
    domain_ops.close = close_domain;
    domain_ops.control = [](fid *, int command, void *arg) {
        auto *var = static_cast<fi_fid_var *>(arg);
        if (command != FI_GET_VAL || !var || var->name != FI_SUET_DGRAM_RX_METADATA)
            return -FI_ENOSYS;
        if (!var->val)
            return -FI_EINVAL;
        *static_cast<uint64_t *>(var->val) = FI_SUET_DGRAM_METADATA_MASK;
        return 0;
    };
    av_fid_ops.close = close_av;
    cq_fid_ops.close = close_cq;
    ep_fid_ops.close = close_ep;
    ep_fid_ops.bind = bind_ep;
    ep_fid_ops.control = control_ep;
    mr_ops.close = close_mr;
    fabric_api.size = sizeof(fabric_api);
    fabric_api.domain = open_domain;
    fabric_api.passive_ep = unsupported;
    fabric_api.eq_open = unsupported;
    fabric_api.wait_open = unsupported;
    fabric_api.trywait = [](fid_fabric *, fid **, int) { return -FI_EAGAIN; };
    domain_api.size = sizeof(domain_api);
    domain_api.av_open = open_av;
    domain_api.cq_open = open_cq;
    domain_api.endpoint = open_ep;
    domain_api.scalable_ep = unsupported;
    domain_api.cntr_open = unsupported;
    domain_api.poll_open = unsupported;
    domain_api.stx_ctx = unsupported;
    domain_api.srx_ctx = unsupported;
    domain_api.query_atomic = unsupported;
    domain_api.query_collective = unsupported;
    av_api.size = sizeof(av_api);
    av_api.insert = insert_av;
    av_api.remove = remove_av;
    av_api.lookup = lookup_av;
    av_api.straddr = straddr_av;
    av_api.insertsvc = unsupported;
    av_api.insertsym = unsupported;
    av_api.av_set = unsupported;
    cq_api.size = sizeof(cq_api);
    cq_api.read = read_cq;
    cq_api.readfrom = readfrom_cq;
    cq_api.readerr = readerr_cq;
    cq_api.sread = unsupported;
    cq_api.sreadfrom = unsupported;
    cq_api.strerror = [](fid_cq *, int error, const void *, char *buf, size_t len) -> const char * {
        const char *str = fi_strerror(error);
        if (buf && len) {
            snprintf(buf, len, "%s", str);
            return buf;
        }
        return str;
    };
    cq_api.signal = [](fid_cq *) { return -FI_ENOSYS; };
    ep_api.size = sizeof(ep_api);
    ep_api.tx_ctx = unsupported;
    ep_api.rx_ctx = unsupported;
    ep_api.rx_size_left = [](fid_ep *f) -> ssize_t {
        return queue_size - reinterpret_cast<Endpoint *>(f)->receives.size();
    };
    ep_api.tx_size_left = [](fid_ep *f) -> ssize_t {
        return queue_size - reinterpret_cast<Endpoint *>(f)->sends.size();
    };
    ep_api.cancel = [](fid *f, void *context) -> ssize_t {
        auto *ep = reinterpret_cast<Endpoint *>(f);
        auto rx = std::find_if(ep->receives.begin(), ep->receives.end(),
                               [context](const Receive &r) { return r.context == context; });
        if (rx == ep->receives.end())
            return -FI_ENOENT;
        if (ep->rx_cq->entries.size() >= ep->rx_cq->capacity)
            return -FI_EAGAIN;
        Completion c;
        c.error = FI_ECANCELED;
        c.entry.op_context = context;
        c.entry.flags = FI_RECV;
        ep->rx_cq->entries.push_back(c);
        ep->receives.erase(rx);
        return 0;
    };
    ep_api.getopt = [](fid *, int, int, void *, size_t *) { return -FI_ENOPROTOOPT; };
    ep_api.setopt = [](fid *, int, int, const void *, size_t) { return -FI_ENOPROTOOPT; };
    cm_api.size = sizeof(cm_api);
    cm_api.getname = getname_ep;
    cm_api.setname = setname_ep;
    cm_api.getpeer = unsupported;
    cm_api.connect = unsupported;
    cm_api.listen = unsupported;
    cm_api.accept = unsupported;
    cm_api.reject = unsupported;
    cm_api.shutdown = unsupported;
    cm_api.join = unsupported;
    msg_api.size = sizeof(msg_api);
    msg_api.recv = recv_ep;
    msg_api.recvv = recvv_ep;
    msg_api.senddata = unsupported;
    msg_api.injectdata = unsupported;
    msg_api.recvmsg = [](fid_ep *f, const fi_msg *m, uint64_t flags) -> ssize_t {
        if (flags)
            return -FI_EBADFLAGS;
        if (!m)
            return -FI_EINVAL;
        return recvv_ep(f, m->msg_iov, m->desc, m->iov_count, m->addr, m->context);
    };
    msg_api.send = send_ep;
    msg_api.sendv = sendv_ep;
    msg_api.sendmsg = sendmsg_ep;
    msg_api.sendmsg_ev = sendmsg_ev_ep;
    msg_api.inject = [](fid_ep *f, const void *buf, size_t len, fi_addr_t addr) -> ssize_t {
        iovec iov{const_cast<void *>(buf), len};
        fi_msg m{};
        m.msg_iov = &iov;
        m.iov_count = 1;
        m.addr = addr;
        return sendmsg_ep(f, &m, FI_INJECT);
    };
    mr_api.size = sizeof(mr_api);
    mr_api.reg = reg_mr;
    mr_api.regv = regv_mr;
    mr_api.regattr = regattr_mr;
    provider.version = FI_VERSION(1, 0);
    provider.fi_version = FI_VERSION(1, 11);
    provider.name = "htsim";
    provider.getinfo = getinfo;
    provider.fabric = open_fabric;
    provider.cleanup = []() {};
    return &provider;
}
