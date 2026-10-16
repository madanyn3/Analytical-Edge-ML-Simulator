#include "edge/cluster.hpp"
#include "edge/checked.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>

namespace edge {
namespace {
using U=std::uint64_t;
using checked::add;
using checked::mul;
enum class Kind { req, resp, mreq, mresp };
// What a packet carries. REQ/RESP name an L1 line; MREQ/MRESP an L2 line.
struct PacketInfo { Kind kind; std::size_t core; U bank; U line; };
struct BankRequest { U arrival, source, l1_line, seq; std::size_t core; };
struct MemoryRequest { U arrival, bank, l2_line, seq; };

// Exact LRU on one set (MRU first). Same rules as the version-1 hierarchy.
bool touch(std::vector<U>& set,U line) {
    auto it=std::find(set.begin(),set.end(),line);
    if (it==set.end()) return false;
    set.erase(it); set.insert(set.begin(),line);
    return true;
}
void install(std::vector<U>& set,U line,U ways,LevelStats& s) {
    if (set.size()==ways) { set.pop_back(); s.evictions=add(s.evictions,1); }
    set.insert(set.begin(),line);
}

class Evaluator {
public:
    explicit Evaluator(const ClusterConfig& config)
        :c_(config),net_(config.topology),map_{config.l2.geometry.line_bytes,config.topology.k*config.topology.k,config.l2.geometry.sets},
         memory_(config.memory) {
        validate_cluster_config(c_);
        if (c_.stimulus.engine!="finite") throw std::invalid_argument("periodic_state engine is not implemented yet");
        const U tiles=map_.banks;
        r_.cores.resize(c_.cores.size());
        r_.l1_state.assign(c_.cores.size(),CacheState(c_.l1.geometry.sets));
        r_.banks.resize(tiles);
        r_.l2_state.assign(tiles,CacheState(c_.l2.geometry.sets));
        banks_.resize(tiles);
        cores_.resize(c_.cores.size());
    }
    ClusterResult run() {
        // a_(p,0) = start_p + c_0: every core's first read begins its private walk.
        for (std::size_t p=0;p<cores_.size();++p) begin(p,add(c_.cores[p].start_cycles,c_.cores[p].compute_cycles));
        for (;;) {
            const auto t=next_time();
            if (!t) break;
            if (++r_.events>c_.stimulus.max_events) throw std::length_error("event budget exceeded");
            // Phase 1: deliveries. Phase 2 (core issues/lookups) happens inside the
            // handlers: private L1 work never depends on other cores (BK3).
            for (auto d:net_.deliveries(*t)) deliver(d.id,*t);
            // Phase 3: grants in fixed resource order: memory controller, banks, links.
            if (memory_grant_time()==t) grant_memory(*t);
            for (U b=0;b<banks_.size();++b) if (bank_grant_time(b)==t) grant_bank(b,*t);
            net_.grants(*t);
        }
        for (std::size_t p=0;p<cores_.size();++p)
            if (cores_[p].next<c_.cores[p].count) throw std::logic_error("cluster stalled with unfinished cores");
        r_.memory_state=memory_.state();
        const auto& topo=net_.topology();
        r_.links.resize(topo.tiles()*noc::port_count);
        for (U tile=0;tile<topo.tiles();++tile) for (std::size_t port=1;port<noc::port_count;++port) {
            const noc::Link l{tile,static_cast<noc::Port>(port)};
            if (topo.has_link(tile,l.out)) r_.links[topo.link_index(l)]=net_.link_stats(l);
        }
        return std::move(r_);
    }
private:
    struct Core { U next{}, issue{}; std::vector<U> lines; std::size_t ref{}; };
    struct Bank { std::vector<BankRequest> queue; U free{}; bool waiting_memory{}; U pointer{}; BankRequest serving{}; U granted{}; };
    const ClusterConfig& c_;
    noc::Network net_;
    noc::BankMap map_;
    MemoryService memory_;
    ClusterResult r_;
    std::vector<Core> cores_;
    std::vector<Bank> banks_;
    std::vector<MemoryRequest> memory_queue_;
    std::map<U,PacketInfo> packets_;
    U next_packet_{}, seq_{};

