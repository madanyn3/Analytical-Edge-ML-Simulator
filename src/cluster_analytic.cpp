#include "edge/cluster_analytic.hpp"
#include "edge/checked.hpp"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace edge::analytic {
namespace {
using checked::add;
using checked::mul;
void check_coord(const TopologyConfig& t,U a) {
    if (a>=t.k) throw std::invalid_argument("coordinate outside the grid");
}
}
U axis_distance(const TopologyConfig& t,U a,U b) {
    check_coord(t,a); check_coord(t,b);
    const U d=a>b?a-b:b-a;
    return t.kind=="torus"?std::min(d,t.k-d):d;
}
U hops(const TopologyConfig& t,U from,U to) {
    if (from>=t.k*t.k || to>=t.k*t.k) throw std::invalid_argument("tile outside the grid");
    return axis_distance(t,from%t.k,to%t.k)+axis_distance(t,from/t.k,to/t.k);
}
U axis_distance_sum(const TopologyConfig& t,U a) {
    check_coord(t,a);
    if (t.kind=="torus") return t.k*t.k/4;
    // Left: 1 + 2 + ... + a. Right: 1 + 2 + ... + (k-1-a).
    return a*(a+1)/2+(t.k-1-a)*(t.k-a)/2;
}
U hop_sum_from(const TopologyConfig& t,U tile) {
    if (tile>=t.k*t.k) throw std::invalid_argument("tile outside the grid");
    return t.k*(axis_distance_sum(t,tile%t.k)+axis_distance_sum(t,tile/t.k));
}
U packet_time(const TopologyConfig& t,U h,U payload) {
    if (!h) return 0;
    const U flits=add(1,checked::ceil_div(payload,t.flit_bytes));
    return add(mul(h,add(t.router_cycles,t.link_cycles)),mul(flits-1,t.link_cycles));
}
U memory_line_cycles(const ClusterConfig& c) {
    const U beats=checked::ceil_div(c.l2.geometry.line_bytes,c.memory.beat_bytes);
    return add(c.memory.first_beat_cycles,mul(beats-1,c.memory.beat_cycles));
}
ZeroLoadRead zero_load_read(const ClusterConfig& c,U core_tile,U bank) {
    const auto& t=c.topology;
    const U to_bank=hops(t,core_tile,bank), to_mc=hops(t,bank,t.mc_tile);
    ZeroLoadRead r{};
    r.l1_hit=c.l1.lookup_cycles;
    r.l2_hit=add(add(add(add(r.l1_hit,packet_time(t,to_bank,0)),c.l2.lookup_cycles),
                     packet_time(t,to_bank,c.l1.geometry.line_bytes)),c.l1.fill_cycles);
    r.l2_miss=add(add(add(add(r.l2_hit,packet_time(t,to_mc,0)),memory_line_cycles(c)),
                      packet_time(t,to_mc,c.l2.geometry.line_bytes)),c.l2.fill_cycles);
    return r;
}
U l2_hit_sum_over_banks(const ClusterConfig& c,U core_tile) {
    const auto& t=c.topology;
    const U banks=t.k*t.k;
    const U f_l1=add(1,checked::ceil_div(c.l1.geometry.line_bytes,t.flit_bytes));
    const U fixed=mul(banks,add(add(c.l1.lookup_cycles,c.l2.lookup_cycles),c.l1.fill_cycles));
    const U per_hop=mul(2,add(t.router_cycles,t.link_cycles));
    return add(add(fixed,mul(per_hop,hop_sum_from(t,core_tile))),mul(mul(banks-1,f_l1-1),t.link_cycles));
}
std::vector<U> k3a_admissions(U P,U Z,U D,U I,U K,U count) {
    if (!P || !I || !K) throw std::invalid_argument("K3a needs P, I, K >= 1");
    std::vector<U> s;
    s.reserve(count);
    for (U n=0;n<count;++n) {
        U t=n<P?Z:add(add(s[n-P],D),Z);           // Source's previous completion + Z.
        if (n>=1) t=std::max(t,add(s[n-1],I));      // Issue spacing.
        if (n>=K) t=std::max(t,add(s[n-K],D));      // Credit freed by request n-K.
        s.push_back(t);
    }
    return s;
}
Rational k3a_cycle_time(U P,U Z,U D,U I,U K) {
    if (!P || !I || !K) throw std::invalid_argument("K3a needs P, I, K >= 1");
    Rational best{I,1};
    // a/b > c/d  <=>  a*d > c*b for positive denominators.
    for (const Rational r:{Rational{D,K},Rational{add(Z,D),P}})
        if (mul(r.num,best.den)>mul(best.num,r.den)) best=r;
    const U g=std::gcd(best.num,best.den);
    return {best.num/g,best.den/g};
}
std::vector<U> xy_links(const TopologyConfig& t,U from,U to) {
    if (from>=t.k*t.k || to>=t.k*t.k) throw std::invalid_argument("tile outside the grid");
    std::vector<U> out;
    U x=from%t.k,y=from/t.k;
    // One dimension: step toward `target`. Mesh: the only direction. Torus: the shorter
    // ring direction, + on an exact tie (distance k/2).
    auto walk=[&](U& coord,U target,U minus_port,U plus_port) {
        if (coord==target) return;
        const U forward=(target+t.k-coord)%t.k;     // Steps going +.
        const bool plus=t.kind=="torus"?forward<=t.k-forward:target>coord;
        const U steps=plus?(t.kind=="torus"?forward:target-coord):(t.kind=="torus"?t.k-forward:coord-target);
        for (U i=0;i<steps;++i) {
            out.push_back((y*t.k+x)*5+(plus?plus_port:minus_port));
            coord=plus?(coord+1)%t.k:(coord+t.k-1)%t.k;
        }
    };
    walk(x,to%t.k,1,2);
    walk(y,to/t.k,3,4);
    return out;
}
Demand resource_demand(const ClusterConfig& c,const std::vector<ClassifiedRead>& reads) {
    const auto& t=c.topology;
    const U B=t.k*t.k;
    Demand d;
    d.link_packets.assign(B*5,0); d.link_flits.assign(B*5,0);
    d.bank_references.assign(B,0); d.bank_misses.assign(B,0); d.bank_min_busy.assign(B,0);
    d.core_l1_hits.assign(c.cores.size(),0); d.core_l1_misses.assign(c.cores.size(),0);
    d.core_min_finish.resize(c.cores.size());
    for (std::size_t p=0;p<c.cores.size();++p) d.core_min_finish[p]=c.cores[p].start_cycles;
    auto packet=[&](U from,U to,U payload) {
        const U flits=add(1,checked::ceil_div(payload,t.flit_bytes));
        for (U l:xy_links(t,from,to)) { d.link_packets[l]=add(d.link_packets[l],1); d.link_flits[l]=add(d.link_flits[l],flits); }
    };
    const U D=memory_line_cycles(c);
    for (const auto& r:reads) {
        if (r.core>=c.cores.size() || r.bank>=B) throw std::invalid_argument("classified read out of range");
        const U tile=c.cores[r.core].tile;
        const auto z=zero_load_read(c,tile,r.bank);
        U latency=z.l1_hit;
        if (r.kind==ReadClass::l1_hit) d.core_l1_hits[r.core]=add(d.core_l1_hits[r.core],1);
        else {
            d.core_l1_misses[r.core]=add(d.core_l1_misses[r.core],1);
            d.bank_references[r.bank]=add(d.bank_references[r.bank],1);
            packet(tile,r.bank,0);
            packet(r.bank,tile,c.l1.geometry.line_bytes);
            U busy=c.l2.lookup_cycles;
            latency=z.l2_hit;
            if (r.kind==ReadClass::l2_miss) {
                d.bank_misses[r.bank]=add(d.bank_misses[r.bank],1);
                d.memory_requests=add(d.memory_requests,1);
                packet(r.bank,t.mc_tile,0);
                packet(t.mc_tile,r.bank,c.l2.geometry.line_bytes);
                const U to_mc=hops(t,r.bank,t.mc_tile);
                busy=add(add(add(add(busy,packet_time(t,to_mc,0)),D),packet_time(t,to_mc,c.l2.geometry.line_bytes)),c.l2.fill_cycles);
                latency=z.l2_miss;
            }
            d.bank_min_busy[r.bank]=add(d.bank_min_busy[r.bank],busy);
        }
        d.core_min_finish[r.core]=add(d.core_min_finish[r.core],add(c.cores[r.core].compute_cycles,latency));
    }
    return d;
}
MakespanBound makespan_lower_bound(const ClusterConfig& c,const Demand& d) {
    MakespanBound b{};
    for (U v:d.core_min_finish) b.core=std::max(b.core,v);
    for (U f:d.link_flits) b.link=std::max(b.link,mul(f,c.topology.link_cycles));
    for (U v:d.bank_min_busy) b.bank=std::max(b.bank,v);
    if (d.memory_requests) {
        const U beats=checked::ceil_div(c.l2.geometry.line_bytes,c.memory.beat_bytes);
        const U I=std::max(c.memory.issue_cycles,mul(beats,c.memory.beat_cycles));
        const U D=memory_line_cycles(c);
        const U N=d.memory_requests;
        b.memory=std::max(add(mul(N-1,I),D),mul(checked::ceil_div(N,c.memory.outstanding),D));
    }
    b.value=std::max({b.core,b.link,b.bank,b.memory});
    return b;
}
U mesh_all_to_all_max_channel_load(U k) { return mul(k,k*k/4); }
}
