#include "edge/memory.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace edge {
namespace {
using U = std::uint64_t;
U add(U a, U b) {
    if (b > std::numeric_limits<U>::max()-a) throw std::overflow_error("count/address overflow");
    return a+b;
}
U mul(U a, U b) {
    if (a && b > std::numeric_limits<U>::max()/a) throw std::overflow_error("count overflow");
    return a*b;
}
void validate(CacheGeometry g) {
    if (!g.line_bytes || !g.sets || !g.ways) throw std::invalid_argument("zero cache dimension");
    if (mul(g.sets,g.ways)>1000000) throw std::length_error("cache state exceeds supported budget");
    (void)mul(mul(g.sets,g.ways),g.line_bytes);
}
CacheState state(CacheGeometry g, CacheState s) {
    if (s.empty()) s.resize(g.sets);
    if (s.size()!=g.sets) throw std::invalid_argument("initial set count");
    for (U i=0;i<g.sets;++i) {
        if (s[i].size()>g.ways) throw std::invalid_argument("initial set over capacity");
        for (std::size_t j=0;j<s[i].size();++j) {
            const auto line=s[i][j];
            if (line%g.sets!=i || line>std::numeric_limits<U>::max()/g.line_bytes)
                throw std::invalid_argument("invalid initial line mapping");
            if (std::find(s[i].begin(),s[i].begin()+static_cast<std::ptrdiff_t>(j),line)
                !=s[i].begin()+static_cast<std::ptrdiff_t>(j))
                throw std::invalid_argument("duplicate initial line");
        }
    }
    return s;
}
void positive(double x) {
    if (!std::isfinite(x) || x<=0) throw std::invalid_argument("ceiling must be finite and positive");
}
}
U contiguous_lines(U a,U n,U l) {
    if (!l) throw std::invalid_argument("zero line size");
    return n ? add(a,n-1)/l-a/l+1 : 0;
}
CacheCounts analyze_reuse(CacheGeometry g,std::span<const Event> events,CacheState initial,U limit) {
    validate(g);
    // Unlimited recency stacks retain evicted identities: position is D_set.
    auto stacks=state(g,std::move(initial));
    CacheCounts out;
    for (const auto& e:events) {
        if (e.write) throw std::invalid_argument("cache writes unsupported");
        out.references=add(out.references,contiguous_lines(e.address,e.bytes,g.line_bytes));
        if (out.references>limit) throw std::length_error("finite reuse expansion exceeds limit");
    }
    for (const auto& e:events) {
        const U n=contiguous_lines(e.address,e.bytes,g.line_bytes);
        for (U j=0;j<n;++j) {
            const U line=e.address/g.line_bytes+j;
            auto& stack=stacks[line%g.sets];
            const auto it=std::find(stack.begin(),stack.end(),line);
            const auto distance=static_cast<U>(it-stack.begin());
            if (it!=stack.end() && distance<g.ways) ++out.hits;
            else {
                ++out.misses;
                if (stack.size()>=g.ways) ++out.evictions;
            }
            if (it!=stack.end()) stack.erase(it);
            stack.insert(stack.begin(),line);
        }
    }
    for (auto& stack:stacks) if (stack.size()>g.ways) stack.resize(g.ways);
    out.final_state=std::move(stacks);
    out.fill_bytes=mul(out.misses,g.line_bytes);
    return out;
}
CacheCounts analyze_cold_sweep(CacheGeometry g,U first,U n,U repeats) {
    validate(g);
    CacheCounts out;
    out.final_state.resize(g.sets);
    if (!n || !repeats) return out;
    const U last=add(first,n-1);
    if (last>std::numeric_limits<U>::max()/g.line_bytes) throw std::overflow_error("line address overflow");
    out.references=mul(n,repeats);
    out.misses=n;
    U resident=0;
    for (U s=0;s<g.sets;++s) {
        // Offset of the first line belonging to set s, without signed arithmetic.
        const U offset=(s+g.sets-first%g.sets)%g.sets;
        const U k=offset<n ? 1+(n-1-offset)/g.sets : 0;
        if (k>g.ways) out.misses=add(out.misses,mul(repeats-1,k));
        const U keep=std::min(k,g.ways);
        resident=add(resident,keep);
        for (U j=0;j<keep;++j)
            out.final_state[s].push_back(first+offset+(k-1-j)*g.sets);
    }
    out.hits=out.references-out.misses;
    out.evictions=out.misses-resident;
    out.fill_bytes=mul(out.misses,g.line_bytes);
    return out;
}
RepeatedCounts analyze_repeated(CacheGeometry g,std::span<const Event> events,U calls,
                                CacheState initial,U limit) {
    validate(g);
    RepeatedCounts out;
    auto start=state(g,std::move(initial));
    if (!calls) { out.total.final_state=std::move(start); return out; }
    out.first=analyze_reuse(g,events,start,limit);
    // Line references of one call, in order, and each line's window [first, last].
    struct Window { U first, last; };
    std::map<U,Window> windows;
    U position=0;
    for (const auto& e:events) {
        const U n=contiguous_lines(e.address,e.bytes,g.line_bytes);
        for (U j=0;j<n;++j,++position) {
            const U line=e.address/g.line_bytes+j;
            auto [it,fresh]=windows.try_emplace(line,Window{position,position});
            if (!fresh) it->second.last=position;
        }
    }
    // Re-misses (references after a line's first in the call) depend only on the call
    // itself: a cold call misses each distinct line once, plus every re-miss.
    const auto cold=analyze_reuse(g,events,{},limit);
    out.re_misses=cold.misses-windows.size();
    // Warm first references: D_cyc(x) = (k_s - 1) - nested(x), where y is nested in x
    // iff first_x < first_y and last_y < last_x (y is never referenced in the cyclic
    // gap from x's last reference to its next first reference). Hit iff D_cyc < W.
    std::vector<std::vector<Window>> by_set(g.sets);
    for (const auto& [line,w]:windows) by_set[line%g.sets].push_back(w);
    for (auto& set:by_set) {
        const U k=set.size();
        // Sort by first descending; a Fenwick tree over ranks of `last` counts nested
        // windows among those already inserted (which all start later).
        std::sort(set.begin(),set.end(),[](const Window& a,const Window& b){return a.first>b.first;});
        std::vector<U> lasts;
        for (const auto& w:set) lasts.push_back(w.last);
        std::sort(lasts.begin(),lasts.end());
        std::vector<U> tree(k+1,0);
        for (const auto& w:set) {
            const U rank=static_cast<U>(std::lower_bound(lasts.begin(),lasts.end(),w.last)-lasts.begin()); // # lasts < w.last
            U nested=0;
            for (U i=rank;i>0;i-=i&(~i+1)) nested+=tree[i];
            if (k-1-nested>=g.ways) ++out.warm_first_reference_misses;
            for (U i=rank+1;i<=k;i+=i&(~i+1)) ++tree[i];
        }
    }
    auto& s=out.steady;
    s.references=out.first.references;
    s.misses=add(out.warm_first_reference_misses,out.re_misses);
    s.hits=s.references-s.misses;
    // Occupancy never decreases and is equal at both ends of a steady call, so it is
    // constant: a set that misses is full, and every steady miss evicts.
    s.evictions=s.misses;
    s.fill_bytes=mul(s.misses,g.line_bytes);
    s.final_state=out.first.final_state;
    const U later=calls-1;
    auto& t=out.total;
    t.references=add(out.first.references,mul(later,s.references));
    t.hits=add(out.first.hits,mul(later,s.hits));
    t.misses=add(out.first.misses,mul(later,s.misses));
    t.evictions=add(out.first.evictions,mul(later,s.evictions));
    t.fill_bytes=add(out.first.fill_bytes,mul(later,s.fill_bytes));
    t.final_state=out.first.final_state;
    return out;
}
RooflineBound roofline_bound(U macs,std::optional<double> peak,const std::string& provenance,
    std::span<const BandwidthDemand> demands,std::optional<double> dependency) {
    RooflineBound out;
    if (peak) {
        positive(*peak);
        if (provenance.empty()) throw std::invalid_argument("missing compute provenance");
        out.known_lower_bound_seconds=static_cast<double>(macs)/ *peak;
    } else if (macs) out.unknowns.push_back("compute ceiling (MAC/s)");
    std::vector<std::string> names;
    for (const auto& d:demands) {
        if (d.boundary.empty() || std::find(names.begin(),names.end(),d.boundary)!=names.end())
            throw std::invalid_argument("empty or duplicate resource boundary");
        names.push_back(d.boundary);
        if (d.bytes_per_second) {
            positive(*d.bytes_per_second);
            if (d.provenance.empty()) throw std::invalid_argument("missing bandwidth provenance");
            out.known_lower_bound_seconds=std::max(out.known_lower_bound_seconds,
                static_cast<double>(d.bytes)/ *d.bytes_per_second);
        } else if (d.bytes) out.unknowns.push_back(d.boundary+" bandwidth (byte/s)");
    }
    out.complete_roofline=out.unknowns.empty();
    if (dependency) {
        if (!std::isfinite(*dependency) || *dependency<0) throw std::invalid_argument("invalid dependency bound");
        out.known_lower_bound_seconds=std::max(out.known_lower_bound_seconds,*dependency);
    }
    if (!std::isfinite(out.known_lower_bound_seconds)) throw std::overflow_error("time overflow");
    return out;
}
}
