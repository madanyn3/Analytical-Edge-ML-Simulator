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
}
