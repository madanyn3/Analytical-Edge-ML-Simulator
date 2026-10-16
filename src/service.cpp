#include "edge/service.hpp"
#include "edge/checked.hpp"
#include <algorithm>
#include <stdexcept>

namespace edge {
namespace {
using checked::add;
using checked::mul;
using U = std::uint64_t;
void validate(const ServiceConfig& c) {
    if (!c.first_beat_cycles || !c.beat_bytes || !c.beat_cycles || !c.issue_cycles ||
        !c.outstanding || c.outstanding > 1000000 || c.provenance.empty())
        throw std::invalid_argument("invalid memory service parameters/provenance");
}
struct Demand { U duration, interval, wire; };
Demand demand(const ServiceConfig& c, Request r) {
    if (!r.bytes) throw std::invalid_argument("zero-byte request");
    (void)add(r.address,r.bytes-1);
    // Unaligned requests consume the first and last partially used bus beats.
    const U beats=checked::ceil_div(add(r.address%c.beat_bytes,r.bytes),c.beat_bytes);
    return {add(c.first_beat_cycles,mul(beats-1,c.beat_cycles)),
            std::max(c.issue_cycles,mul(beats,c.beat_cycles)),mul(beats,c.beat_bytes)};
}
}
MemoryService::MemoryService(ServiceConfig c, ServiceState s):config_(std::move(c)),state_(std::move(s)) {
    validate(config_);
    if (state_.pending.size()>config_.outstanding || !std::is_sorted(state_.pending.begin(),state_.pending.end()))
        throw std::invalid_argument("invalid service initial state");
}
Completion MemoryService::submit(Request r) {
    if (r.arrival<state_.last_arrival) throw std::invalid_argument("arrivals must be nondecreasing");
    const auto d=demand(config_,r);
    auto next=state_; // Commit only after arithmetic and input checks succeed.
    U start=std::max(r.arrival,next.next_issue);
    auto release=[&] {
        next.pending.erase(next.pending.begin(),std::upper_bound(next.pending.begin(),next.pending.end(),start));
    };
    release();
    // With all credits occupied, admission waits for the earliest completion.
    // Completion at a tick precedes admission at that same tick.
    if (next.pending.size()==config_.outstanding) { start=next.pending.front(); release(); }
    const U done=add(start,d.duration);
    next.next_issue=add(start,d.interval);
    next.pending.insert(std::upper_bound(next.pending.begin(),next.pending.end(),done),done);
    next.last_arrival=r.arrival;
    state_=std::move(next);
    return {r.arrival,start,done,d.wire};
}
PeriodicResult periodic_memory(const ServiceConfig& c,Request first,U n,U spacing) {
    validate(c);
    const auto d=demand(c,first);
    PeriodicResult out;
    if (!n) return out;
    const U k=c.outstanding;
    const U j=std::max(spacing,d.interval);
    // s_i=max(a_i,s_(i-1)+I,s_(i-K)+D). For a_i=a0+i*P and constant
    // D,I its solution is a0+floor(i/K)*H+(i mod K)*J,
    // J=max(P,I), H=max(K*J,D). This is a project derivation, not a
    // transcription of a stochastic NoC equation. See the proof in the contract.
    const U h=std::max(mul(k,j),d.duration);
    auto start=[&](U i){return add(first.arrival,add(mul(i/k,h),mul(i%k,j)));};
    const U last=start(n-1);
    const U last_arrival=add(first.arrival,mul(n-1,spacing));
    out.requests=n;
    out.requested_bytes=mul(n,first.bytes);
    out.wire_bytes=mul(n,d.wire);
    out.first_done=add(first.arrival,d.duration);
    out.last_done=add(last,d.duration);
    out.min_latency=d.duration;
    out.max_latency=out.last_done-last_arrival;
    // Sum quotient/remainder runs without enumerating requests. Floating arithmetic
    // is used only for the reported mean; cycle counts and state remain integers.
    const U q=n/k, rem=n%k;
    const long double sum_q=static_cast<long double>(k)*q*(static_cast<long double>(q)-1)/2+
                           static_cast<long double>(rem)*q;
    const long double sum_r=static_cast<long double>(q)*k*(k-1)/2+
                           static_cast<long double>(rem)*(static_cast<long double>(rem)-1)/2;
    out.mean_latency=d.duration+(sum_q*(h-mul(k,spacing))+sum_r*(j-spacing))/n;
    out.final_state.next_issue=add(last,d.interval);
    out.final_state.last_arrival=last_arrival;
    // At most K transactions survive the last admission. Earlier completions
    // have already released their credits; preserve these for later composition.
    for (U i=n>k?n-k:0;i<n;++i) {
        const U done=add(start(i),d.duration);
        if (done>last) out.final_state.pending.push_back(done);
    }
    return out;
}
}
