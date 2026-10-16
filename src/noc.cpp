#include "edge/noc.hpp"
#include "edge/checked.hpp"
#include <stdexcept>

namespace edge::noc {
Port opposite(Port p) {
    switch (p) {
    case Port::minus_x: return Port::plus_x;
    case Port::plus_x: return Port::minus_x;
    case Port::minus_y: return Port::plus_y;
    case Port::plus_y: return Port::minus_y;
    case Port::local: break;
    }
    return Port::local;
}
Topology::Topology(const TopologyConfig& c):k_(c.k),torus_(c.kind=="torus") {
    // Same domain as validate_cluster_config; repeated so the class is safe on its own.
    if (k_<2 || k_>8 || (c.kind!="mesh" && c.kind!="torus") || (torus_ && k_<3))
        throw std::invalid_argument("unsupported topology");
}
Coord Topology::coord(U t) const {
    if (t>=tiles()) throw std::out_of_range("tile outside grid");
    return {t%k_,t/k_};
}
U Topology::tile(Coord c) const {
    if (c.x>=k_ || c.y>=k_) throw std::out_of_range("coordinate outside grid");
    return c.y*k_+c.x;
}
bool Topology::has_link(U t,Port out) const {
    const auto c=coord(t);
    if (out==Port::local) return false;
    if (torus_) return true; // Every router has all four neighbours (k >= 3).
    switch (out) {
    case Port::minus_x: return c.x>0;
    case Port::plus_x: return c.x+1<k_;
    case Port::minus_y: return c.y>0;
    case Port::plus_y: return c.y+1<k_;
    case Port::local: break;
    }
    return false;
}
U Topology::neighbor(U t,Port out) const {
    if (!has_link(t,out)) throw std::invalid_argument("no link in that direction");
    auto c=coord(t);
    // Unsigned wraparound arithmetic: (x + k - 1) mod k steps backwards on the ring.
    switch (out) {
    case Port::minus_x: c.x=(c.x+k_-1)%k_; break;
    case Port::plus_x: c.x=(c.x+1)%k_; break;
    case Port::minus_y: c.y=(c.y+k_-1)%k_; break;
    case Port::plus_y: c.y=(c.y+1)%k_; break;
    case Port::local: break;
    }
    return tile(c);
}
std::pair<int,U> Topology::axis_move(U from,U to) const {
    if (from==to) return {0,0};
    if (!torus_) return from<to?std::pair<int,U>{+1,to-from}:std::pair<int,U>{-1,from-to};
    const U forward=(to+k_-from)%k_, backward=k_-forward;
    // Shorter ring direction; the k/2 tie goes to the positive direction (design §4.1).
    return forward<=backward?std::pair<int,U>{+1,forward}:std::pair<int,U>{-1,backward};
}
std::vector<Link> Topology::route(U s,U t) const {
    const auto a=coord(s), b=coord(t);
    std::vector<Link> path;
    U at=s;
    const auto [dx,nx]=axis_move(a.x,b.x);
    for (U i=0;i<nx;++i) {
        const Port p=dx>0?Port::plus_x:Port::minus_x;
        path.push_back({at,p}); at=neighbor(at,p);
    }
    const auto [dy,ny]=axis_move(a.y,b.y);
    for (U i=0;i<ny;++i) {
        const Port p=dy>0?Port::plus_y:Port::minus_y;
        path.push_back({at,p}); at=neighbor(at,p);
    }
    return path;
}
U Topology::hops(U s,U t) const {
    const auto a=coord(s), b=coord(t);
    return axis_move(a.x,b.x).second+axis_move(a.y,b.y).second;
}
U flits(U payload,U flit_bytes) {
    return checked::add(1,checked::ceil_div(payload,flit_bytes));
}
U zero_load(U h,U f,U tr,U tl) {
    if (!h) return 0;
    if (!f || !tl) throw std::invalid_argument("packets need >= 1 flit and link_cycles >= 1");
    // Head pays router + link at every hop; the remaining F-1 flits stream behind it.
    return checked::add(checked::mul(h,checked::add(tr,tl)),checked::mul(f-1,tl));
}
}
