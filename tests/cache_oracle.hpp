#pragma once
// Independent finite cache oracle shared by the cache tests: bounded resident map,
// timestamps, minimum-age eviction. Byte stepping identifies line crossings without the
// analytical geometry helpers, and nothing here is used by the analyzers it checks.
#include "edge/memory.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>

namespace oracle {
using U=std::uint64_t;
inline void check(bool ok) { if (!ok) throw std::runtime_error("memory contract failed"); }
inline edge::CacheCounts replay(edge::CacheGeometry g,const std::vector<edge::Event>& es,
                                edge::CacheState initial={}) {
    std::vector<std::map<U,U>> residents(g.sets);
    U tick=0;
    if (!initial.empty()) for (U s=0;s<g.sets;++s)
        for (auto it=initial[s].rbegin();it!=initial[s].rend();++it) residents[s][*it]=++tick;
    edge::CacheCounts out;
    for (const auto& e:es) {
        U previous=std::numeric_limits<U>::max();
        for (U b=0;b<e.bytes;++b) {
            const U line=(e.address+b)/g.line_bytes;
            if (b && line==previous) continue;
            previous=line;
            ++out.references;
            auto& set=residents[line%g.sets];
            if (set.contains(line)) ++out.hits;
            else {
                ++out.misses;
                if (set.size()==g.ways) {
                    auto victim=std::min_element(set.begin(),set.end(),
                        [](auto a,auto b2){return a.second<b2.second;});
                    set.erase(victim);
                    ++out.evictions;
                }
            }
            set[line]=++tick;
        }
    }
    out.final_state.resize(g.sets);
    for (U s=0;s<g.sets;++s) {
        std::vector<std::pair<U,U>> ordered(residents[s].begin(),residents[s].end());
        std::sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a.second>b.second;});
        for (auto p:ordered) out.final_state[s].push_back(p.first);
    }
    out.fill_bytes=out.misses*g.line_bytes;
    return out;
}
inline void equal(const edge::CacheCounts& a,const edge::CacheCounts& b) {
    check(a.references==b.references && a.hits==b.hits && a.misses==b.misses &&
          a.evictions==b.evictions && a.fill_bytes==b.fill_bytes && a.final_state==b.final_state);
}
}
