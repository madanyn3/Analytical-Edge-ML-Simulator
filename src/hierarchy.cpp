#include "edge/hierarchy.hpp"
#include "edge/checked.hpp"
#include <algorithm>
#include <stdexcept>

namespace edge {
namespace {
using U=std::uint64_t;
using checked::add;
using checked::mul;
}
Hierarchy::Hierarchy(ModelConfig c):config_(std::move(c)),memory_(config_.memory) {
    validate_config(config_);
    stats_.levels.resize(config_.caches.size());
    for (const auto& cache:config_.caches) {
        auto s=cache.initial;
        if (s.empty()) s.resize(cache.geometry.sets);
        state_.push_back(std::move(s));
    }
}
void Hierarchy::invalidate_above(std::size_t level,U line) {
    // With nested line sizes, an upper line belongs wholly to one lower line.
    // Inclusive lower eviction invalidates ALL matching upper copies. These are
    // invalidations, not LRU replacement evictions; the two counters stay separate.
    const U lower_bytes=config_.caches[level].geometry.line_bytes;
    for (std::size_t i=0;i<level;++i) {
        const U upper_bytes=config_.caches[i].geometry.line_bytes;
        for (auto& set:state_[i]) {
            const auto old=set.size();
            std::erase_if(set,[&](U upper){return mul(upper,upper_bytes)/lower_bytes==line;});
            stats_.levels[i].invalidations=add(stats_.levels[i].invalidations,old-set.size());
        }
    }
}
U Hierarchy::access(std::size_t level,U address,U bytes,U arrival) {
    if (level==config_.caches.size()) {
        if (++transactions_>config_.stimulus.max_transactions) throw std::length_error("transaction expansion budget exceeded");
        const auto result=memory_.submit({arrival,address,bytes});
        stats_.memory_requests=add(stats_.memory_requests,1);
        stats_.memory_requested_bytes=add(stats_.memory_requested_bytes,bytes);
        stats_.memory_wire_bytes=add(stats_.memory_wire_bytes,result.wire_bytes);
        return result.done;
    }
    const auto& c=config_.caches[level];
    auto& counts=stats_.levels[level];
    const U last=add(address,bytes-1)/c.geometry.line_bytes;
    const U first=address/c.geometry.line_bytes;
    const U references=last-first+1;
    if (references>config_.stimulus.max_transactions-transactions_)
        throw std::length_error("line expansion budget exceeded");
    U done=arrival;
    for (U line=first;;++line) {
        if (++transactions_>config_.stimulus.max_transactions) throw std::length_error("transaction expansion budget exceeded");
        counts.references=add(counts.references,1);
        done=add(done,c.lookup_cycles);
        auto& set=state_[level][line%c.geometry.sets];
        auto it=std::find(set.begin(),set.end(),line);
        if (it!=set.end()) {
            counts.hits=add(counts.hits,1);
            set.erase(it);
        } else {
            counts.misses=add(counts.misses,1);
            // Full-line demand allocation. Waiting for the complete lower response
            // makes this a blocking cache: no early restart, MSHR merge or prefetch.
            const U base=mul(line,c.geometry.line_bytes);
            (void)add(base,c.geometry.line_bytes-1);
            done=access(level+1,base,c.geometry.line_bytes,done);
            done=add(done,c.fill_cycles);
            counts.fill_bytes=add(counts.fill_bytes,c.geometry.line_bytes);
            // Lower-level allocation may have invalidated entries in this set, so
            // choose the victim only after the recursive fill has returned.
            if (set.size()==c.geometry.ways) {
                const U victim=set.back();
                set.pop_back();
                counts.evictions=add(counts.evictions,1);
                if (config_.inclusion=="inclusive") invalidate_above(level,victim);
            }
        }
        set.insert(set.begin(),line);
        if (line==last) break; // Avoid wrapping an address-space final line.
    }
    return done;
}
Completion Hierarchy::submit(const std::string& entry,Request r) {
    if (entry!=config_.stimulus.entry) throw std::invalid_argument("mixed injection entries require unsupported arbitration");
    if (!r.bytes || r.arrival<last_arrival_) throw std::invalid_argument("invalid request size/arrival order");
    (void)add(r.address,r.bytes-1);
    std::size_t level=config_.caches.size();
    if (entry!="memory") {
        const auto it=std::find_if(config_.caches.begin(),config_.caches.end(),[&](const auto& l){return l.name==entry;});
        if (it==config_.caches.end()) throw std::invalid_argument("unknown injection entry");
        level=static_cast<std::size_t>(it-config_.caches.begin());
    }
    if (entry=="memory") {
        // Direct injection intentionally bypasses the blocking cache admission gate.
        // Keep a single entry per run: mixed cache/direct traffic needs arbitration.
        if (++transactions_>config_.stimulus.max_transactions) throw std::length_error("transaction budget exceeded");
        const auto result=memory_.submit(r);
        stats_.memory_requests=add(stats_.memory_requests,1);
        stats_.memory_requested_bytes=add(stats_.memory_requested_bytes,r.bytes);
        stats_.memory_wire_bytes=add(stats_.memory_wire_bytes,result.wire_bytes);
        ready_=std::max(ready_,result.done);
        last_arrival_=r.arrival;
        return result;
    }
    const U start=std::max(r.arrival,ready_);
    const U before=stats_.memory_wire_bytes;
    const U done=access(level,r.address,r.bytes,start);
    ready_=done;
    last_arrival_=r.arrival;
    return {r.arrival,start,done,stats_.memory_wire_bytes-before};
}
std::vector<U> Hierarchy::boundary_key(U now) const {
    if (now<ready_) throw std::invalid_argument("boundary must be drained");
    std::vector<U> key;
    for (const auto& level:state_) for (const auto& set:level) {
        key.push_back(set.size()); key.insert(key.end(),set.begin(),set.end());
    }
    const auto& m=memory_.state();
    key.push_back(m.next_issue>now?m.next_issue-now:0);
    for (U done:m.pending) if (done>now) key.push_back(done-now);
    return key;
}
void Hierarchy::skip_fixed_blocks(U boundary,U elapsed,const HierarchyStats& before,U blocks) {
    if (boundary<ready_ || before.levels.size()!=stats_.levels.size())
        throw std::invalid_argument("invalid fixed-point boundary");
    const U delta=mul(elapsed,blocks);
    auto advance=[&](U& value,U old) {
        if (old>value) throw std::invalid_argument("counter delta cannot be negative");
        value=add(value,mul(value-old,blocks));
    };
    for (std::size_t i=0;i<stats_.levels.size();++i) {
        auto& a=stats_.levels[i]; const auto& b=before.levels[i];
        advance(a.references,b.references); advance(a.hits,b.hits); advance(a.misses,b.misses);
        advance(a.evictions,b.evictions); advance(a.invalidations,b.invalidations); advance(a.fill_bytes,b.fill_bytes);
    }
    advance(stats_.memory_requests,before.memory_requests);
    advance(stats_.memory_requested_bytes,before.memory_requested_bytes);
    advance(stats_.memory_wire_bytes,before.memory_wire_bytes);
    ServiceState next;
    const U new_boundary=add(boundary,delta);
    next.next_issue=add(new_boundary,memory_.state().next_issue>boundary?memory_.state().next_issue-boundary:0);
    next.last_arrival=new_boundary;
    for (auto done:memory_.state().pending) if (done>boundary) next.pending.push_back(add(done,delta));
    memory_=MemoryService(config_.memory,next);
    ready_=new_boundary;
    last_arrival_=new_boundary;
    // transactions_ measures actual fallback work, not skipped analytical demand.
}
}
