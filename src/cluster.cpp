#include "edge/cluster.hpp"
#include "edge/checked.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <string>
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
// Local-port tie order (design §4.3): memory responses, bank responses, bank memory
// requests, core requests. Each tile injects at most one packet per class per tick,
// so packet ids never decide arbitration.
U rank(Kind k) {
    switch (k) { case Kind::mresp: return 0; case Kind::resp: return 1; case Kind::mreq: return 2; case Kind::req: return 3; }
    return 3;
}
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
        periodic_=c_.stimulus.engine=="periodic_state";
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
        std::optional<U> last;
        for (;;) {
            const auto t=next_time();
            if (!t) break;
            // Checkpoint only between distinct times, when everything before t is done.
            if (periodic_ && (!last || *t>*last) && checkpoint(*t)) continue;
            last=t;
            if (++r_.events>c_.stimulus.max_events) throw std::length_error("event budget exceeded");
            // Phase 1: deliveries. Phase 2 (core issues/lookups) happens inside the
            // handlers: private L1 work never depends on other cores (BK3).
            for (auto d:net_.deliveries(*t)) deliver(d.id,*t);
            // Phase 2: cores whose next read issues now start their private L1 walk.
            for (std::size_t p=0;p<cores_.size();++p)
                if (cores_[p].resuming && cores_[p].issue==*t) { cores_[p].resuming=false; walk(p,*t); }
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
    // `resuming`: the next read is set up and its L1 walk starts at `issue` (phase 2).
    struct Core { U next{}, issue{}; std::vector<U> lines; std::size_t ref{}; bool resuming{}; };
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

    // ---- periodic_state: exact period skipping (design §5) -------------------------
    // Counters at a checkpoint, to form per-period deltas.
    struct Snapshot {
        U time;
        std::vector<U> next;
        std::vector<CoreStats> cores;
        std::vector<BankStats> banks;
        std::vector<noc::LinkStats> links;
        U memory_requests, memory_wire_bytes, memory_wait_cycles;
    };
    bool periodic_{};
    std::map<std::string,Snapshot> seen_;
    std::size_t seen_bytes_{};
    static constexpr std::size_t key_budget_bytes=64u<<20; // Beyond it: no new keys.
    std::optional<std::pair<std::size_t,U>> mark_;       // (clock core, block) last keyed.

    // Canonical state relative to T (every event before T processed). Two equal keys
    // at T1 < T2 mean the whole system at T2 is the system at T1 translated by T2 - T1:
    // every rule of the model compares times only with each other or adds constants,
    // so the futures coincide up to that translation, counter increments included.
    //  - cores: read index mod address_period (or finished), issue time, line cursor;
    //  - L1 and L2 contents (absolute line numbers: periodic streams revisit lines);
    //  - banks: free clamped to T (a past free time cannot delay a grant at >= T),
    //    pointer, the request served during a miss and its grant time, sorted queue;
    //  - memory queue (sorted) and service state: next_issue clamped to T and only
    //    completions after T (credits completing by T are free for any admission at
    //    >= T; earliest_admission then agrees wherever a grant time is defined);
    //  - network: Network::state_key, packets described by content.
    // Sequence numbers and packet ids are omitted: they never decide a tie.
    std::string key(U T) const {
        std::string k;
        // Raw 8-byte words; every variable-length part is preceded by its length, so the
        // encoding is injective. Offsets are v - T (mod 2^64).
        auto num=[&](U v){ k.append(reinterpret_cast<const char*>(&v),sizeof v); };
        for (std::size_t p=0;p<cores_.size();++p) {
            const auto& core=cores_[p];
            const auto& cfg=c_.cores[p];
            if (core.next>=cfg.count) { num(~U{0}); continue; }   // Finished.
            num(core.next%cfg.address_period); num(core.issue-T); num(core.ref); num(core.resuming);
        }
        auto cache=[&](const CacheState& state) {
            for (const auto& set:state) { num(set.size()); for (U line:set) num(line); }
        };
        for (const auto& st:r_.l1_state) cache(st);
        for (const auto& st:r_.l2_state) cache(st);
        std::vector<std::array<U,4>> q;
        for (const auto& bank:banks_) {
            num(bank.free>T?bank.free-T:0); num(bank.pointer); num(bank.waiting_memory);
            if (bank.waiting_memory) { num(bank.serving.source); num(bank.serving.l1_line); num(bank.serving.core); num(bank.granted-T); }
            q.clear();
            for (const auto& r:bank.queue) q.push_back({r.arrival-T,r.source,r.l1_line,static_cast<U>(r.core)});
            std::sort(q.begin(),q.end());
            num(q.size());
            for (const auto& e:q) for (U v:e) num(v);
        }
        std::vector<std::array<U,3>> mq;
        for (const auto& r:memory_queue_) mq.push_back({r.arrival-T,r.bank,r.l2_line});
        std::sort(mq.begin(),mq.end());
        num(mq.size());
        for (const auto& e:mq) for (U v:e) num(v);
        const auto& m=memory_.state();
        num(m.next_issue>T?m.next_issue-T:0);
        U live=0;
        for (U done:m.pending) live+=done>T;
        num(live);
        for (U done:m.pending) if (done>T) num(done-T);
        net_.state_key(k,T,[&](U id) {
            const auto& i=packets_.at(id);
            return noc::Network::Describe{static_cast<U>(i.kind),static_cast<U>(i.core),i.bank,i.line};
        });
        return k;
    }
    Snapshot snapshot(U T) const {
        Snapshot s{T,{},r_.cores,r_.banks,net_.all_link_stats(),r_.memory_requests,r_.memory_wire_bytes,r_.memory_wait_cycles};
        for (const auto& core:cores_) s.next.push_back(core.next);
        return s;
    }
    // Keys are formed when the clock core (first unfinished core) enters a new block of
    // address_period reads. The rule depends only on the state, so in a periodic regime
    // it samples the same phase every period. Returns true if it skipped.
    bool checkpoint(U T) {
        std::optional<std::size_t> clock;
        for (std::size_t p=0;p<cores_.size() && !clock;++p) if (cores_[p].next<c_.cores[p].count) clock=p;
        if (!clock) return false;
        const std::pair<std::size_t,U> mark{*clock,cores_[*clock].next/c_.cores[*clock].address_period};
        if (mark_==mark) return false;
        mark_=mark;
        auto k=key(T);
        const auto it=seen_.find(k);
        if (it==seen_.end()) {
            if (seen_bytes_+k.size()<=key_budget_bytes) { seen_bytes_+=k.size(); seen_.emplace(std::move(k),snapshot(T)); }
            return false;
        }
        return skip(it->second,T);
    }
    // State at T equals the state at s.time translated by delta. Skip m whole periods,
    // with m chosen so every unfinished core is still unfinished afterwards: up to that
    // point the finite run equals the unbounded periodic one.
    bool skip(const Snapshot& s,U T) {
        const U delta=T-s.time;
        if (!delta) return false;
        U m=std::numeric_limits<U>::max();
        std::vector<U> step(cores_.size());
        for (std::size_t p=0;p<cores_.size();++p) {
            const U count=c_.cores[p].count;
            step[p]=cores_[p].next-s.next[p];
            if (cores_[p].next>=count) { if (step[p]) throw std::logic_error("finished core advanced"); continue; }
            if (!step[p]) return false; // Cannot happen for equal keys; refuse rather than assume.
            m=std::min(m,(count-1-cores_[p].next)/step[p]);
        }
        // Leave at least one whole period to evaluate after the skip (m = q - 1): any
        // memory credit that completed by T and is dead in the key, but still listed by
        // the service, is then pruned by a real admission exactly as in the finite run.
        if (m==std::numeric_limits<U>::max() || m<2) return false;
        --m;
        const U shift=mul(m,delta);
        auto bump=[&](U& now,U before){ now=add(now,mul(now-before,m)); };
        auto bump_level=[&](LevelStats& now,const LevelStats& b) {
            bump(now.references,b.references); bump(now.hits,b.hits); bump(now.misses,b.misses);
            bump(now.evictions,b.evictions); bump(now.invalidations,b.invalidations); bump(now.fill_bytes,b.fill_bytes);
        };
        U reads=0;
        for (std::size_t p=0;p<cores_.size();++p) {
            if (!step[p]) continue;
            auto& core=cores_[p];
            auto& st=r_.cores[p];
            const auto& b=s.cores[p];
            if (b.reads && st.last_done!=add(b.last_done,delta)) throw std::logic_error("period skip: last completion not translated");
            core.next=add(core.next,mul(step[p],m));
            core.issue=add(core.issue,shift);
            bump(st.reads,b.reads); bump(st.latency_sum,b.latency_sum); bump_level(st.l1,b.l1);
            st.last_done=add(st.last_done,shift);  // min/max latency repeat unchanged.
            reads=add(reads,mul(step[p],m));
        }
        for (std::size_t i=0;i<banks_.size();++i) {
            auto& bank=banks_[i];
            for (auto& q:bank.queue) q.arrival=add(q.arrival,shift);
            bank.free=add(bank.free,shift); bank.granted=add(bank.granted,shift);
            bank.serving.arrival=add(bank.serving.arrival,shift);
            auto& st=r_.banks[i];
            const auto& b=s.banks[i];
            bump_level(st.l2,b.l2); bump(st.grants,b.grants); bump(st.wait_cycles,b.wait_cycles); bump(st.busy_cycles,b.busy_cycles);
        }
        for (auto& q:memory_queue_) q.arrival=add(q.arrival,shift);
        // A period without admissions leaves the service literally unchanged (its times
        // are all <= T, hence clamped or dropped in the key): keep it as is, which is
        // what the finite run holds. Otherwise translate it with everything else.
        if (r_.memory_requests>s.memory_requests) {
            auto ms=memory_.state();
            ms.next_issue=add(ms.next_issue,shift);
            for (auto& d:ms.pending) d=add(d,shift);
            ms.last_arrival=add(ms.last_arrival,shift);
            memory_=MemoryService(c_.memory,ms);
        }
        bump(r_.memory_requests,s.memory_requests); bump(r_.memory_wire_bytes,s.memory_wire_bytes);
        bump(r_.memory_wait_cycles,s.memory_wait_cycles);
        net_.skip(shift,s.links,m);
        r_.skipped_reads=add(r_.skipped_reads,reads);
        r_.skipped_cycles=add(r_.skipped_cycles,shift);
        r_.skipped_periods=add(r_.skipped_periods,m);
        // Stored snapshots describe pre-skip times; start afresh.
        seen_.clear(); seen_bytes_=0; mark_.reset();
        return true;
    }

    void charge() {
        if (++r_.transactions>c_.stimulus.max_transactions) throw std::length_error("transaction budget exceeded");
    }
    void send(PacketInfo info,U source,U target,U flits,U time) {
        const U id=next_packet_++;
        packets_.emplace(id,info);
        net_.inject({id,source,target,flits,rank(info.kind)},time);
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
        core.issue=issue; core.ref=0; core.resuming=true;
    }
    // Serial L1 line references of the current read from time t. Hits continue; the
    // first miss sends a REQ and suspends the core until its RESP is delivered
    // (blocking core, CL1). A completed read sets up the next one, which resumes as its
    // own event at its issue time: a core never runs ahead of the event loop, so
    // periodic_state checkpoints also see long all-hit streams.
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
                begin(p,add(t,c_.cores[p].compute_cycles));   // a_(k+1) = d_k + c_(k+1).
                return;
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
        for (const auto& core:cores_) if (core.resuming) consider(core.issue);
        consider(memory_grant_time());
        for (U b=0;b<banks_.size();++b) consider(bank_grant_time(b));
        return t;
    }
};
}
ClusterResult run_cluster(const ClusterConfig& c) { return Evaluator(c).run(); }
}
