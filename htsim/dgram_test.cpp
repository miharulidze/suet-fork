/* SPDX-License-Identifier: BSD-2-Clause */
#include "dgram.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>

struct Peer {
    fi_info *info;
    fid_domain *domain;
    fid_ep *ep;
    fid_av *av;
    fid_cq *tx, *rx;
    sockaddr_in address;
    void open(unsigned id, fid_fabric *&fabric) {
        fi_info *hints = fi_allocinfo();
        hints->ep_attr->type = FI_EP_DGRAM;
        hints->fabric_attr->prov_name = strdup("htsim");
        assert(!fi_getinfo(FI_VERSION(1, 11), std::to_string(id).c_str(), "1", FI_SOURCE, hints,
                           &info));
        fi_freeinfo(hints);
        if (!fabric)
            assert(!fi_fabric(info->fabric_attr, &fabric, nullptr));
        assert(!fi_domain(fabric, info, &domain, nullptr));
        fi_av_attr aa{};
        aa.type = FI_AV_TABLE;
        assert(!fi_av_open(domain, &aa, &av, nullptr));
        fi_cq_attr ca{};
        ca.format = FI_CQ_FORMAT_MSG;
        ca.size = 2;
        ca.wait_obj = FI_WAIT_NONE;
        assert(!fi_cq_open(domain, &ca, &tx, nullptr));
        assert(!fi_cq_open(domain, &ca, &rx, nullptr));
        assert(!fi_endpoint(domain, info, &ep, nullptr));
        assert(!fi_ep_bind(ep, &av->fid, 0));
        assert(!fi_ep_bind(ep, &tx->fid, FI_TRANSMIT));
        assert(!fi_ep_bind(ep, &rx->fid, FI_RECV));
        assert(!fi_enable(ep));
        size_t len = sizeof(address);
        assert(!fi_getname(&ep->fid, &address, &len));
    }
    void close() {
        assert(fi_close(&domain->fid) == -FI_EBUSY);
        assert(fi_close(&av->fid) == -FI_EBUSY);
        assert(!fi_close(&ep->fid));
        assert(!fi_close(&av->fid));
        assert(!fi_close(&tx->fid));
        assert(!fi_close(&rx->fid));
        assert(!fi_close(&domain->fid));
        fi_freeinfo(info);
    }
};
int main() {
    assert(!htsim_dgram_configure(2, 2));
    fid_fabric *fabric = nullptr;
    Peer a{}, b{};
    a.open(0, fabric);
    b.open(1, fabric);
    fi_addr_t dest, source;
    assert(fi_av_insert(a.av, &b.address, 1, &dest, 0, nullptr) == 1);
    char data[] = "datagram", received[16] = {};
    fi_cq_msg_entry cq[2];
    assert(!fi_recv(b.ep, received, sizeof(received), nullptr, FI_ADDR_UNSPEC, received));
    assert(!fi_send(a.ep, data, sizeof(data), nullptr, dest, data));
    assert(fi_close(&a.ep->fid) == -FI_EAGAIN);
    assert(fi_cq_read(a.tx, cq, 2) == -FI_EAGAIN);
    assert(fi_cq_read(b.rx, cq, 2) == -FI_EAGAIN);
    htsim_dgram_frame frame;
    assert(htsim_dgram_take_tx(&frame) == 1);
    assert(fi_cq_read(a.tx, cq, 2) == 1 && cq[0].op_context == data);
    assert(fi_cq_read(b.rx, cq, 2) == -FI_EAGAIN); // local completion is not delivery
    data[0] = 'X';                                 // TX completion releases the application buffer
    assert(!htsim_dgram_deliver(&frame));
    assert(received[0] == 'd');
    data[0] = 'd';
    assert(fi_cq_read(b.rx, cq, 2) == -FI_EAVAIL);
    fi_cq_err_entry err{};
    assert(fi_cq_readerr(b.rx, &err, 0) == 1 && err.err == FI_EADDRNOTAVAIL);
    assert(!memcmp(received, data, sizeof(data)) && err.err_data_size == sizeof(sockaddr_in));
    assert(fi_av_insert(b.av, err.err_data, 1, &source, 0, nullptr) == 1);
    assert(!htsim_dgram_pending());

    assert(!fi_send(a.ep, data, sizeof(data), nullptr, dest, data));
    assert(!fi_send(a.ep, data, sizeof(data), nullptr, dest, data));
    assert(fi_send(a.ep, data, sizeof(data), nullptr, dest, data) == -FI_EAGAIN);
    assert(htsim_dgram_pending() == 2);
    assert(htsim_dgram_take_tx(&frame) == 1);
    assert(htsim_dgram_deliver(&frame) == -FI_EAGAIN); // no posted receive: real drop
    assert(htsim_dgram_take_tx(&frame) == 1);
    assert(!fi_recv(b.ep, received, 2, nullptr, FI_ADDR_UNSPEC, received));
    assert(!htsim_dgram_deliver(&frame));
    assert(fi_cq_readerr(b.rx, &err, 0) == 1 && err.err == FI_ETRUNC &&
           err.olen == sizeof(data) - 2);
    assert(fi_cq_read(a.tx, cq, 2) == 2);

    assert(!fi_recv(b.ep, received, sizeof(received), nullptr, FI_ADDR_UNSPEC, received));
    assert(!fi_send(a.ep, data, sizeof(data), nullptr, dest, data));
    assert(htsim_dgram_take_tx(&frame) == 1 && !htsim_dgram_deliver(&frame));
    fi_addr_t got_source;
    assert(fi_cq_readfrom(b.rx, cq, 1, &got_source) == 1 && got_source == source);
    assert(cq[0].len == sizeof(data) && !memcmp(received, data, sizeof(data)));
    assert(!fi_recv(b.ep, received, sizeof(received), nullptr, FI_ADDR_UNSPEC, received));
    assert(!fi_cancel(&b.ep->fid, received));
    assert(fi_cq_readerr(b.rx, &err, 0) == 1 && err.err == FI_ECANCELED);
    fid_mr *mr;
    assert(!fi_mr_reg(a.domain, data, sizeof(data), FI_SEND, 0, 0, 0, &mr, nullptr));
    assert(fi_mr_desc(mr));
    assert(!fi_close(&mr->fid));
    a.close();
    b.close();
    assert(!fi_close(&fabric->fid));
    std::cout << "Datagram completion timing, source discovery, overflow, cancellation and "
                 "lifetime: PASS\n";
}
