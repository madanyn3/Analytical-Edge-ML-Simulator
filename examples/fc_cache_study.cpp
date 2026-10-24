// A4 approximation study (docs/fc-cache-theorem.md §5): exact FC flash misses (Theorems
// A1/A2) against a fully associative LRU of equal capacity and the random-set-mapping
// model, over a sweep of the row length n. Prints CSV; scripts/plot_fc_study.py draws it.
//   fc_cache_study [L S m n_first n_last]   (defaults 16 8 16 40 296)
#include "edge/cache_approx.hpp"
#include "edge/fc_cache.hpp"
#include <cstdio>
#include <cstdlib>
#include <iostream>

int main(int argc,char** argv) {
    using U=std::uint64_t;
    try {
        auto arg=[&](int i,U def){ return argc>i ? std::strtoull(argv[i],nullptr,10) : def; };
        const U L=arg(1,16), S=arg(2,8), m=arg(3,16), n0=arg(4,40), n1=arg(5,296);
        std::cout<<"# Synthetic contract (memory-models.md); not an L4R5 ART claim. w=0x08000000, bias after weights.\n";
        std::cout<<"L,S,W,m,n,n_mod_SL,refs_per_call,exact_cold,fa_cold,rs_cold,exact_warm,fa_warm,rs_warm,cursor_re_misses\n";
        for (U W:{1U,2U,4U}) for (U n=n0;n<=n1;++n) {
            const U w=0x08000000, b=((w+m*n+L-1)/L*L+15)/16*16+16;
            const auto ir=edge::derive_fc({static_cast<int>(n),static_cast<int>(m),1,0,1073741824,-1,-128,127},
                                          {0x20040000,w,b,(0x20040000+n+15)/16*16});
            std::vector<edge::Event> call;
            for (auto e:edge::enumerate_events(ir,10000000))
                if (e.tensor==edge::Tensor::weights || e.tensor==edge::Tensor::bias) call.push_back(e);
            const edge::CacheGeometry g{L,S,W};
            const auto exact=edge::analyze_fc_flash_repeated(g,ir,2);
            const auto cold=edge::approximate_misses(g,call,0), warm=edge::approximate_misses(g,call,1);
            const auto a1=edge::analyze_fc_flash(g,ir);
            std::printf("%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%llu,%.6f,%.6f,%llu\n",
                (unsigned long long)L,(unsigned long long)S,(unsigned long long)W,(unsigned long long)m,
                (unsigned long long)n,(unsigned long long)(n%(S*L)),(unsigned long long)exact.first.references,
                (unsigned long long)exact.first.misses,cold.fully_associative_misses,cold.random_set_misses,
                (unsigned long long)exact.steady.misses,warm.fully_associative_misses,warm.random_set_misses,
                (unsigned long long)a1.cursor_re_misses);
        }
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
