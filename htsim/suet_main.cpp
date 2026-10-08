/* SPDX-License-Identifier: BSD-2-Clause */
#include "connection_matrix.h"
#include "dgram.h"
#include "fat_tree_switch.h"
#include "fat_tree_topology.h"
#include "suet_ext.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_tagged.h>
#include <stdexcept>
#include <vector>

static void check(int ret, const char *operation) {
    if (ret < 0)
        throw std::runtime_error(std::string(operation) + ": " + fi_strerror(-ret));
}
static bool ordered = true;
static uint32_t reorder_every, drop_per_mille, burst_every, burst_length = 2, trim_every;
static bool trim_queues, trim_lasthop;
static uint64_t elapsed_ms(void *) { return EventList::now() / 1000000000ULL; }

struct Host {
    fi_info *info = nullptr;
    fid_domain *domain = nullptr;
    fid_ep *ep = nullptr;
    fid_av *av = nullptr;
    fid_cq *cq = nullptr;
    std::vector<unsigned char> address;
    std::map<uint32_t, fi_addr_t> peers;

    void open(uint32_t id, fid_fabric *&fabric) {
        fi_info *hints = fi_allocinfo();
        if (!hints)
            throw std::bad_alloc();
        hints->caps = FI_MSG | FI_TAGGED;
        hints->tx_attr->msg_order = ordered ? FI_ORDER_SAS : 0;
        hints->ep_attr->type = FI_EP_RDM;
        hints->domain_attr->threading = FI_THREAD_DOMAIN;
        hints->fabric_attr->prov_name = strdup("htsim;ofi_suet");
        int ret =
            fi_getinfo(FI_VERSION(1, 11), std::to_string(id).c_str(), "1", FI_SOURCE, hints, &info);
        fi_freeinfo(hints);
        check(ret, "fi_getinfo(htsim;ofi_suet)");
        info->tx_attr->msg_order = ordered ? FI_ORDER_SAS : 0;
        if (!fabric)
            check(fi_fabric(info->fabric_attr, &fabric, nullptr), "fi_fabric");
        check(fi_domain(fabric, info, &domain, nullptr), "fi_domain");
        fi_suet_clock clock{sizeof(clock), FI_SUET_CLOCK_ASYNC_CLOSE, elapsed_ms, nullptr};
        check(fi_set_val(&domain->fid, FI_SUET_CLOCK, &clock), "SUET simulated clock");
        fi_av_attr av_attr{};
        av_attr.type = FI_AV_TABLE;
        check(fi_av_open(domain, &av_attr, &av, nullptr), "fi_av_open");
        fi_cq_attr cq_attr{};
        cq_attr.format = FI_CQ_FORMAT_TAGGED;
        cq_attr.wait_obj = FI_WAIT_NONE;
        cq_attr.size = 4096;
        check(fi_cq_open(domain, &cq_attr, &cq, nullptr), "fi_cq_open");
        check(fi_endpoint(domain, info, &ep, nullptr), "fi_endpoint");
        check(fi_ep_bind(ep, &av->fid, 0), "bind AV");
        check(fi_ep_bind(ep, &cq->fid, FI_TRANSMIT | FI_RECV), "bind CQ");
        check(fi_enable(ep), "fi_enable");
        size_t size = 0;
        ret = fi_getname(&ep->fid, nullptr, &size);
        if (ret != -FI_ETOOSMALL)
            check(ret, "fi_getname size");
        address.resize(size);
        check(fi_getname(&ep->fid, address.data(), &size), "fi_getname");
    }
    void add_peer(uint32_t id, const Host &peer) {
        if (peers.count(id))
            return;
        fi_addr_t addr;
        int ret = fi_av_insert(av, peer.address.data(), 1, &addr, 0, nullptr);
        if (ret != 1)
            throw std::runtime_error("SUET AV insertion failed");
        peers[id] = addr;
    }
    bool close() {
        if (ep) {
            check(fi_close(&ep->fid), "close EP");
            ep = nullptr;
        }
        if (cq) {
            check(fi_close(&cq->fid), "close CQ");
            cq = nullptr;
        }
        if (av) {
            check(fi_close(&av->fid), "close AV");
            av = nullptr;
        }
        if (domain) {
            int ret = fi_close(&domain->fid);
            if (ret == -FI_EAGAIN)
                return false;
            check(ret, "close domain");
            domain = nullptr;
        }
        if (info) {
            fi_freeinfo(info);
            info = nullptr;
        }
        return true;
    }
};

