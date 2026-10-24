// Approximate models for A4: reuse distances against a naive count, the fully associative
// model against the exact analyzer with one set, and the research-plan §7.4 diagnostic.
#include "cache_oracle.hpp"
#include "edge/cache_approx.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <set>

namespace {
using U=std::uint64_t;
using oracle::check;
edge::Event read(U a,std::uint32_t n=1) { return {edge::Tensor::weights,a,n,false}; }
bool near(double a,double b) { return std::abs(a-b)<=1e-9*std::max(1.0,std::abs(b)); }
}
int main() {
    try {
        std::mt19937 rng(20261022);
        for (int trial=0;trial<300;++trial) {
            const U L=1+rng()%8, S=1+rng()%5, W=1+rng()%4;
            std::vector<edge::Event> es;
            for (int j=0;j<40;++j) es.push_back(read(rng()%150,1+rng()%6));
            // Naive distances: walk back to the previous reference, collecting distinct lines.
            std::vector<U> lines;
            for (const auto& e:es) {
                U prev=~U{0};
                for (U b=0;b<e.bytes;++b) { const U x=(e.address+b)/L; if (x!=prev) lines.push_back(x); prev=x; }
            }
            const auto d=edge::reuse_distances(L,es);
            check(d.size()==lines.size());
            for (U i=0;i<lines.size();++i) {
                std::set<U> between;
                U j=i;
                bool found=false;
                while (j>0) { --j; if (lines[j]==lines[i]) { found=true; break; } between.insert(lines[j]); }
                check(found ? d[i]==between.size() : d[i]==edge::no_reuse);
            }
            // Fully associative model = exact LRU with one set of S*W ways (cold and warm).
            const auto cold=oracle::replay({L,1,S*W},es), warm=oracle::replay({L,1,S*W},es,cold.final_state);
            const auto a0=edge::approximate_misses({L,S,W},es,0), a1=edge::approximate_misses({L,S,W},es,1);
            check(a0.references==cold.references && near(a0.fully_associative_misses,static_cast<double>(cold.misses)));
            check(a1.references==warm.references && near(a1.fully_associative_misses,static_cast<double>(warm.misses)));
            // With one set the random-set model is exact too.
            const auto one=edge::approximate_misses({L,1,W},es,1);
            check(near(one.random_set_misses,static_cast<double>(oracle::replay({L,1,W},es,oracle::replay({L,1,W},es).final_state).misses)));
        }
        // Binomial sums against direct products, and the edges.
        for (U S=2;S<=6;++S) for (U W=1;W<=4;++W) for (U D=0;D<=30;++D) {
            double direct=0;
            for (U k=0;k<W && k<=D;++k) {
                double c=1; for (U i=0;i<k;++i) c=c*static_cast<double>(D-i)/static_cast<double>(i+1);
                direct+=c*std::pow(1.0/static_cast<double>(S),static_cast<double>(k))*std::pow(1-1.0/static_cast<double>(S),static_cast<double>(D-k));
            }
            check(near(edge::random_set_hit_probability(D,S,W),direct));
            if (D<W) check(near(edge::random_set_hit_probability(D,S,W),1.0));
        }
        check(edge::random_set_hit_probability(100000,4,2)>=0);
        // §7.4 diagnostic: 4-set direct-mapped, 32-byte lines. Alternating 0 and 128 conflicts
        // (same set) and misses every time; 0 and 32 does not. Both have the same reuse
        // distances, so both statistical models predict the same count for the two streams.
        std::vector<edge::Event> conflict,spread;
        for (int i=0;i<10;++i) { conflict.push_back(read(0)); conflict.push_back(read(128)); spread.push_back(read(0)); spread.push_back(read(32)); }
        const edge::CacheGeometry dm{32,4,1};
        check(oracle::replay(dm,conflict).misses==20 && oracle::replay(dm,spread).misses==2);
        const auto ac=edge::approximate_misses(dm,conflict,0), as=edge::approximate_misses(dm,spread,0);
        check(near(ac.random_set_misses,as.random_set_misses) && near(ac.random_set_misses,2+18*0.25));
        check(near(ac.fully_associative_misses,2) && near(as.fully_associative_misses,2));
        std::cout<<"Passed 300 distance/model cases, binomial sums and the set-conflict diagnostic\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
