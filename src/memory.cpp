#include "edge/memory.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
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