    void charge() {
        if (++r_.transactions>c_.stimulus.max_transactions) throw std::length_error("transaction budget exceeded");
    }
    void send(PacketInfo info,U source,U target,U flits,U time) {
        const U id=next_packet_++;
        packets_.emplace(id,info);
        net_.inject({id,source,target,flits},time);
    }
    U bank_tile(U b) const { return b; } // One bank per tile; bank index = tile index.
    // Start read `next` of core p at its issue time: split into ascending L1 lines.
    void begin(std::size_t p,U issue) {
        auto& core=cores_[p];
        const auto& k=c_.cores[p];
        if (core.next>=k.count) return;
        if (r_.reads.size()>=c_.stimulus.max_requests) throw std::length_error("request budget exceeded");
        const U a=add(k.address,mul(core.next%k.address_period,k.stride));
        const U l1=c_.l1.geometry.line_bytes;
        core.lines.clear();
        for (U line=a/l1;;++line) { core.lines.push_back(line); if (line==add(a,k.bytes-1)/l1) break; }
        core.issue=issue; core.ref=0;
        walk(p,issue);
    }
    // Serial L1 line references from time t. Hits continue; the first miss sends a
    // REQ and suspends the core until its RESP is delivered (blocking core, CL1).
    void walk(std::size_t p,U t) {
        auto& core=cores_[p];
        auto& stats=r_.cores[p];
        const auto& g=c_.l1.geometry;
        for (;;) {
            if (core.ref==core.lines.size()) {
                // Read complete at t: a_(k+1) = d_k + c_(k+1).
                const U latency=t-core.issue;
                stats.min_latency=stats.reads?std::min(stats.min_latency,latency):latency;
                stats.max_latency=std::max(stats.max_latency,latency);
                stats.latency_sum=add(stats.latency_sum,latency);
                stats.reads=add(stats.reads,1);
                stats.last_done=t;
                r_.reads.push_back({p,core.next,core.issue,t});
                ++core.next;
                const auto& k=c_.cores[p];
                if (core.next>=k.count) return;
                // Iterative rather than recursive: long all-hit streams stay flat.
                const U issue=add(t,k.compute_cycles);
                if (r_.reads.size()>=c_.stimulus.max_requests) throw std::length_error("request budget exceeded");
                const U a=add(k.address,mul(core.next%k.address_period,k.stride));
                core.lines.clear();
                for (U line=a/g.line_bytes;;++line) { core.lines.push_back(line); if (line==add(a,k.bytes-1)/g.line_bytes) break; }
                core.issue=issue; core.ref=0; t=issue;
                continue;
            }
            charge();
            const U line=core.lines[core.ref];
            stats.l1.references=add(stats.l1.references,1);
            t=add(t,c_.l1.lookup_cycles);
            if (touch(r_.l1_state[p][line%g.sets],line)) { stats.l1.hits=add(stats.l1.hits,1); ++core.ref; continue; }
            stats.l1.misses=add(stats.l1.misses,1);
            // Nested lines: the whole L1 line lies in one L2 line, hence one home bank.
            const U bank=map_.bank(mul(line,g.line_bytes));
            send({Kind::req,p,bank,line},c_.cores[p].tile,bank_tile(bank),noc::flits(0,c_.topology.flit_bytes),t);
            return;
        }
    }
    void deliver(U id,U t) {
        const auto info=packets_.at(id);
        packets_.erase(id);
        switch (info.kind) {
        case Kind::req:
            banks_[info.bank].queue.push_back({t,c_.cores[info.core].tile,info.line,seq_++,info.core});
            break;
        case Kind::mreq:
            memory_queue_.push_back({t,info.bank,info.line,seq_++});
            break;
        case Kind::mresp: {
            // L2 fill, then the bank answers the core it was serving (blocking bank, BK1).
            auto& bank=banks_[info.bank];
            auto& s=r_.banks[info.bank];
            const U ready=add(t,c_.l2.fill_cycles);
            install(r_.l2_state[info.bank][map_.set(mul(info.line,map_.line_bytes))],info.line,c_.l2.geometry.ways,s.l2);
            s.l2.fill_bytes=add(s.l2.fill_bytes,map_.line_bytes);
            respond(info.bank,ready);
            s.busy_cycles=add(s.busy_cycles,ready-bank.granted);
            bank.waiting_memory=false;
            break;
        }
        case Kind::resp: {
            auto& core=cores_[info.core];
            auto& stats=r_.cores[info.core];
            const auto& g=c_.l1.geometry;
            install(r_.l1_state[info.core][info.line%g.sets],info.line,g.ways,stats.l1);
            stats.l1.fill_bytes=add(stats.l1.fill_bytes,g.line_bytes);
            ++core.ref;
            walk(info.core,add(t,c_.l1.fill_cycles));
            break;
        }
        }
    }
    void respond(U b,U time) {
        auto& bank=banks_[b];
        const auto& req=bank.serving;
        send({Kind::resp,req.core,b,req.l1_line},bank_tile(b),req.source,
             noc::flits(c_.l1.geometry.line_bytes,c_.topology.flit_bytes),time);
        bank.free=time;
    }
    std::optional<U> bank_grant_time(U b) const {
        const auto& bank=banks_[b];
        if (bank.waiting_memory || bank.queue.empty()) return std::nullopt;
        U earliest=bank.queue.front().arrival;
        for (const auto& q:bank.queue) earliest=std::min(earliest,q.arrival);
        return std::max(bank.free,earliest);
    }
    void grant_bank(U b,U t) {
        auto& bank=banks_[b];
        auto& s=r_.banks[b];
        auto eligible=[&](const BankRequest& q){return q.arrival<=t;};
        auto older=[](const BankRequest& x,const BankRequest& y){return std::tie(x.arrival,x.source,x.seq)<std::tie(y.arrival,y.source,y.seq);};
        auto best=bank.queue.end();
        const auto& policy=c_.l2.arbitration;
        if (policy=="round_robin") {
            for (U k=0;k<map_.banks && best==bank.queue.end();++k) {
                const U tile=(bank.pointer+k)%map_.banks;
                for (auto it=bank.queue.begin();it!=bank.queue.end();++it)
                    if (eligible(*it) && it->source==tile && (best==bank.queue.end() || older(*it,*best))) best=it;
            }
            bank.pointer=(best->source+1)%map_.banks;
        } else {
            for (auto it=bank.queue.begin();it!=bank.queue.end();++it) {
                if (!eligible(*it)) continue;
                const bool better=best==bank.queue.end() || (policy=="fixed_priority"
                    ? std::tie(it->source,it->arrival,it->seq)<std::tie(best->source,best->arrival,best->seq)
                    : older(*it,*best));
                if (better) best=it;
            }
        }
        bank.serving=*best;
        bank.queue.erase(best);
        bank.granted=t;
        charge();
        s.grants=add(s.grants,1);
        s.wait_cycles=add(s.wait_cycles,t-bank.serving.arrival);
        s.l2.references=add(s.l2.references,1);
        const U done=add(t,c_.l2.lookup_cycles);
        const U l2_line=mul(bank.serving.l1_line,c_.l1.geometry.line_bytes)/map_.line_bytes;
        if (touch(r_.l2_state[b][map_.set(mul(l2_line,map_.line_bytes))],l2_line)) {
            s.l2.hits=add(s.l2.hits,1);
            s.busy_cycles=add(s.busy_cycles,c_.l2.lookup_cycles);
            respond(b,done);
        } else {
            s.l2.misses=add(s.l2.misses,1);
            bank.waiting_memory=true; // Busy through the whole memory round trip (BK1).
            send({Kind::mreq,0,b,l2_line},bank_tile(b),c_.topology.mc_tile,noc::flits(0,c_.topology.flit_bytes),done);
        }
    }
    std::optional<U> memory_grant_time() const {
        if (memory_queue_.empty()) return std::nullopt;
        U earliest=memory_queue_.front().arrival;
        for (const auto& q:memory_queue_) earliest=std::min(earliest,q.arrival);
        return std::max(memory_.earliest_admission(),earliest);
    }
    void grant_memory(U t) {
        auto best=memory_queue_.end();
        for (auto it=memory_queue_.begin();it!=memory_queue_.end();++it)
            if (it->arrival<=t && (best==memory_queue_.end() ||
                std::tie(it->arrival,it->bank,it->seq)<std::tie(best->arrival,best->bank,best->seq))) best=it;
        const auto q=*best;
        memory_queue_.erase(best);
        charge();
        // Submitted at the (nondecreasing) grant time, not the original arrival, so
        // the service's arrival-order rule holds; it must then start exactly at t.
        const auto done=memory_.submit({t,mul(q.l2_line,map_.line_bytes),map_.line_bytes});
        if (done.start!=t) throw std::logic_error("memory admission disagrees with earliest_admission");
        r_.memory_requests=add(r_.memory_requests,1);
        r_.memory_wire_bytes=add(r_.memory_wire_bytes,done.wire_bytes);
        r_.memory_wait_cycles=add(r_.memory_wait_cycles,t-q.arrival);
        send({Kind::mresp,0,q.bank,q.l2_line},c_.topology.mc_tile,bank_tile(q.bank),
             noc::flits(map_.line_bytes,c_.topology.flit_bytes),done.done);
    }
    std::optional<U> next_time() const {
        std::optional<U> t=net_.next_time();
        auto consider=[&](std::optional<U> v){ if (v) t=t?std::min(*t,*v):*v; };
        consider(memory_grant_time());
        for (U b=0;b<banks_.size();++b) consider(bank_grant_time(b));
        return t;
    }
};
}
ClusterResult run_cluster(const ClusterConfig& c) { return Evaluator(c).run(); }
}
