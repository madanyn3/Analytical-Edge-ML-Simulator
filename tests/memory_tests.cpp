#include "edge/memory.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>

namespace {
using U=std::uint64_t;
void check(bool ok) { if (!ok) throw std::runtime_error("memory contract failed"); }
template<class F> void rejects(F f) {
    bool caught=false;
    try { f(); } catch (const std::exception&) { caught=true; }
    check(caught);
}
// Independent finite oracle: bounded resident map, timestamps, minimum-age eviction.
// Byte stepping identifies line crossings without the analytical geometry helper.
edge::CacheCounts replay(edge::CacheGeometry g,const std::vector<edge::Event>& es,
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
void equal(const edge::CacheCounts& a,const edge::CacheCounts& b) {
    check(a.references==b.references && a.hits==b.hits && a.misses==b.misses &&
          a.evictions==b.evictions && a.fill_bytes==b.fill_bytes && a.final_state==b.final_state);
}
edge::Event read(U a,std::uint32_t n=1) { return {edge::Tensor::weights,a,n,false}; }
}
int main() {
    try {
        // Geometry across every offset, including non-power-of-two dimensions.
        for (U l:{1U,3U,8U,16U}) for (U a=0;a<40;++a) for (U n=0;n<40;++n) {
            std::map<U,bool> lines;
            for (U j=0;j<n;++j) lines[(a+j)/l]=true;
            check(edge::contiguous_lines(a,n,l)==lines.size());
        }
        // Exact boundary D_set=W-1 hits, D_set=W misses; other sets do not count.
        equal(edge::analyze_reuse({1,2,2},std::vector{read(0),read(2),read(1),read(0)}),
              replay({1,2,2},{read(0),read(2),read(1),read(0)}));
        check(edge::analyze_reuse({1,2,2},std::vector{read(0),read(2),read(4),read(0)}).misses==4);
        const edge::CacheState partial{{4,0},{1}};
        const std::vector<edge::Event> warm_reads{read(0),read(3),read(1),read(6),read(4)};
        equal(edge::analyze_reuse({1,2,3},warm_reads,partial),replay({1,2,3},warm_reads,partial));
        check(edge::analyze_reuse({1,2,3},{},partial).final_state==partial);
        // Symbolic counts and final states against reference, over mapping/capacity edges.
        U sweep_cases=0;
        for (U s=1;s<=5;++s) for (U w=1;w<=4;++w) for (U first=0;first<7;++first)
        for (U n=0;n<=15;++n) for (U r=0;r<=5;++r) {
            std::vector<edge::Event> es;
            for (U i=0;i<r;++i) for (U j=0;j<n;++j) es.push_back(read((first+j)*8));
            equal(edge::analyze_cold_sweep({8,s,w},first,n,r),replay({8,s,w},es));
            ++sweep_cases;
        }
        const auto large=edge::analyze_cold_sweep({8,2,2},0,5,1000000000);
        check(large.references==5000000000 && large.misses==3000000002);
        // Arbitrary bounded traces; warm states and composition checked independently.
        std::mt19937 rng(479);
        for (U s=1;s<=5;++s) for (U w=1;w<=4;++w) for (U trial=0;trial<30;++trial) {
            edge::CacheGeometry g{3,s,w};
            std::vector<edge::Event> a,b;
            for (int j=0;j<35;++j) a.push_back(read(rng()%160,1+rng()%9));
            for (int j=0;j<25;++j) b.push_back(read(rng()%160,1+rng()%9));
            const auto first=edge::analyze_reuse(g,a);
            equal(first,replay(g,a));
            const auto second=edge::analyze_reuse(g,b,first.final_state);
            equal(second,replay(g,b,first.final_state));
            a.insert(a.end(),b.begin(),b.end());
            const auto combined=edge::analyze_reuse(g,a);
            check(combined.misses==first.misses+second.misses && combined.final_state==second.final_state);
        }
        // FC source schedules, including unaligned weights, tails and row pairs.
        for (int n:{1,3,4,5,8,9}) for (int m:{1,2,3,5}) {
            const auto ir=edge::derive_fc({n,m,1,0,1073741824,-1,-128,127},
                {0x20040000,0x08010000,0x08010100,0x20040100});
            std::vector<edge::Event> flash;
            for (auto e:edge::enumerate_events(ir))
                if (e.tensor==edge::Tensor::weights || e.tensor==edge::Tensor::bias) flash.push_back(e);
            equal(edge::analyze_reuse({8,2,2},flash),replay({8,2,2},flash));
        }
        const std::vector<edge::Event> es{read(0)};
        rejects([&]{edge::analyze_reuse({0,2,2},es);});
        rejects([&]{edge::analyze_reuse({8,2,2},es,{{0,0},{}});});
        rejects([&]{edge::analyze_reuse({8,2,2},es,{{1},{}});});
        rejects([&]{edge::analyze_reuse({8,2,2},es,{{0,2,4},{}});});
        rejects([&]{edge::analyze_reuse({8,2,2},es,{},0);});
        rejects([&]{edge::analyze_reuse({8,2,2},std::vector{edge::Event{edge::Tensor::output,0,1,true}});});
        const U max=std::numeric_limits<U>::max();
        rejects([&]{edge::contiguous_lines(max,2,8);});
        rejects([&]{edge::analyze_cold_sweep({8,2,2},0,5,max);});
        rejects([&]{edge::analyze_cold_sweep({8,2,2},max,2,1);});
        rejects([&]{edge::analyze_cold_sweep({8,1,1},0,2,max/2);}); // fill-byte overflow
        std::vector<edge::BandwidthDemand> d{{"flash",40,20e6,"synthetic"},{"SRAM",10,100e6,"synthetic"}};
        auto bound=edge::roofline_bound(15,100e6,"synthetic",d);
        check(bound.complete_roofline && std::abs(bound.known_lower_bound_seconds-2e-6)<1e-18);
        check(edge::roofline_bound(1000,100e6,"synthetic",d).known_lower_bound_seconds==1e-5);
        check(edge::roofline_bound(15,100e6,"synthetic",d,3e-6).known_lower_bound_seconds==3e-6);
        d[0].bytes_per_second=std::nullopt;
        bound=edge::roofline_bound(15,std::nullopt,"",d);
        check(!bound.complete_roofline && bound.unknowns.size()==2);
        rejects([&]{edge::roofline_bound(15,0,"synthetic",d);});
        rejects([&]{edge::roofline_bound(15,1,"",d);});
        rejects([&]{edge::roofline_bound(15,1,"synthetic",d,-1);});
        rejects([&]{edge::roofline_bound(15,std::numeric_limits<double>::quiet_NaN(),"synthetic",d);});
        d[0].bytes_per_second=0;
        rejects([&]{edge::roofline_bound(15,1,"synthetic",d);});
        d[0].bytes_per_second=1;
        d[0].provenance="";
        rejects([&]{edge::roofline_bound(15,1,"synthetic",d);});
        d[0].provenance="synthetic";
        d[0].boundary=d[1].boundary;
        rejects([&]{edge::roofline_bound(15,1,"synthetic",d);});
        std::cout<<"Passed "<<sweep_cases<<" symbolic sweep cases, 600 trace/composition cases, "
                 <<"24 FC schedules, geometry, bounds and rejection checks\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
