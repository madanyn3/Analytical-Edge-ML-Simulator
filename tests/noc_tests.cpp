// Network geometry checks. The oracle builds its own adjacency from coordinate
// arithmetic and finds shortest paths by BFS; it never calls route() or hops().
#include "edge/noc.hpp"
#include <deque>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using U=std::uint64_t;
using edge::noc::Port;
void check(bool ok,const std::string& what) { if (!ok) throw std::runtime_error(what); }
edge::TopologyConfig config(const char* kind,U k) { return {kind,k,0,1,1,16,"fifo","test"}; }
// Independent neighbour set: mesh = 4-neighbourhood without wrap, torus = with wrap.
std::set<U> neighbours(U t,U k,bool torus) {
    const long x=static_cast<long>(t%k), y=static_cast<long>(t/k), K=static_cast<long>(k);
    std::set<U> out;
    for (auto [dx,dy]:{std::pair{1L,0L},{-1L,0L},{0L,1L},{0L,-1L}}) {
        long nx=x+dx, ny=y+dy;
        if (torus) { nx=(nx+K)%K; ny=(ny+K)%K; }
        else if (nx<0 || ny<0 || nx>=K || ny>=K) continue;
        out.insert(static_cast<U>(ny*K+nx));
    }
    return out;
}
U bfs(U s,U t,U k,bool torus) {
    std::vector<U> dist(k*k,~U{0});
    std::deque<U> q{s}; dist[s]=0;
    while (!q.empty()) {
        const U u=q.front(); q.pop_front();
        for (U v:neighbours(u,k,torus)) if (dist[v]==~U{0}) { dist[v]=dist[u]+1; q.push_back(v); }
    }
    return dist[t];
}
}
int main() {
    try {
        // Every route on every supported grid: contiguous neighbour links, ends at the
        // target, all X moves before Y moves, and as short as BFS says is possible.
        for (const char* kind:{"mesh","torus"}) for (U k=(kind[0]=='t'?3:2);k<=8;++k) {
            const bool torus=kind[0]=='t';
            const edge::noc::Topology topo(config(kind,k));
            U total=0;
            for (U s=0;s<k*k;++s) for (U t=0;t<k*k;++t) {
                const auto path=topo.route(s,t);
                U at=s; bool seen_y=false;
                for (const auto& l:path) {
                    check(l.from==at,"route is contiguous");
                    const bool is_y=l.out==Port::minus_y || l.out==Port::plus_y;
                    check(!(seen_y && !is_y),"XY order: no X move after a Y move");
                    seen_y=seen_y||is_y;
                    const U next=topo.neighbor(at,l.out);
                    check(neighbours(at,k,torus).contains(next),"link joins oracle neighbours");
                    at=next;
                }
                check(at==t,"route ends at target");
                check(path.size()==topo.hops(s,t),"hops() equals route length");
                check(path.size()==bfs(s,t,k,torus),"route is a shortest path");
                total+=path.size();
            }
            // Mean hops over uniform (source, destination), self included (design §6 N1):
            // mesh 2(k^2-1)/(3k); torus k/2 for even k, (k^2-1)/(2k) for odd k.
            // Compare total*den == num*pairs exactly in integers.
            const U pairs=k*k*k*k;
            U num,den;
            if (!torus) { num=2*(k*k-1); den=3*k; }
            else if (k%2==0) { num=k; den=2; }
            else { num=k*k-1; den=2*k; }
            check(total*den==num*pairs,std::string("mean-hop formula ")+kind+" k="+std::to_string(k));
        }
        // Golden routes on the design document's 2x2 mesh.
        const edge::noc::Topology m2(config("mesh",2));
        auto r=m2.route(0,3);
        check(r.size()==2 && r[0]==edge::noc::Link{0,Port::plus_x} && r[1]==edge::noc::Link{1,Port::plus_y},"0->3 is +X then +Y");
        r=m2.route(3,0);
        check(r[0]==edge::noc::Link{3,Port::minus_x} && r[1]==edge::noc::Link{2,Port::minus_y},"3->0 is -X then -Y");
        check(m2.input_port(r[0])==Port::plus_x,"packet from tile 3 enters router 2 on its +X port");
        check(m2.route(2,2).empty() && m2.hops(1,1)==0,"local traffic has no hops");
        check(!m2.has_link(0,Port::minus_x) && !m2.has_link(0,Port::local),"mesh edge and local are not links");
        // Torus wraparound and the k/2 tie rule.
        const edge::noc::Topology t3(config("torus",3)), t4(config("torus",4));
        r=t3.route(0,2);
        check(r.size()==1 && r[0]==edge::noc::Link{0,Port::minus_x},"3x3 torus: 0->2 wraps one hop -X");
        r=t4.route(0,2);
        check(r.size()==2 && r[0].out==Port::plus_x && r[1].out==Port::plus_x,"4x4 torus tie at k/2 goes +X");
        r=t4.route(2,0);
        check(r.size()==2 && r[0].out==Port::plus_x,"tie rule applies in both directions");
        bool threw=false;
        try { edge::noc::Topology bad(config("torus",2)); } catch (const std::exception&) { threw=true; }
        check(threw,"torus k=2 rejected by Topology too");

        // Packet sizes and zero-load latency (design §7.2 golden numbers, t_r=t_l=1).
        using edge::noc::flits; using edge::noc::zero_load;
        check(flits(0,16)==1 && flits(32,16)==3 && flits(64,16)==5 && flits(33,16)==4,"flit counts");
        check(zero_load(2,1,1,1)==4,"REQ over 2 hops");
        check(zero_load(2,3,1,1)==6,"RESP (3 flits) over 2 hops");
        check(zero_load(1,3,1,1)==4 && zero_load(0,5,1,1)==0,"1 hop and local");
        check(zero_load(2,5,1,1)==8,"MRESP (5 flits) over 2 hops");
        check(1+zero_load(2,1,1,1)+4+zero_load(2,3,1,1)+1==16,"zero-load diagonal L2 hit = 16 cycles");

        // Bank mapping golden example: 0x1240 -> line 73, bank 1, set 2, tag 1.
        const edge::noc::BankMap map{64,4,16};
        check(map.line(0x1240)==73 && map.bank(0x1240)==1 && map.set(0x1240)==2 && map.tag(0x1240)==1,"bank map");
        // Bank and set fields do not overlap: the B*S consecutive lines cover every
        // (bank, set) pair exactly once.
        std::set<std::pair<U,U>> cells;
        for (U line=0;line<64;++line) cells.insert({map.bank(line*64),map.set(line*64)});
        check(cells.size()==64,"every (bank,set) reachable");
        // Strided resonance: line stride sigma touches B/gcd(sigma,B) banks.
        for (U banks:{4,9,16}) for (U sigma=1;sigma<=40;++sigma) {
            const edge::noc::BankMap b{64,banks,16};
            std::set<U> touched;
            for (U i=0;i<4*banks;++i) touched.insert(b.bank(i*sigma*64));
            check(touched.size()==banks/std::gcd(sigma,banks),"bank resonance formula");
        }
        std::cout<<"noc tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
