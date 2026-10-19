// Cluster evaluator: hand-derived golden cases on the design's 2x2 mesh
// (docs/cluster-design.md §3, §6 N1, §7.2). t_r = t_l = 1, 16-byte flits,
// L1 32 B lines (RESP = 3 flits), L2 64 B lines (MRESP = 5 flits), MC on tile 0,
// memory D = 40 + 3*2 = 46 cycles per 64-byte line.
#include "edge/cluster.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <stdexcept>
#include <string>

namespace {
using U=std::uint64_t;
using edge::CacheState;
void check(bool ok,const std::string& what) { if (!ok) throw std::runtime_error(what); }
edge::ClusterConfig base() {
    edge::ClusterConfig c;
    c.clock_hz=1000000000;
    c.topology={"mesh",2,0,1,1,16,"round_robin","test"};
    c.l1={{32,4,2},1,1,"","test"};
    c.l2={{64,16,4},4,2,"round_robin","test"};
    c.memory={40,16,2,2,4,"test"};
    c.stimulus={"finite",1000,100000,1000000};
    return c;
}
edge::CoreConfig core(const char* name,U tile,U count,U address,U stride,U period) {
    return {name,tile,0,10,count,address,4,stride,period};
}

// ---------------------------------------------------------------------------------
// Independent tick oracle for the whole cluster, written from the design text.
// It steps every tick; keeps an agenda of timed actions; uses timestamp LRU maps,
// its own bank mapping arithmetic, its own link arbitration and a tick memory model.
// Shared with production: only Topology::route (verified separately in noc_tests).
// Remaining link ties after class rank are broken by REVERSE packet id, the opposite
// of production: agreement therefore shows that ids never decide arbitration.
// Each tick: (A) run all actions due now, to a fixpoint (local same-tick chains);
// (B) grants: memory controller, banks by index, links in REVERSE order.
struct ClusterOracle {
    const edge::ClusterConfig& c;
    edge::noc::Topology topo;
    U B, now{}, stamp{};
    enum class K { issue, lookup, inject, deliver, fill, bank_fill };
    enum class P { req, resp, mreq, mresp };
    struct Pkt { P kind; U core, bank, line, src, dst, flits; std::vector<edge::noc::Link> path; std::size_t hop{}; U ready{}; int port{};
        U rank() const { return kind==P::mresp?0:kind==P::resp?1:kind==P::mreq?2:3; } };
    struct Act { K kind; U who; U pkt; };
    std::multimap<U,Act> agenda;
    std::map<U,Pkt> pkts; U next_pkt{};
    struct Core { U next{}, issue{}; std::vector<U> lines; std::size_t ref{}; };
    std::vector<Core> cores;
    std::vector<std::vector<std::map<U,U>>> l1, l2; // set -> (line -> last-use stamp)
    struct Bank { std::vector<std::tuple<U,U,U,U>> q; U free{}; bool mem{}; U ptr{}; U core{}, line{}, src{}, granted{}; };
    std::vector<Bank> banks;
    std::vector<std::tuple<U,U,U>> mq;                // (arrival, bank, l2 line)
    U issue_ready{}; std::vector<U> credits;          // tick memory
    std::map<U,U> busy; std::map<U,int> ptr;          // per link index
    std::vector<U> waiting;                           // packets in the network
    edge::ClusterResult r;
    explicit ClusterOracle(const edge::ClusterConfig& cfg):c(cfg),topo(cfg.topology),B(cfg.topology.k*cfg.topology.k) {
        cores.resize(c.cores.size());
        l1.assign(c.cores.size(),std::vector<std::map<U,U>>(c.l1.geometry.sets));
        l2.assign(B,std::vector<std::map<U,U>>(c.l2.geometry.sets));
        banks.resize(B); r.cores.resize(c.cores.size()); r.banks.resize(B);
        r.links.resize(B*5);
        for (U p=0;p<cores.size();++p) if (c.cores[p].count) agenda.insert({c.cores[p].start_cycles+c.cores[p].compute_cycles,{K::issue,p,0}});
    }
    U flits(U bytes) const { return 1+(bytes+c.topology.flit_bytes-1)/c.topology.flit_bytes; }
    void send(P kind,U core,U bank,U line,U src,U dst,U bytes,U at) {
        pkts[next_pkt]={kind,core,bank,line,src,dst,flits(bytes),topo.route(src,dst)};
        agenda.insert({at,{K::inject,0,next_pkt++}});
    }
    bool lru_hit(std::map<U,U>& set,U line) { auto it=set.find(line); if (it==set.end()) return false; it->second=++stamp; return true; }
    void lru_fill(std::map<U,U>& set,U line,U ways,edge::LevelStats& s) {
        if (set.size()==ways) {
            auto victim=std::min_element(set.begin(),set.end(),[](auto& a,auto& b){return a.second<b.second;});
            set.erase(victim); ++s.evictions;
        }
        set[line]=++stamp;
    }
    void complete(U p,U t) {
        auto& k=cores[p]; auto& s=r.cores[p];
        const U lat=t-k.issue;
        s.min_latency=s.reads?std::min(s.min_latency,lat):lat; s.max_latency=std::max(s.max_latency,lat);
        s.latency_sum+=lat; ++s.reads; s.last_done=t;
        r.reads.push_back({p,k.next,k.issue,t});
        if (++k.next<c.cores[p].count) agenda.insert({t+c.cores[p].compute_cycles,{K::issue,p,0}});
    }
    void act(const Act& a,U t) {
        switch (a.kind) {
        case K::issue: {
            auto& k=cores[a.who]; const auto& cc=c.cores[a.who];
            const U addr=cc.address+(k.next%cc.address_period)*cc.stride, L=c.l1.geometry.line_bytes;
            k.lines.clear(); for (U b=0;b<cc.bytes;++b) if (k.lines.empty() || k.lines.back()!=(addr+b)/L) k.lines.push_back((addr+b)/L);
            k.issue=t; k.ref=0; agenda.insert({t+c.l1.lookup_cycles,{K::lookup,a.who,0}}); break;
        }
        case K::lookup: {
            auto& k=cores[a.who]; auto& s=r.cores[a.who].l1; const U line=k.lines[k.ref];
            ++s.references;
            if (lru_hit(l1[a.who][line%c.l1.geometry.sets],line)) {
                ++s.hits;
                if (++k.ref<k.lines.size()) agenda.insert({t+c.l1.lookup_cycles,{K::lookup,a.who,0}}); else complete(a.who,t);
            } else {
                ++s.misses;
                const U bank=(line*c.l1.geometry.line_bytes/c.l2.geometry.line_bytes)%B;
                send(P::req,a.who,bank,line,c.cores[a.who].tile,bank,0,t);
            }
            break;
        }
        case K::inject: {
            auto& x=pkts[a.pkt];
            if (x.path.empty()) { agenda.insert({t,{K::deliver,0,a.pkt}}); break; }
            x.hop=0; x.ready=t+c.topology.router_cycles; x.port=0; waiting.push_back(a.pkt); break;
        }
        case K::deliver: {
            const auto x=pkts[a.pkt]; pkts.erase(a.pkt);
            if (x.kind==P::req) banks[x.bank].q.push_back({t,c.cores[x.core].tile,x.line,x.core});
            else if (x.kind==P::mreq) mq.push_back({t,x.bank,x.line});
            else if (x.kind==P::mresp) agenda.insert({t+c.l2.fill_cycles,{K::bank_fill,x.bank,x.line}});
            else agenda.insert({t+c.l1.fill_cycles,{K::fill,x.core,x.line}});
            break;
        }
        case K::fill: {
            auto& k=cores[a.who]; auto& s=r.cores[a.who].l1;
            lru_fill(l1[a.who][a.pkt%c.l1.geometry.sets],a.pkt,c.l1.geometry.ways,s); s.fill_bytes+=c.l1.geometry.line_bytes;
            if (++k.ref<k.lines.size()) agenda.insert({t+c.l1.lookup_cycles,{K::lookup,a.who,0}}); else complete(a.who,t);
            break;
        }
        case K::bank_fill: {
            auto& b=banks[a.who]; auto& s=r.banks[a.who];
            const U line=a.pkt;
            lru_fill(l2[a.who][(line/B)%c.l2.geometry.sets],line,c.l2.geometry.ways,s.l2); s.l2.fill_bytes+=c.l2.geometry.line_bytes;
            send(P::resp,b.core,a.who,b.line,a.who,b.src,c.l1.geometry.line_bytes,t);
            b.free=t; b.mem=false; s.busy_cycles+=t-b.granted; break;
        }
        }
    }
    void grants(U t) {
        // Memory controller (tick model: credit free iff its completion <= t).
        if (!mq.empty() && t>=issue_ready && static_cast<U>(std::count_if(credits.begin(),credits.end(),[&](U d){return d>t;}))<c.memory.outstanding) {
            auto best=mq.end();
            for (auto it=mq.begin();it!=mq.end();++it) if (std::get<0>(*it)<=t && (best==mq.end() ||
                std::tie(std::get<0>(*it),std::get<1>(*it))<std::tie(std::get<0>(*best),std::get<1>(*best)))) best=it;
            if (best!=mq.end()) {
                const auto [arr,bank,line]=*best; mq.erase(best);
                const U addr=line*c.l2.geometry.line_bytes, n=c.l2.geometry.line_bytes;
                std::set<U> beats; for (U b=0;b<n;++b) beats.insert((addr+b)/c.memory.beat_bytes);
                const U done=t+c.memory.first_beat_cycles+(beats.size()-1)*c.memory.beat_cycles;
                issue_ready=t+std::max(c.memory.issue_cycles,static_cast<U>(beats.size())*c.memory.beat_cycles);
                credits.push_back(done);
                ++r.memory_requests; r.memory_wire_bytes+=beats.size()*c.memory.beat_bytes; r.memory_wait_cycles+=t-arr;
                send(P::mresp,0,bank,line,c.topology.mc_tile,bank,n,done);
            }
        }
        for (U b=0;b<B;++b) {
            auto& bank=banks[b];
            if (bank.mem || t<bank.free) continue;
            int best=-1;
            for (int i=0;i<static_cast<int>(bank.q.size());++i) {
                const auto& [arr,src,line,core]=bank.q[static_cast<std::size_t>(i)];
                if (arr>t) continue;
                if (best<0) { best=i; continue; }
                const auto& o=bank.q[static_cast<std::size_t>(best)];
                const auto& pol=c.l2.arbitration;
                bool better;
                if (pol=="round_robin") better=(src+B-bank.ptr)%B<(std::get<1>(o)+B-bank.ptr)%B;
                else if (pol=="fixed_priority") better=src<std::get<1>(o);
                else better=std::tie(arr,src)<std::tie(std::get<0>(o),std::get<1>(o));
                if (better) best=i;
            }
            if (best<0) continue;
            const auto [arr,src,line,core]=bank.q[static_cast<std::size_t>(best)];
            bank.q.erase(bank.q.begin()+best);
            if (c.l2.arbitration=="round_robin") bank.ptr=(src+1)%B;
            auto& s=r.banks[b]; ++s.grants; s.wait_cycles+=t-arr; ++s.l2.references;
            const U l2line=line*c.l1.geometry.line_bytes/c.l2.geometry.line_bytes;
            bank.core=core; bank.line=line; bank.src=src; bank.granted=t;
            const U done=t+c.l2.lookup_cycles;
            if (lru_hit(l2[b][(l2line/B)%c.l2.geometry.sets],l2line)) {
                ++s.l2.hits; s.busy_cycles+=c.l2.lookup_cycles; bank.free=done;
                send(P::resp,core,b,line,b,src,c.l1.geometry.line_bytes,done);
            } else {
                ++s.l2.misses; bank.mem=true;
                send(P::mreq,0,b,l2line,b,c.topology.mc_tile,0,done);
            }
        }
        for (U tile=B;tile-->0;) for (int dir=4;dir>=1;--dir) {
            const U li=tile*5+static_cast<U>(dir);
            if (!topo.has_link(tile,static_cast<edge::noc::Port>(dir)) || busy[li]>t) continue;
            U win=~U{0};
            for (U id:waiting) {
                const auto& x=pkts[id];
                if (x.ready>t || x.path[x.hop].from!=tile || static_cast<int>(x.path[x.hop].out)!=dir) continue;
                if (win==~U{0}) { win=id; continue; }
                const auto& w=pkts[win];
                bool better;
                if (c.topology.link_arbitration=="round_robin") {
                    const int a=(x.port-ptr[li]+5)%5, b=(w.port-ptr[li]+5)%5;
                    better=a!=b?a<b:std::make_tuple(x.ready,x.rank(),win)<std::make_tuple(w.ready,w.rank(),id);
                } else better=std::make_tuple(x.ready,x.port,x.rank(),win)<std::make_tuple(w.ready,w.port,w.rank(),id);
                if (better) win=id;
            }
            if (win==~U{0}) continue;
            auto& x=pkts[win];
            if (c.topology.link_arbitration=="round_robin") ptr[li]=(x.port+1)%5;
            const U occ=x.flits*c.topology.link_cycles;
            auto& s=r.links[li]; ++s.packets; s.flits+=x.flits; s.busy_cycles+=occ; s.wait_cycles+=t-x.ready;
            busy[li]=t+occ;
            if (x.hop+1<x.path.size()) { ++x.hop; x.ready=t+c.topology.link_cycles+c.topology.router_cycles; x.port=dir==1?2:dir==2?1:dir==3?4:3; }
            else { std::erase(waiting,win); agenda.insert({t+occ,{K::deliver,0,win}}); }
        }
    }
    edge::ClusterResult run() {
        for (U t=0;;++t) {
            if (t>2000000) throw std::runtime_error("oracle tick budget");
            for (auto it=agenda.find(t);it!=agenda.end() && it->first==t;it=agenda.find(t)) {
                // lower_bound re-queried each time: actions may add more actions at t.
                auto cur=agenda.lower_bound(t); if (cur==agenda.end() || cur->first!=t) break;
                const Act a=cur->second; agenda.erase(cur); act(a,t);
            }
            grants(t);
            bool busy_any=!agenda.empty() || !waiting.empty() || !mq.empty();
            for (auto& b:banks) busy_any=busy_any || !b.q.empty() || b.mem;
            if (!busy_any) break;
        }
        for (U p=0;p<cores.size();++p) {
            CacheState st(c.l1.geometry.sets);
            for (U s=0;s<st.size();++s) st[s]=order(l1[p][s]);
            r.l1_state.push_back(st);
        }
        for (U b=0;b<B;++b) {
            CacheState st(c.l2.geometry.sets);
            for (U s=0;s<st.size();++s) st[s]=order(l2[b][s]);
            r.l2_state.push_back(st);
        }
        return r;
    }
    static std::vector<U> order(const std::map<U,U>& set) {
        std::vector<std::pair<U,U>> v; for (auto [line,st]:set) v.push_back({st,line});
        std::sort(v.rbegin(),v.rend());
        std::vector<U> out; for (auto [st,line]:v) out.push_back(line); return out;
    }
};

U latency(const edge::ClusterResult& r,U core,U index) {
    for (const auto& x:r.reads) if (x.core==core && x.index==index) return x.done-x.issue;
    throw std::runtime_error("read not found");
}
}
int main() {
    try {
        // Address 192 = L2 line 3 -> bank 3 (diagonal from tile 0, 2 hops).
        // Reads: 192 (cold), 224 (other L1 line, same L2 line), 192 again.
        {
            auto c=base();
            c.cores={core("c0",0,3,192,32,2)};
            const auto r=edge::run_cluster(c);
            // Cold: L1 1 + REQ T0(2,1)=4 + L2 4 + MREQ 4 + D 46 + MRESP T0(2,5)=8
            //       + L2 fill 2 + RESP T0(2,3)=6 + L1 fill 1 = 76.
            check(latency(r,0,0)==76,"cold diagonal L2 miss = 76");
            check(r.reads[0].issue==10 && r.reads[0].done==86,"issue at start + compute");
            // L1 miss, L2 hit: 1 + 4 + 4 + 6 + 1 = 16.
            check(latency(r,0,1)==16,"diagonal L2 hit = 16");
            check(latency(r,0,2)==1,"L1 hit = lookup only");
            check(r.reads[1].issue==96 && r.reads[2].issue==122,"a_(k+1) = d_k + compute");
            const auto& b=r.banks[3];
            check(b.l2.references==2 && b.l2.hits==1 && b.l2.misses==1 && b.l2.fill_bytes==64,"bank 3 counters");
            check(b.busy_cycles==(4+4+46+8+2)+4,"bank busy: whole miss round trip, then one lookup");
            check(r.memory_requests==1 && r.memory_wire_bytes==64,"one line from memory");
            check(r.cores[0].l1.misses==2 && r.cores[0].l1.hits==1,"L1 counters");
            check(r.l2_state[3][0]==std::vector<U>{3},"L2 line 3 resident in bank 3 set 0");
            check(r.l1_state[0][2]==std::vector<U>{6} && r.l1_state[0][3]==std::vector<U>{7},"L1 lines 6 and 7");
        }
        // Local bank (core on tile 3, bank 3): no network for REQ/RESP.
        {
            auto c=base();
            c.cores={core("c3",3,2,192,0,1)};
            const auto r=edge::run_cluster(c);
            check(latency(r,0,0)==1+4+4+46+8+2+1,"local bank cold miss = 66");
            check(latency(r,0,1)==1,"then an L1 hit");
        }
        // Bank on the memory-controller tile: no network at all.
        {
            auto c=base();
            c.cores={core("c0",0,1,0,0,1)};
            check(latency(edge::run_cluster(c),0,0)==1+4+46+2+1,"bank 0 on MC tile: 54");
        }
        // Bank conflict: c1 (tile 1) and c2 (tile 2) read line 3 at the same time.
        // Both REQs (1 hop) arrive at bank 3 at t=13. Round-robin pointer 0 reaches
        // tile 1 first. c1: grant 13, lookup 17, MREQ 3->0 delivered 21, memory done 67,
        // MRESP delivered 75, fill 77, RESP 3->1 (1 hop, 3 flits) delivered 81, L1 fill 82.
        // c2 waits for the blocking bank until 77, then hits: lookup 81, RESP 85, fill 86.
        {
            auto c=base();
            c.cores={core("c1",1,1,192,0,1),core("c2",2,1,192,0,1)};
            const auto r=edge::run_cluster(c);
            check(latency(r,0,0)==72,"c1 wins round-robin and misses: 72");
            check(latency(r,1,0)==76,"c2 queues behind the miss, then hits: 76");
            check(r.banks[3].wait_cycles==77-13,"bank wait = 64 cycles");
            // Fixed priority also favours tile 1; FIFO ties break by source tile too.
            for (const char* p:{"fifo","fixed_priority"}) {
                c.l2.arbitration=p;
                const auto q=edge::run_cluster(c);
                check(latency(q,0,0)==72 && latency(q,1,0)==76,std::string("tie order under ")+p);
            }
        }

        // Random clusters against the tick oracle: every read's issue/done tick, all
        // per-core, per-bank, memory and per-link counters, and final L1/L2 states.
        {
            std::mt19937_64 rng(20261016);
            auto pick=[&](U lo,U hi){ return std::uniform_int_distribution<U>(lo,hi)(rng); };
            auto choose=[&](std::initializer_list<const char*> v){ return std::string(*(v.begin()+pick(0,v.size()-1))); };
            int cases=0, queued=0;
            for (int trial=0;trial<300;++trial) {
                edge::ClusterConfig c=base();
                const bool torus=pick(0,3)==0;
                const U k=torus?3:pick(2,3);
                c.topology={torus?"torus":"mesh",k,pick(0,k*k-1),pick(0,2),pick(1,2),pick(0,1)?U{16}:U{8},choose({"fifo","round_robin"}),"t"};
                const U l1line=pick(0,1)?U{16}:U{32};
                c.l1={{l1line,pick(1,4),pick(1,2)},pick(1,2),pick(0,2),"","t"};
                c.l2={{l1line*pick(1,2),pick(1,4),pick(1,2)},pick(1,4),pick(0,2),choose({"fifo","round_robin","fixed_priority"}),"t"};
                c.memory={pick(5,20),pick(0,1)?U{8}:U{16},pick(1,2),pick(1,4),pick(1,3),"t"};
                std::vector<U> tiles(k*k); for (U i=0;i<tiles.size();++i) tiles[i]=i;
                std::shuffle(tiles.begin(),tiles.end(),rng);
                const U n=pick(1,k*k);
                c.cores.clear();
                for (U i=0;i<n;++i)
                    c.cores.push_back({"c"+std::to_string(i),tiles[i],pick(0,20),pick(0,15),pick(1,12),
                                       pick(0,1023),pick(1,40),pick(0,256),pick(1,6)});
                const auto a=edge::run_cluster(c);
                const auto b=ClusterOracle(c).run();
                auto sorted=[](std::vector<edge::ReadRecord> v){ std::sort(v.begin(),v.end(),[](auto& x,auto& y){return std::tie(x.core,x.index)<std::tie(y.core,y.index);}); return v; };
                const std::string at=" (trial "+std::to_string(trial)+")";
                if (sorted(a.reads)!=sorted(b.reads)) {
                    // Diagnostic: the failing configuration and the first differing read.
                    std::cerr<<edge::describe_cluster_config(c);
                    const auto x=sorted(a.reads), y=sorted(b.reads);
                    for (std::size_t i=0;i<std::min(x.size(),y.size());++i) if (!(x[i]==y[i])) {
                        std::cerr<<"core "<<x[i].core<<" read "<<x[i].index<<": evaluator issue/done "<<x[i].issue<<'/'<<x[i].done
                                 <<", oracle "<<y[i].issue<<'/'<<y[i].done<<'\n';
                        break;
                    }
                }
                check(sorted(a.reads)==sorted(b.reads),"read issue/done ticks vs oracle"+at);
                check(a.cores==b.cores,"core counters vs oracle"+at);
                check(a.banks==b.banks,"bank counters vs oracle"+at);
                check(a.memory_requests==b.memory_requests && a.memory_wire_bytes==b.memory_wire_bytes &&
                      a.memory_wait_cycles==b.memory_wait_cycles,"memory counters vs oracle"+at);
                check(a.links==b.links,"link counters vs oracle"+at);
                check(a.l1_state==b.l1_state && a.l2_state==b.l2_state,"final cache states vs oracle"+at);
                U wait=0; for (auto& x:a.banks) wait+=x.wait_cycles; for (auto& x:a.links) wait+=x.wait_cycles;
                queued+=wait>0; ++cases;
            }
            check(queued*2>cases,"most random clusters exercise bank or link queueing");
            std::cout<<"cluster oracle sweep: "<<cases<<" random clusters, "<<queued<<" with queueing\n";
        }
        // The bench report prints the evaluator's counts unchanged. Same three reads as the
        // first golden: latencies 76, 16, 1 (mean 31), last completion 122 + 1 = 123.
        {
            auto c=base();
            c.cores={core("c0",0,3,192,32,2)};
            const auto r=edge::run_cluster(c);
            const auto text=edge::report_cluster(c,r);
            auto has=[&](const std::string& s){ check(text.find(s)!=std::string::npos,"report line: "+s); };
            has("makespan_cycles=123\n");
            has("\nc0,0,0,0,0,3,31,1,76,123,1,2,0,");          // hops_to_mc 0; 1 L1 hit, 2 misses.
            has("fairness_jain_throughput=1\n");              // One core: J = 1 exactly.
            const auto& b=r.banks[3];
            has("\n3,1,1,2,1,1,0,2,"+std::to_string(b.wait_cycles)+",68,");
            // XY 0->3 uses 0:+x and 1:+y; 3->0 uses 3:-x and 2:-y. Nothing else.
            has("links_used=4\n");
            has("\n2,0,0,0,+x,1,3,");                         // REQ, REQ, MRESP on 0:+x.
            has("bank3.l2.set0=3,\n");
            // The effective configuration in the report parses back to the input.
            const auto tail=text.substr(text.find("# Effective configuration"));
            check(edge::parse_cluster_config(tail.substr(tail.find('\n')+1))==c,"report config round trip");
        }
        // Budgets fail explicitly instead of truncating.
        {
            auto c=base();
            c.cores={core("c0",0,50,0,64,50)};
            c.stimulus.max_events=20;
            bool threw=false;
            try { edge::run_cluster(c); } catch (const std::length_error&) { threw=true; }
            check(threw,"event budget enforced");
        }
        std::cout<<"cluster golden tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