struct NetworkStats {
    std::set<uint16_t> data_evs;
    uint64_t sacks = 0, retransmits = 0, ecn_marks = 0, ack_marks = 0, trims = 0, trim_nacks = 0;
    uint64_t sent = 0, received = 0, dropped = 0, receive_drops = 0, in_flight = 0;
};
class FabricPacket : public Packet {
  public:
    FabricPacket(const htsim_dgram_frame &data, PacketFlow &flow, const Route &route,
                 NetworkStats &stats, size_t overhead)
        : frame(data), _stats(stats), _overhead(overhead) {
        set_route(flow, route, data.size + overhead, stats.sent++);
        _type = IP;
        unsigned type = frame.data[0] >> 3;
        _is_header = type != 2 && type != 3;
        set_dst(htsim_dgram_host(&data.dst));
        // Same entropy-to-ECMP input as UecSrc::send_packet: Packet::pathid.
        set_pathid((data.rx_metadata & FI_SUET_DGRAM_EV) ? data.ev :
                   htsim_dgram_host(&data.src) * 2654435761U + dst());
        if (!_is_header && (data.rx_metadata & FI_SUET_DGRAM_EV))
            stats.data_evs.insert(data.ev);
        stats.in_flight++;
    }
    PktPriority priority() const override { return header_only() ? PRIO_HI : PRIO_LO; }
    void strip_payload(uint16_t trim_size) override {
        Packet::strip_payload(trim_size);
        frame.size = std::min(frame.size, trim_size > _overhead ? trim_size - _overhead : 0);
        frame.rx_metadata |= FI_SUET_DGRAM_TRIMMED;
        _stats.trims++;
    }
    void free() override {
        _stats.in_flight--;
        if (!delivered)
            _stats.dropped++;
        delete this;
    }
    htsim_dgram_frame frame;
    bool delivered = false;

  private:
    NetworkStats &_stats;
    size_t _overhead;
};
class FabricPort : public PacketSink {
  public:
    FabricPort(uint32_t id, NetworkStats &stats)
        : _name("fabric_host_" + std::to_string(id)), _stats(stats) {}
    void receivePacket(Packet &raw) override {
        auto &packet = static_cast<FabricPacket &>(raw);
        packet.delivered = true;
        if (packet.flags() & ECN_CE) {
            packet.frame.rx_metadata |= FI_SUET_DGRAM_ECN;
            _stats.ecn_marks++;
        }
        if (htsim_dgram_deliver(&packet.frame))
            _stats.receive_drops++;
        else
            _stats.received++;
        packet.free();
    }
    const std::string &nodename() override { return _name; }

  private:
    std::string _name;
    NetworkStats &_stats;
};

class Transfer : public EventSource, public TriggerTarget {
  public:
    Transfer(EventList &events, connection &c, uint64_t tag)
        : EventSource(events, "suet_transfer"), conn(c), tag(tag) {}
    void doNextEvent() override { activate(); }
    void activate() override {
        if (active)
            throw std::runtime_error("matrix activated a message twice");
        active = true;
        started = EventList::now();
        tx.resize(std::max(conn.size, 1));
        rx.resize(tx.size());
        for (int i = 0; i < conn.size; i++)
            tx[i] = (i * 37 + tag * 11) & 255;
    }
    connection &conn;
    uint64_t tag;
    simtime_picosec started = 0;
    bool active = false, posted = false, submitted = false, sent = false, received = false;
    std::vector<unsigned char> tx, rx;
    Trigger *send_trigger = nullptr;
    Trigger *recv_trigger = nullptr;
};

