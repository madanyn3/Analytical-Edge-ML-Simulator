#include "edge/network.hpp"
#include "edge/checked.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace edge::noc {
using checked::add;
using checked::mul;
Network::Network(const TopologyConfig& c)
    :topology_(c),router_(c.router_cycles),link_(c.link_cycles),round_robin_(c.link_arbitration=="round_robin") {
    if (!link_ || (c.link_arbitration!="fifo" && !round_robin_))
        throw std::invalid_argument("network needs link_cycles >= 1 and fifo/round_robin arbitration");
    links_.resize(topology_.tiles()*port_count);
    stats_.resize(links_.size());
}
void Network::inject(const Packet& p,U time) {
    if (time<now_) throw std::invalid_argument("injection precedes processed time");
    if (!p.flits) throw std::invalid_argument("packet needs >= 1 flit");
    (void)topology_.coord(p.source); (void)topology_.coord(p.target);
    if (flights_.contains(p.id) || std::any_of(pending_.begin(),pending_.end(),[&](const Delivery& d){return d.id==p.id;}))
        throw std::invalid_argument("duplicate in-flight packet id");
    (void)mul(p.flits,link_); // Occupancy F*t_l must be representable.
    if (p.source==p.target) { pending_.push_back({p.id,time}); return; } // BK2: local.
    auto path=topology_.route(p.source,p.target);
    // The head reaches the first output after the source router's t_r.
    links_[topology_.link_index(path.front())].queue.push_back({add(time,router_),Port::local,p.rank,p.id,0});
    flights_.emplace(p.id,InFlight{p,std::move(path)});
}
std::optional<U> Network::grant_time(const LinkState& l) const {
    if (l.queue.empty()) return std::nullopt;
    U earliest=l.queue.front().ready;
    for (const auto& w:l.queue) earliest=std::min(earliest,w.ready);
    return std::max(l.free,earliest);
}
std::optional<U> Network::next_time() const {
    std::optional<U> t;
    auto consider=[&](U v){ t=t?std::min(*t,v):v; };
    for (const auto& d:pending_) consider(d.time);
    for (const auto& l:links_) if (auto g=grant_time(l)) consider(*g);
    return t;
}
std::vector<Delivery> Network::deliveries(U t) {
    if (t<now_) throw std::invalid_argument("time moved backwards");
    std::vector<Delivery> out;
    for (const auto& d:pending_) {
        if (d.time<t) throw std::logic_error("a delivery event was skipped");
        if (d.time==t) out.push_back(d);
    }
    std::erase_if(pending_,[&](const Delivery& d){return d.time==t;});
    std::sort(out.begin(),out.end(),[](const Delivery& a,const Delivery& b){return a.id<b.id;});
    now_=t;
    return out;
}
void Network::grants(U t) {
    if (t<now_) throw std::invalid_argument("time moved backwards");
    now_=t;
    for (std::size_t index=0;index<links_.size();++index) {
        auto& link=links_[index];
        const auto g=grant_time(link);
        if (!g) continue;
        if (*g<t) throw std::logic_error("a link grant event was skipped");
        if (*g>t) continue;
        // Eligible: heads already waiting at t. Grant time is never later than the
        // earliest ready head, so at least one packet is eligible here.
        auto eligible=[&](const Waiting& w){return w.ready<=t;};
        auto fifo_less=[](const Waiting& a,const Waiting& b) {
            if (a.ready!=b.ready) return a.ready<b.ready;
            if (a.in!=b.in) return a.in<b.in;   // Fixed port order local,-X,+X,-Y,+Y.
            if (a.rank!=b.rank) return a.rank<b.rank;
            return a.id<b.id;
        };
        auto best=link.queue.end();
        if (!round_robin_) {
            for (auto it=link.queue.begin();it!=link.queue.end();++it)
                if (eligible(*it) && (best==link.queue.end() || fifo_less(*it,*best))) best=it;
        } else {
            // First input port at or after the pointer (cyclically) with an eligible
            // packet wins; FIFO within that port. Pointer moves past the winner's port.
            for (std::size_t k=0;k<port_count && best==link.queue.end();++k) {
                const auto port=static_cast<Port>((link.pointer+k)%port_count);
                for (auto it=link.queue.begin();it!=link.queue.end();++it)
                    if (eligible(*it) && it->in==port && (best==link.queue.end() || fifo_less(*it,*best))) best=it;
            }
            link.pointer=static_cast<std::uint8_t>((static_cast<std::size_t>(best->in)+1)%port_count);
        }
        const Waiting w=*best;
        link.queue.erase(best);
        auto& flight=flights_.at(w.id);
        const U occupancy=mul(flight.packet.flits,link_);
        link.free=add(t,occupancy);
        auto& s=stats_[index];
        s.packets=add(s.packets,1); s.flits=add(s.flits,flight.packet.flits);
        s.busy_cycles=add(s.busy_cycles,occupancy); s.wait_cycles=add(s.wait_cycles,t-w.ready);
        const auto& here=flight.path[w.hop];
        if (w.hop+1<flight.path.size()) {
            // Head crosses this link (t_l) then the next router (t_r) before contending.
            const auto& next=flight.path[w.hop+1];
            links_[topology_.link_index(next)].queue.push_back(
                {add(add(t,link_),router_),topology_.input_port(here),w.rank,w.id,w.hop+1});
        } else {
            // Whole packet has arrived once its last flit leaves the final link.
            pending_.push_back({w.id,add(t,occupancy)});
            flights_.erase(w.id);
        }
    }
}
namespace {
void put(std::string& k,U v) { k.append(reinterpret_cast<const char*>(&v),sizeof v); }
}
void Network::state_key(std::string& k,U T,const std::function<Describe(U)>& describe) const {
    using Entry=std::array<U,11>;
    std::vector<Entry> entries;
    for (std::size_t index=0;index<links_.size();++index) {
        const auto& l=links_[index];
        if (l.queue.empty() && l.free<=T && !l.pointer) continue; // Idle link: no entry.
        put(k,index); put(k,l.free>T?l.free-T:0); put(k,l.pointer); put(k,l.queue.size());
        entries.clear();
        for (const auto& w:l.queue) {
            const auto& f=flights_.at(w.id);
            const auto d=describe(w.id);
            entries.push_back({w.ready-T,static_cast<U>(w.in),w.rank,f.packet.source,f.packet.target,
                               f.packet.flits,static_cast<U>(w.hop),d[0],d[1],d[2],d[3]});
        }
        std::sort(entries.begin(),entries.end());
        for (const auto& e:entries) for (U v:e) put(k,v);
    }
    put(k,~U{0}); // Separator: no link index takes this value.
    std::vector<std::array<U,5>> deliveries;
    for (const auto& d:pending_) {
        const auto x=describe(d.id);
        deliveries.push_back({d.time-T,x[0],x[1],x[2],x[3]});
    }
    std::sort(deliveries.begin(),deliveries.end());
    put(k,deliveries.size());
    for (const auto& e:deliveries) for (U v:e) put(k,v);
}
void Network::skip(U shift,const std::vector<LinkStats>& before,U m) {
    if (before.size()!=stats_.size()) throw std::invalid_argument("link stats size mismatch");
    now_=add(now_,shift);
    for (auto& l:links_) {
        l.free=add(l.free,shift);
        for (auto& w:l.queue) w.ready=add(w.ready,shift);
    }
    for (auto& d:pending_) d.time=add(d.time,shift);
    for (std::size_t i=0;i<stats_.size();++i) {
        auto& s=stats_[i];
        const auto& b=before[i];
        s.packets=add(s.packets,mul(s.packets-b.packets,m));
        s.flits=add(s.flits,mul(s.flits-b.flits,m));
        s.busy_cycles=add(s.busy_cycles,mul(s.busy_cycles-b.busy_cycles,m));
        s.wait_cycles=add(s.wait_cycles,mul(s.wait_cycles-b.wait_cycles,m));
    }
}
bool Network::idle() const { return pending_.empty() && flights_.empty(); }
}
