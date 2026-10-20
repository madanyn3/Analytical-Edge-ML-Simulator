// Theorem A1 (docs/fc-cache-theorem.md §3): the closed-form cold-call analyzer against the
// independent oracle, which replays the enumerated flash stream reference by reference.
#include "cache_oracle.hpp"
#include "edge/fc_cache.hpp"
#include <iostream>
#include <random>

namespace {
using U=std::uint64_t;
using oracle::check;
edge::KernelIR fc(U n,U m,U w,U b) {
    return edge::derive_fc({static_cast<int>(n),static_cast<int>(m),1,0,1073741824,-1,-128,127},
                           {0x20040000,w,b,(0x20040000+n+15)/16*16});
}
std::vector<edge::Event> flash(const edge::KernelIR& ir) {
    std::vector<edge::Event> out;
    for (auto e:edge::enumerate_events(ir,10000000))
        if (e.tensor==edge::Tensor::weights || e.tensor==edge::Tensor::bias) out.push_back(e);
    return out;
}
template<class F> void rejects(F f) {
    bool caught=false;
    try { f(); } catch (const std::invalid_argument&) { caught=true; }
    check(caught);
}
}
int main() {
    try {
        U cases=0,cursor=0,middle=0,boundary=0,bias=0;
        auto run=[&](edge::CacheGeometry g,const edge::KernelIR& ir) {
            const auto r=edge::analyze_fc_flash(g,ir);
            oracle::equal(r.counts,oracle::replay(g,flash(ir)));
            cursor+=r.cursor_re_misses; middle+=r.middle_re_misses;
            boundary+=r.boundary_re_misses; bias+=r.bias_re_misses;
            ++cases;
        };
        // Exhaustive: every n in a window above the domain edge (all n mod 4 and mod L),
        // m up to 7 (pairs and an odd row), both base offsets in an SL period that 16-byte
        // alignment allows, bias placed directly after the weights' last line or apart.
        for (U L:{4U,8U,16U,32U}) for (U S:{1U,2U,3U,4U,8U}) for (U W:{1U,2U,3U,4U}) {
            const U SL=S*L;
            for (U n=2*L+8;n<2*L+8+20;++n) for (U m:{1U,2U,3U,4U,5U,7U})
            for (U wo:{U{0},(SL/2+15)/16*16<SL ? (SL/2+15)/16*16 : U{0}}) for (U gap:{U{1},U{3}}) {
                const U w=0x08000000+wo;
                const U b=((w+m*n+L-1)/L+gap*S-1)*L/16*16+16;   // 16-aligned, past w's lines.
                run({L,S,W},fc(n,m,w,b));
            }
        }
        // Random larger layers: n up to 700, m up to 40, arbitrary 16-aligned bases.
        std::mt19937 rng(20261023);
        for (int trial=0;trial<300;++trial) {
            const U L=U{4}<<(rng()%4), S=1+rng()%16, W=1+rng()%4;
            const U n=2*L+8+rng()%600, m=1+rng()%40;
            const U w=0x08000000+16*(rng()%64), b=(w+m*n+L*(1+rng()%40))/16*16+16;
            run({L,S,W},fc(n,m,w,b));
        }
        // Every re-reference kind occurs and misses somewhere.
        check(cursor>0 && middle>0 && boundary>0 && bias>0);
        // Outside the domain: n < 2L+8, and weight/bias ranges sharing a line.
        rejects([]{edge::analyze_fc_flash({8,2,2},fc(23,4,0x08000000,0x08001000));});
        rejects([]{edge::analyze_fc_flash({32,2,2},fc(80,1,0x08000000,0x08000050));});
        rejects([]{edge::analyze_fc_flash({0,2,2},fc(80,1,0x08000000,0x08001000));});
        // Cost does not depend on n or m: a 2^20 x 2^10 layer without enumeration.
        const auto big=edge::analyze_fc_flash({16,64,2},fc(U{1}<<20,1024,0x80000000,0xC8000000));
        check(big.counts.hits+big.counts.misses==big.counts.references &&
              big.distinct_lines==(U{1}<<30)/16+1024*4/16);
        std::cout<<"Passed "<<cases<<" A1 cases against the oracle (re-misses: cursor "<<cursor
                 <<", middle "<<middle<<", boundary "<<boundary<<", bias "<<bias<<")\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