class Simulation : public EventSource {
  public:
    Simulation(EventList &events, ConnectionMatrix &matrix, FatTreeTopology &topology,
               simtime_picosec poll, size_t overhead, uint64_t drop_first)
        : EventSource(events, "suet_progress"), _poll(poll), _overhead(overhead),
          _drop_first(drop_first), _flow(nullptr) {
        _flow.set_flowid(1);
        for (auto *c : *matrix.getAllConnections()) {
            if (c->src < 0 || c->dst < 0 || (uint32_t)c->src >= matrix.N ||
                (uint32_t)c->dst >= matrix.N || c->size < 0)
                throw std::runtime_error("invalid matrix endpoint or size");
            for (uint32_t id : {(uint32_t)c->src, (uint32_t)c->dst}) {
                if (_hosts.count(id))
                    continue;
                auto host = std::make_unique<Host>();
                host->open(id, _fabric);
                _hosts[id] = std::move(host);
                auto port = std::make_unique<FabricPort>(id, stats);
                uint32_t tor = topology.cfg().HOST_POD_SWITCH(id);
                topology.switches_lp[tor]->addHostPort(id, _flow.flow_id(), port.get());
                _ports[id] = std::move(port);
                Route route;
                route.push_back(topology.queues_ns_nlp[id][tor][0]);
                route.push_back(topology.pipes_ns_nlp[id][tor][0]);
                route.push_back(topology.queues_ns_nlp[id][tor][0]->getRemoteEndpoint());
                _routes[id] = std::move(route);
            }
            _hosts[c->src]->add_peer(c->dst, *_hosts[c->dst]);
            _hosts[c->dst]->add_peer(c->src, *_hosts[c->src]);
            auto transfer = std::make_unique<Transfer>(events, *c, _transfers.size() + 1);
            if (c->send_done_trigger)
                transfer->send_trigger = matrix.getTrigger(c->send_done_trigger, events);
            if (c->recv_done_trigger)
                transfer->recv_trigger = matrix.getTrigger(c->recv_done_trigger, events);
            if (c->trigger)
                matrix.getTrigger(c->trigger, events)->add_target(*transfer);
            else
                events.sourceIsPending(*transfer, c->start);
            _transfers.push_back(std::move(transfer));
        }
        events.sourceIsPending(*this, events.now());
    }
    void doNextEvent() override {
        if (!_draining && _completed == _transfers.size() && !stats.in_flight &&
            !htsim_dgram_pending())
            _draining = true;
        if (_draining) {
            _drained = true;
            for (auto &host : _hosts)
                if (!host.second->close())
                    _drained = false;
        } else {
            for (auto &transfer : _transfers) {
                auto &t = *transfer;
                if (!t.active || t.submitted)
                    continue;
                if (!t.posted) {
                    ssize_t ret = fi_trecv(_hosts[t.conn.dst]->ep, t.rx.data(), t.conn.size,
                                           nullptr, FI_ADDR_UNSPEC, t.tag, 0, &t);
                    if (ret == -FI_EAGAIN)
                        continue;
                    check(ret, "fi_trecv");
                    t.posted = true;
                }
                iovec iov{t.tx.data(), (size_t)t.conn.size};
                fi_msg_tagged msg{};
                msg.msg_iov = &iov;
                msg.iov_count = 1;
                msg.addr = _hosts[t.conn.src]->peers.at(t.conn.dst);
                msg.tag = t.tag;
                msg.context = &t;
                ssize_t ret =
                    fi_tsendmsg(_hosts[t.conn.src]->ep, &msg, FI_COMPLETION | FI_DELIVERY_COMPLETE);
                if (ret == -FI_EAGAIN)
                    continue;
                check(ret, "fi_tsendmsg");
                t.submitted = true;
            }
            for (auto &entry : _hosts) {
                auto &host = *entry.second;
                fi_cq_tagged_entry completions[64];
                ssize_t count;
                do {
                    count = fi_cq_read(host.cq, completions, 64);
                    if (count == -FI_EAVAIL) {
                        fi_cq_err_entry err{};
                        check(fi_cq_readerr(host.cq, &err, 0), "fi_cq_readerr");
                        throw std::runtime_error(std::string("SUET completion: ") +
                                                 fi_strerror(err.err));
                    }
                    if (count != -FI_EAGAIN)
                        check(count, "fi_cq_read");
                    for (ssize_t i = 0; i < count; i++) {
                        auto &t = *static_cast<Transfer *>(completions[i].op_context);
                        if (completions[i].flags & FI_RECV) {
                            if (t.received || completions[i].len != (size_t)t.conn.size ||
                                !std::equal(t.tx.begin(), t.tx.begin() + t.conn.size, t.rx.begin()))
                                throw std::runtime_error("payload or completion mismatch");
                            t.received = true;
                            if (t.recv_trigger)
                                t.recv_trigger->activate();
                        } else {
                            if (t.sent)
                                throw std::runtime_error("duplicate send completion");
                            t.sent = true;
                            if (t.send_trigger)
                                t.send_trigger->activate();
                        }
                        if (t.sent && t.received) {
                            _completed++;
                            std::cout << "Flow " << t.conn.src << "->" << t.conn.dst << " id "
                                      << t.conn.flowid << " message " << t.tag << " bytes "
                                      << t.conn.size << " start_us " << timeAsUs(t.started)
                                      << " finish_us " << timeAsUs(EventList::now()) << " fct_us "
                                      << timeAsUs(EventList::now() - t.started) << " verified\n";
                            t.tx.clear();
                            t.tx.shrink_to_fit();
                            t.rx.clear();
                            t.rx.shrink_to_fit();
                        }
                    }
                } while (count == 64);
            }
        }
        for (auto it = _delayed.begin(); it != _delayed.end();) {
            if (it->first <= EventList::now()) {
                it->second->sendOn();
                it = _delayed.erase(it);
            } else
                ++it;
        }
        htsim_dgram_frame frame;
        int ret;
        while ((ret = htsim_dgram_take_tx(&frame)) > 0) {
            if (frame.size >= 2) {
                unsigned type = frame.data[0] >> 3;
                if ((type == 7 || type == 8 || type == 9) && (frame.data[1] & 0x20))
                    stats.ack_marks++;
                if (type == 10 && frame.size >= 12 && (frame.data[2] == 1 || frame.data[2] == 2))
                    stats.trim_nacks++;
                if (type == 8 && frame.size >= 32) {
                    for (size_t i = 16; i < 24; i++)
                        if (frame.data[i]) { stats.sacks++; break; }
                }
                if (type == 2 || type == 3) {
                    if (frame.data[1] & 0x10) stats.retransmits++;
                    if (type != (ordered ? 3U : 2U))
                        throw std::runtime_error("incorrect PDC wire type");
                }
            }
            auto *packet = new FabricPacket(frame, _flow, _routes.at(htsim_dgram_host(&frame.src)),
                                            stats, _overhead);
            unsigned type = frame.data[0] >> 3;
            bool data = type == 2 || type == 3;
            if (data)
                ++_data_packets;
            if (data && trim_every && (_data_packets - 1) % trim_every == 0) {
                packet->strip_payload(64);
                if (trim_lasthop)
                    packet->frame.rx_metadata |= FI_SUET_DGRAM_TRIMMED_LASTHOP;
            }
            if (_drop_first) {
                _drop_first--;
                packet->free();
            } else if (data && ((drop_per_mille && _random() % 1000 < drop_per_mille) ||
                                (burst_every && _data_packets % burst_every < burst_length))) {
                packet->free();
            } else if (data && reorder_every && _data_packets % reorder_every == 0) {
                _delayed.emplace_back(EventList::now() + timeFromUs(10.0), packet);
            } else
                packet->sendOn();
        }
        check(ret, "take datagram");
        if (!finished())
            eventlist().sourceIsPendingRel(*this, _poll);
    }
    bool finished() const { return _drained && !stats.in_flight && !htsim_dgram_pending(); }
    size_t completed() const { return _completed; }
    size_t total() const { return _transfers.size(); }
    void close() {
        if (_fabric) {
            check(fi_close(&_fabric->fid), "close fabric");
            _fabric = nullptr;
        }
    }
    NetworkStats stats;

