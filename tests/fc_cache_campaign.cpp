// A3: exhaustive verification campaign for the FC cache theorems (docs/fc-cache-theorem.md
// §4). Every case compares the closed-form cold and warm counts, evictions and final states
// with the independent oracle replaying the enumerated stream twice (cold, then warm).
//   fc_cache_campaign           full grid (minutes; the result is recorded in the docs)
//   fc_cache_campaign --quick   subset run by ctest
#include "cache_oracle.hpp"
#include "edge/fc_cache.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

namespace {
using U=std::uint64_t;
struct Case { U L,S,W,n,m,w,b; };
struct Tally {
    U cases{}, references{}, resonant{}, warm_differs{}, partial{};
    void operator+=(const Tally& t) {
        cases+=t.cases; references+=t.references; resonant+=t.resonant;
        warm_differs+=t.warm_differs; partial+=t.partial;
    }
};
// Bias placements: the first 16-aligned address after the weights' last line, and one
// shifted to a different residue mod S*L when S*L > 16.
U bias_base(U w,U m,U n,U L,U SL,int which) {
    const U after=((w+m*n+L-1)/L*L+15)/16*16;
    return which ? after+16*(1+SL/32) : after;
}
// One layer under every W: enumerate once, replay per W.
void run(const Case& c0,U max_w,Tally& t) {
    const auto ir=edge::derive_fc({static_cast<int>(c0.n),static_cast<int>(c0.m),1,0,1073741824,-1,-128,127},
                                  {0x20040000,c0.w,c0.b,(0x20040000+c0.n+15)/16*16});
    std::vector<edge::Event> call;
    for (auto e:edge::enumerate_events(ir,10000000))
        if (e.tensor==edge::Tensor::weights || e.tensor==edge::Tensor::bias) call.push_back(e);
    for (U W=1;W<=max_w;++W) {
        const edge::CacheGeometry g{c0.L,c0.S,W};
        const auto r=edge::analyze_fc_flash_repeated(g,ir,2);
        const auto cold=oracle::replay(g,call);
        const auto warm=oracle::replay(g,call,cold.final_state);
        oracle::equal(r.first,cold);
        oracle::equal(r.steady,warm);
        ++t.cases;
        t.references+=2*cold.references;
        const auto a1=edge::analyze_fc_flash(g,ir);
        if (a1.cursor_re_misses) ++t.resonant;
        if (warm.misses!=cold.misses) ++t.warm_differs;
        if (r.warm_first_reference_misses && r.warm_first_reference_misses<a1.distinct_lines) ++t.partial;
    }
}
}
int main(int argc,char** argv) {
    const bool quick=argc>1 && !std::strcmp(argv[1],"--quick");
    const auto start=std::chrono::steady_clock::now();
    std::vector<Case> grid;
    const std::vector<U> Ls=quick ? std::vector<U>{4,6,16} : std::vector<U>{2,4,6,8,12,16};
    const std::vector<U> Ss=quick ? std::vector<U>{1,3,8} : std::vector<U>{1,2,3,4,5,6,7,8,12,16};
    const U max_w=quick ? 4 : 6;
    for (U L:Ls) for (U S:Ss) {
        const U SL=S*L;
        for (U n=2*L+8;n<=64;++n) for (U m=1;m<=64;++m) {
            if (quick && !(m<=3 || m==7 || m==16 || m==33 || m==64)) continue;
            if (quick && n%5) continue;
            // Every 16-aligned weight base residue in one S*L period (one if S*L <= 16).
            for (U wo=0;wo<std::max(SL,U{16});wo+=16) for (int which:{0,1}) {
                if (quick && wo>16) continue;
                const U w=0x08000000+wo;
                grid.push_back({L,S,0,n,m,w,bias_base(w,m,n,L,SL,which)});
            }
        }
    }
    // Random large layers, including L = 32, beyond the grid.
    std::mt19937 rng(20261024);
    const int randoms=quick ? 100 : 2000;
    std::vector<std::pair<Case,U>> large;
    for (int i=0;i<randoms;++i) {
        const U L=quick ? U{4}<<(rng()%4) : std::vector<U>{3,4,5,8,10,16,24,32,64}[rng()%9];
        const U S=1+rng()%32, W=1+rng()%8;
        const U n=2*L+8+rng()%700, m=1+rng()%80;
        const U w=0x08000000+16*(rng()%64);
        large.push_back({{L,S,W,n,m,w,bias_base(w,m,n,L,S*L,static_cast<int>(rng()%2))},W});
    }
    std::atomic<std::size_t> next{0};
    std::mutex lock;
    Tally total;
    std::atomic<bool> failed{false};
    auto worker=[&]{
        Tally t;
        try {
            for (std::size_t i;(i=next++)<grid.size()+large.size();) {
                if (i<grid.size()) run(grid[i],max_w,t);
                else {
                    const auto& [c,W]=large[i-grid.size()];
                    Tally one;
                    // Large cases: only their own W (W loop inside run starts at 1).
                    Case only=c;
                    run(only,W,one);
                    t+=one;
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard g(lock);
            std::cerr<<e.what()<<'\n';
            failed=true;
        }
        std::lock_guard g(lock);
        total+=t;
    };
    std::vector<std::thread> pool;
    for (unsigned i=0;i<std::max(1U,std::thread::hardware_concurrency());++i) pool.emplace_back(worker);
    for (auto& th:pool) th.join();
    if (failed) return 1;
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<(quick ? "Quick" : "Full")<<" A3 campaign passed: "<<total.cases<<" geometry cases ("
             <<grid.size()<<" grid layers x W=1.."<<max_w<<", "<<large.size()<<" random large layers x W=1..W), "
             <<total.references<<" oracle references, 0 mismatches\n"
             <<"  resonant (cursor re-misses) "<<total.resonant<<", warm != cold "<<total.warm_differs
             <<", partial warm survival "<<total.partial<<", "<<seconds<<" s\n";
}
