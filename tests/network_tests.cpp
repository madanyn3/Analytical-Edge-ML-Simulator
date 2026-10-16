// Link-contention checks. The oracle steps every tick and scans every link in REVERSE
// index order, written from docs/cluster-design.md §4.2–4.3; it shares no arbitration
// or timing code with edge::noc::Network (routing is reused: noc_tests verifies it).
#include "edge/network.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

namespace {
using U=std::uint64_t;
using namespace edge::noc;
void check(bool ok,const std::string& what) { if (!ok) throw std::runtime_error(what); }
struct Injection { Packet packet; U time; };
struct Outcome {
    std::map<U,U> delivered; // id -> delivery tick
    std::map<std::pair<U,int>,LinkStats> links;
};
edge::TopologyConfig cfg(const char* kind,U k,U tr,U tl,const char* arb) { return {kind,k,0,tr,tl,16,arb,"test"}; }

// Event-driven model under test, driven in the design's phase order per event time:
// deliveries (phase 1), injections (phase 2), link grants (phase 3).
Outcome evaluate(const edge::TopologyConfig& c,std::vector<Injection> in) {
    std::stable_sort(in.begin(),in.end(),[](auto& a,auto& b){return a.time<b.time;});
    Network net(c);
    Outcome out;
    std::size_t next=0;
    for (;;) {
        auto t=net.next_time();
        if (next<in.size()) t=t?std::min(*t,in[next].time):in[next].time;
        if (!t) break;
        for (auto d:net.deliveries(*t)) out.delivered[d.id]=d.time;
        while (next<in.size() && in[next].time==*t) { net.inject(in[next].packet,*t); ++next; }
        net.grants(*t);
    }
    check(net.idle(),"network drained");
    const auto& topo=net.topology();
    for (U tile=0;tile<topo.tiles();++tile) for (int p=1;p<5;++p)
        if (topo.has_link(tile,static_cast<Port>(p))) out.links[{tile,p}]=net.link_stats({tile,static_cast<Port>(p)});
    return out;
}

// Independent tick oracle.
Outcome oracle(const edge::TopologyConfig& c,const std::vector<Injection>& in) {
    const Topology topo(c);
    const bool rr=c.link_arbitration=="round_robin";
    struct P { Packet p; U inject; std::vector<Link> path; std::size_t hop{}; U ready{}; int port{}; bool moving{}; };
    std::vector<P> ps;
    for (auto& i:in) ps.push_back({i.packet,i.time,topo.route(i.packet.source,i.packet.target)});
    std::map<std::pair<U,int>,U> busy_until;
    std::map<std::pair<U,int>,int> pointer;
    Outcome out;
    for (U tile=0;tile<topo.tiles();++tile) for (int p=1;p<5;++p)
        if (topo.has_link(tile,static_cast<Port>(p))) out.links[{tile,p}]={};
    std::size_t done=0;
    for (U t=0;done<ps.size();++t) {
        check(t<100000,"oracle tick budget");
        for (auto& x:ps) if (x.inject==t) {
            if (x.path.empty()) { out.delivered[x.p.id]=t; ++done; }
            else { x.moving=true; x.hop=0; x.ready=t+c.router_cycles; x.port=0; }
        }
        // Reverse scan: grants at one tick must not depend on link visiting order.
        for (U tile=topo.tiles();tile-->0;) for (int dir=4;dir>=1;--dir) {
            const std::pair<U,int> key{tile,dir};
            if (!out.links.contains(key) || busy_until[key]>t) continue;
            std::vector<P*> waiting;
            for (auto& x:ps) if (x.moving && x.ready<=t && x.path[x.hop].from==tile &&
                                 static_cast<int>(x.path[x.hop].out)==dir) waiting.push_back(&x);
            if (waiting.empty()) continue;
            auto older=[](P* a,P* b){ return std::tuple(a->ready,a->port,a->p.id)<std::tuple(b->ready,b->port,b->p.id); };
            P* win=nullptr;
            if (!rr) win=*std::min_element(waiting.begin(),waiting.end(),older);
            else for (int k=0;k<5 && !win;++k) {
                const int port=(pointer[key]+k)%5;
                for (P* x:waiting) if (x->port==port && (!win || older(x,win))) win=x;
            }
            if (rr) pointer[key]=(win->port+1)%5;
            const U occ=win->p.flits*c.link_cycles;
            auto& s=out.links[key];
            ++s.packets; s.flits+=win->p.flits; s.busy_cycles+=occ; s.wait_cycles+=t-win->ready;
            busy_until[key]=t+occ;
            if (win->hop+1<win->path.size()) {
                // Entering the next router through the port facing back along this link.
                const int back=dir==1?2:dir==2?1:dir==3?4:3;
                ++win->hop; win->ready=t+c.link_cycles+c.router_cycles; win->port=back;
            } else { win->moving=false; out.delivered[win->p.id]=t+occ; ++done; }
        }
    }
    return out;
}
}
int main() {
    try {
        // Golden contention example (explained in the design walkthrough), 2x2 mesh,
        // t_r = t_l = 1. C (1 flit, tile 2 -> 0, t=0) uses link 2->0 first. Then A
        // (3 flits, 3 -> 0 via 2, t=0) and B (3 flits, 2 -> 0, t=2) both become ready
        // for link 2->0 at t=3: A on input port +X, B on port local.
        const std::vector<Injection> golden{{{1,3,0,3},0},{{2,2,0,3},2},{{3,2,0,1},0}};
        auto fifo=evaluate(cfg("mesh",2,1,1,"fifo"),golden);
        check(fifo.delivered[3]==2,"C: ready 1, granted 1, delivered 1+1 = T0(1,1) = 2");
        check(fifo.delivered[2]==6 && fifo.delivered[1]==9,"FIFO tie: local (B) beats +X (A); A waits 3 cycles");
        // Round-robin: C's grant from port local moved the pointer to -X, so the search
        // reaches +X (A) before wrapping to local (B): the winner flips.
        auto rr=evaluate(cfg("mesh",2,1,1,"round_robin"),golden);
        check(rr.delivered[1]==6 && rr.delivered[2]==9,"RR: pointer after local grant favours +X (A)");
        const auto& l20=fifo.links[{2,static_cast<int>(Port::minus_y)}];
        check(l20.packets==3 && l20.flits==7 && l20.busy_cycles==7 && l20.wait_cycles==3,"link 2->0 counters");
        check(fifo.delivered==oracle(cfg("mesh",2,1,1,"fifo"),golden).delivered,"golden FIFO vs oracle");
        check(rr.delivered==oracle(cfg("mesh",2,1,1,"round_robin"),golden).delivered,"golden RR vs oracle");

        // An isolated packet always takes exactly the zero-load time T0(H,F).
        for (const char* kind:{"mesh","torus"}) for (U k=(kind[0]=='t'?3:2);k<=4;++k) {
            const Topology topo(cfg(kind,k,1,1,"fifo"));
            for (U s=0;s<k*k;++s) for (U d=0;d<k*k;++d) for (U f:{1,3,5}) for (U tr:{0,2}) for (U tl:{1,3}) {
                auto r=evaluate(cfg(kind,k,tr,tl,"fifo"),{{{7,s,d,f},11}});
                check(r.delivered[7]==11+zero_load(topo.hops(s,d),f,tr,tl),"isolated packet = T0");
            }
        }

        // Random contention against the tick oracle: delivery times and link counters.
        std::mt19937_64 rng(20261016);
        auto pick=[&](U lo,U hi){ return std::uniform_int_distribution<U>(lo,hi)(rng); };
        int cases=0, contended=0;
        for (int trial=0;trial<600;++trial) {
            const bool torus=pick(0,2)==0;
            const U k=torus?pick(3,4):pick(2,4);
            const auto c=cfg(torus?"torus":"mesh",k,pick(0,2),pick(1,2),pick(0,1)?"fifo":"round_robin");
            std::vector<Injection> in;
            const U n=pick(1,40);
            for (U i=0;i<n;++i) {
                // Hotspot bias: half the packets target tile 0 to force contention.
                const U target=pick(0,1)?0:pick(0,k*k-1);
                in.push_back({{100+i,pick(0,k*k-1),target,pick(1,5)},pick(0,30)});
            }
            const auto a=evaluate(c,in), b=oracle(c,in);
            check(a.delivered==b.delivered,"delivery times vs oracle (trial "+std::to_string(trial)+")");
            check(a.links==b.links,"link counters vs oracle (trial "+std::to_string(trial)+")");
            U wait=0; for (const auto& [key,st]:a.links) wait+=st.wait_cycles;
            contended+=wait>0;
            ++cases;
        }
        // The comparison is only meaningful if packets actually queued behind each other.
        check(contended*2>cases,"most random trials exercise link contention");
        // Misuse fails explicitly rather than corrupting timing.
        bool threw=false;
        try { Network n(cfg("mesh",2,1,1,"fifo")); n.inject({1,0,3,1},5); n.grants(9); } catch (const std::logic_error&) { threw=true; }
        check(threw,"skipping past a grant event is rejected");
        threw=false;
        try { Network n(cfg("mesh",2,1,1,"fifo")); n.inject({1,0,3,1},5); n.inject({1,1,3,1},5); } catch (const std::invalid_argument&) { threw=true; }
        check(threw,"duplicate packet id rejected");
        std::cout<<"network tests passed ("<<cases<<" random cases, "<<contended<<" with link waiting)\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