  private:
    simtime_picosec _poll;
    size_t _overhead;
    uint64_t _drop_first;
    uint64_t _data_packets = 0;
    std::mt19937 _random{1};
    std::vector<std::pair<simtime_picosec, FabricPacket *>> _delayed;
    size_t _completed = 0;
    bool _draining = false, _drained = false;
    fid_fabric *_fabric = nullptr;
    PacketFlow _flow;
    std::map<uint32_t, std::unique_ptr<Host>> _hosts;
    std::map<uint32_t, std::unique_ptr<FabricPort>> _ports;
    std::map<uint32_t, Route> _routes;
    std::vector<std::unique_ptr<Transfer>> _transfers;
};

int main(int argc, char **argv) {
    std::cout << std::unitbuf;
    try {
        std::string matrix_file, topo_file;
        double end_us = 100000, poll_ns = 100, speed_gbps = 100, latency_ns = 1000;
        uint32_t seed = 1, tiers = 3;
        size_t queue_bytes = 1024 * 1024, overhead = 42, capacity = 1024;
        uint64_t drop_first = 0;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "-help" || arg == "--help") {
                std::cout << "htsim_suet -tm FILE [-topo FILE] [-tiers 2|3] [-end US]\n"
                          << "  [-poll_ns NS] [-linkspeed GBPS] [-hop_latency NS]\n"
                          << "  [-q BYTES] [-dgram_queue COUNT] [-overhead BYTES] [-seed N] "
                             "[-drop_first N] [-pdc rod|rud]\n"
                          << "  [-reorder_every N] [-drop_per_mille N] [-burst_every N] [-burst_length N]\n"
                          << "  [-trim_every N] [-trim_lasthop 0|1] [-trim_queues 0|1]\n";
                return 0;
            }
            if (++i == argc)
                throw std::runtime_error("missing option value");
            std::string value = argv[i];
            if (arg == "-tm")
                matrix_file = value;
            else if (arg == "-topo")
                topo_file = value;
            else if (arg == "-end")
                end_us = std::stod(value);
            else if (arg == "-poll_ns")
                poll_ns = std::stod(value);
            else if (arg == "-linkspeed")
                speed_gbps = std::stod(value);
            else if (arg == "-hop_latency")
                latency_ns = std::stod(value);
            else if (arg == "-q")
                queue_bytes = std::stoull(value);
            else if (arg == "-dgram_queue")
                capacity = std::stoull(value);
            else if (arg == "-overhead")
                overhead = std::stoull(value);
            else if (arg == "-seed")
                seed = std::stoul(value);
            else if (arg == "-tiers")
                tiers = std::stoul(value);
            else if (arg == "-trim_every")
                trim_every = std::stoul(value);
            else if (arg == "-trim_queues")
                trim_queues = std::stoul(value) != 0;
            else if (arg == "-trim_lasthop")
                trim_lasthop = std::stoul(value) != 0;
            else if (arg == "-reorder_every")
                reorder_every = std::stoul(value);
            else if (arg == "-drop_per_mille")
                drop_per_mille = std::stoul(value);
            else if (arg == "-burst_every")
                burst_every = std::stoul(value);
            else if (arg == "-burst_length")
                burst_length = std::stoul(value);
            else if (arg == "-pdc") {
                if (value != "rod" && value != "rud")
                    throw std::runtime_error("invalid PDC type");
                ordered = value == "rod";
            } else if (arg == "-drop_first")
                drop_first = std::stoull(value);
            else
                throw std::runtime_error("unknown option: " + arg);
        }
        if (matrix_file.empty() || end_us <= 0 || poll_ns < 0.001 || speed_gbps <= 0 ||
            latency_ns < 0 || queue_bytes < HTSIM_DGRAM_MTU + overhead || overhead > 1024 ||
            !capacity || drop_per_mille > 1000 ||
            (burst_every && burst_length >= burst_every) || (tiers != 2 && tiers != 3))
            throw std::runtime_error("invalid simulation options");
        srand(seed);
        srandom(seed);
        EventList events;
        events.setEndtime(timeFromUs(end_us));
        Packet::set_packet_size(HTSIM_DGRAM_MTU + overhead);
        ConnectionMatrix matrix(0);
        if (!matrix.load(matrix_file.c_str()))
            throw std::runtime_error("cannot load traffic matrix");
        check(htsim_dgram_configure(matrix.N, capacity), "configure datagram provider");
        FatTreeSwitch::set_strategy(FatTreeSwitch::ECMP);
        auto cfg = topo_file.empty()
                       ? std::make_unique<FatTreeTopologyCfg>(
                             tiers, matrix.N, speedFromGbps(speed_gbps), queue_bytes,
                             timeFromNs(latency_ns), 0, trim_queues ? COMPOSITE : ECN, PRIORITY)
                       : FatTreeTopologyCfg::load(topo_file, queue_bytes, trim_queues ? COMPOSITE : ECN, PRIORITY);
        if (cfg->no_of_nodes() != matrix.N)
            throw std::runtime_error("topology/matrix node count mismatch");
        cfg->set_queue_sizes(queue_bytes);
        cfg->set_ecn_parameters(true, true, 2 * HTSIM_DGRAM_MTU, 4 * HTSIM_DGRAM_MTU);
        FatTreeSwitch::_trim_size = 64;
        FatTreeSwitch::_disable_trim = false;
        FatTreeTopology topology(cfg.get(), nullptr, &events, nullptr);
        for (auto *failure : matrix.failures)
            topology.add_failed_link(failure->switch_type, failure->switch_id, failure->link_id);
        Simulation simulation(events, matrix, topology, timeFromNs(poll_ns), overhead, drop_first);
        std::cout << "Starting SUET over htsim: poll_ns " << poll_ns << " seed " << seed << '\n';
        while (events.doNextEvent()) {
        }
        bool complete = simulation.finished();
        std::cout << "SUET result: " << simulation.completed() << '/' << simulation.total()
                  << " messages; packets " << simulation.stats.sent << " received "
                  << simulation.stats.received << " network_drops " << simulation.stats.dropped
                  << " data_evs " << simulation.stats.data_evs.size()
                  << " sacks " << simulation.stats.sacks << " retransmits " << simulation.stats.retransmits
                  << " ecn_marks " << simulation.stats.ecn_marks << " ack_marks " << simulation.stats.ack_marks
                  << " trims " << simulation.stats.trims << " trim_nacks " << simulation.stats.trim_nacks
                  << " receive_drops " << simulation.stats.receive_drops << " elapsed_us "
                  << timeAsUs(events.now()) << '\n';
        if (!complete) {
            std::cerr << "Simulation ended with incomplete transfers or undelivered packets\n";
            return 2;
        }
        simulation.close();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "htsim_suet: " << error.what() << '\n';
        return 1;
    }
}
