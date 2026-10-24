// A5 cost study (docs/fc-cache-theorem.md §6): end-to-end host time of the closed-form FC
// analyzers against enumeration plus finite analysis, as the layer grows. Each timing
// includes derive_fc and, for the finite path, enumerate_events and the flash filter.
// Timings are host measurements of this build, not properties of the target.
//   fc_cache_cost [max_finite_seconds]
#include "edge/fc_cache.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace {
using U=std::uint64_t;
using Clock=std::chrono::steady_clock;
template<class F> double seconds(F f,int repeat=1) {
    const auto t0=Clock::now();
    for (int i=0;i<repeat;++i) f();
    return std::chrono::duration<double>(Clock::now()-t0).count()/repeat;
}
// Baseline trace simulation: per-set arrays of W lines with move-to-front, O(R*W) per call.
// This is the cheapest straightforward simulator, so the comparison is not against the
// quadratic unlimited-stack finite analyzer alone.
U simulate(edge::CacheGeometry g,const std::vector<edge::Event>& call,U calls) {
    std::vector<U> ways(g.sets*g.ways,~U{0});
    U misses=0;
    for (U c=0;c<calls;++c) for (const auto& e:call)
        for (U x=e.address/g.line_bytes;x<=(e.address+e.bytes-1)/g.line_bytes;++x) {
            U* set=&ways[(x%g.sets)*g.ways];
            U i=0;
            while (i<g.ways && set[i]!=x) ++i;
            if (i==g.ways) { ++misses; i=g.ways-1; }
            for (;i>0;--i) set[i]=set[i-1];
            set[0]=x;
        }
    return misses;
}
edge::KernelIR layer(U n,U m) {
    const U w=0x08000000, b=((w+m*n+15)/16)*16+16;
    return edge::derive_fc({static_cast<int>(n),static_cast<int>(m),1,0,1073741824,-1,-128,127},
                           {0x20000000,w,b,(0x20000000+n+15)/16*16});
}
}
int main(int argc,char** argv) {
    const double budget=argc>1 ? std::atof(argv[1]) : 5.0;
    const edge::CacheGeometry g{16,8,2};
    std::printf("# Host timings (s), L=16, S=8, W=2. closed_repeated covers any number of calls (10 here);\n"
                "# simulate_per_call = mean over 10 calls, excluding enumeration (bounded W-way move-to-front);\n"
                "# finite_* (unlimited-stack exact analyzer) include enumeration; skipped above %.1f s.\n",budget);
    std::printf("n,m,references_per_call,closed_cold,closed_repeated_any_calls,enumerate,simulate_per_call,finite_cold,finite_repeated_10,agree\n");
    bool finite=true;
    for (U k=0;k<=14;++k) {
        const U n=64U<<(k/2), m=16U<<((k+1)/2);   // n*m grows 2x per step: 1 KiB ... 16 MiB.
        U checksum_closed=0, checksum_finite=0, refs=0;
        const double cold=seconds([&]{ auto r=edge::analyze_fc_flash(g,layer(n,m)); checksum_closed=r.counts.misses; refs=r.counts.references; },5);
        U rep_closed=0;
        const double rep=seconds([&]{ rep_closed=edge::analyze_fc_flash_repeated(g,layer(n,m),10).total.misses; },5);
        // Enumeration (timed on its own after a warm-up run, so page faults of the first
        // allocation are not charged), then bounded simulation of 1 and 10 calls on it.
        auto events=[&]{
            std::vector<edge::Event> out;
            for (auto e:edge::enumerate_events(layer(n,m),U{1}<<32))
                if (e.tensor==edge::Tensor::weights || e.tensor==edge::Tensor::bias) out.push_back(e);
            return out;
        };
        (void)events();
        std::vector<edge::Event> stream;
        const double enumerate=seconds([&]{ stream=events(); });
        U sim_cold=0, sim_rep=0;
        (void)simulate(g,stream,1);   // Warm-up.
        const int reps=refs<1000000 ? 5 : 1;
        sim_cold=simulate(g,stream,1);   // Checked against the closed form only.
        const double srep=seconds([&]{ sim_rep=simulate(g,stream,10); },reps)/10;
        if (sim_cold!=checksum_closed || sim_rep!=rep_closed) { std::cerr<<"simulation disagrees\n"; return 1; }
        double fcold=-1, frep=-1;
        U rep_finite=0;
        if (finite) {
            auto flash=[&]{ return stream; };
            fcold=seconds([&]{ checksum_finite=edge::analyze_reuse(g,flash(),{},U{1}<<40).misses; });
            frep=seconds([&]{ rep_finite=edge::analyze_repeated(g,flash(),10,{},U{1}<<40).total.misses; });
            if (fcold+frep>budget) finite=false;
        }
        const bool agree=fcold<0 || (checksum_closed==checksum_finite && rep_closed==rep_finite);
        std::printf("%llu,%llu,%llu,%.3g,%.3g,%.3g,%.3g,%s,%s,%s\n",(unsigned long long)n,(unsigned long long)m,
            (unsigned long long)refs,cold,rep,enumerate,srep,
            fcold<0 ? "" : std::to_string(fcold).c_str(),frep<0 ? "" : std::to_string(frep).c_str(),
            fcold<0 ? "n/a" : agree ? "yes" : "NO");
        if (!agree) return 1;
    }
}
