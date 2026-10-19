// Analytical results for the tiled cluster (docs/cluster-design.md §6) checked
// against the exact event evaluator. The closed forms live in cluster_analytic.cpp
// and never call the evaluator; the evaluator never calls them.
#include "edge/cluster.hpp"
#include "edge/cluster_analytic.hpp"
#include "edge/noc.hpp"
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
using U=std::uint64_t;
void check(bool ok,const std::string& what) { if (!ok) throw std::runtime_error(what); }
edge::ClusterConfig base() {
    edge::ClusterConfig c;
    c.clock_hz=1000000000;
    c.topology={"mesh",2,0,1,1,16,"round_robin","test"};
    c.l1={{32,4,2},1,1,"","test"};
    c.l2={{64,16,4},4,2,"round_robin","test"};
    c.memory={40,16,2,2,4,"test"};
    c.stimulus={"finite",1000,100000,1000000};
    return c;
}

// N1, geometry: per-tile hop sums and the grid-wide mean-hop table.
void hop_sums() {
    for (const char* kind:{"mesh","torus"}) for (U k=2;k<=8;++k) {
        if (std::string(kind)=="torus" && k<3) continue;
        const edge::TopologyConfig t{kind,k,0,1,1,16,"fifo","test"};
        const edge::noc::Topology topo(t);  // Route-walking hop count, verified vs BFS.
        U total=0;
        for (U a=0;a<k*k;++a) {
            U brute=0;
            for (U b=0;b<k*k;++b) {
                check(edge::analytic::hops(t,a,b)==topo.hops(a,b),"per-dimension hops = routed hops");
                brute+=topo.hops(a,b);
            }
            check(edge::analytic::hop_sum_from(t,a)==brute,"k(S(x)+S(y)) = sum of hops from tile");
            total+=brute;
        }
        // Mean over k^4 (source, destination) pairs, cross-multiplied to stay exact.
        const U k4=k*k*k*k;
        if (std::string(kind)=="mesh") check(3*k*total==2*(k*k-1)*k4,"mesh mean 2(k^2-1)/(3k)");
        else if (k%2==0) check(2*total==k*k4,"even torus mean k/2");
        else check(2*k*total==(k*k-1)*k4,"odd torus mean (k^2-1)/(2k)");
    }
}

// N1, hand goldens on the design's 2x2 example (same numbers as cluster_tests).
void goldens() {
    const auto c=base();
    check(edge::analytic::memory_line_cycles(c)==46,"D = 40 + 3*2");
    check((edge::analytic::zero_load_read(c,0,3)==edge::analytic::ZeroLoadRead{1,16,76}),"diagonal: 1/16/76");
    check(edge::analytic::zero_load_read(c,3,3).l2_miss==66,"local bank cold miss 66");
    check(edge::analytic::zero_load_read(c,0,0).l2_miss==54,"bank on MC tile 54");
}

// N1 against the evaluator. One core on tile p reads with stride = half an L1 line,
// so read j touches L1 line j/2 and L2 line j/(2r), r = L2 line / L1 line:
//   j odd             -> L1 hit (second half of the line just filled);
//   j mod 2r == 0     -> cold L2 miss, bank = j/(2r) (lines 0..B-1, one per bank);
//   otherwise         -> L1 miss, L2 hit in the same bank.
// A lone blocking core never meets contention: each packet, bank grant and memory
// admission it causes has finished before its next read issues. Memory is idle
// for the next miss because its issue spacing I = max(J0, b*tau) <= D when
// J0 <= L and tau <= L, and consecutive misses are at least D apart.
void zero_load_vs_evaluator() {
    std::mt19937_64 rng(20261019);
    auto pick=[&](U lo,U hi){ return std::uniform_int_distribution<U>(lo,hi)(rng); };
    struct Grid { const char* kind; U k; };
    U runs=0,reads=0;
    for (const Grid g:{Grid{"mesh",2},Grid{"mesh",3},Grid{"mesh",4},Grid{"torus",3},Grid{"torus",4}})
        for (int trial=0;trial<6;++trial) {
            auto c=base();
            const U B=g.k*g.k;
            const U flit[]={4,8,16,32};
            c.topology={g.kind,g.k,pick(0,B-1),pick(0,3),pick(1,3),flit[pick(0,3)],
                        pick(0,1)?"fifo":"round_robin","test"};
            const U l1=pick(0,1)?16:32, r=pick(0,1)?2:4;
            c.l1={{l1,4,2},pick(1,3),pick(0,3),"","test"};
            c.l2={{l1*r,8,2},pick(1,6),pick(0,4),pick(0,1)?"fifo":"round_robin","test"};
            const U L=pick(20,60);
            const U beat[]={8,16,32};
            c.memory={L,beat[pick(0,2)],pick(1,4),pick(1,L),pick(1,4),"test"};
            for (U tile=0;tile<B;++tile) {
                c.cores={{"c",tile,pick(0,5),pick(0,7),2*r*B,0,4,l1/2,2*r*B}};
                const auto res=edge::run_cluster(c);
                const std::string at=std::string(" [")+g.kind+" k="+std::to_string(g.k)+" trial "+
                                     std::to_string(trial)+" tile "+std::to_string(tile)+"]";
                check(res.reads.size()==2*r*B,"all reads evaluated"+at);
                U hit_sum=0;
                for (U j=0;j<res.reads.size();++j) {
                    const auto& x=res.reads[j];
                    check(x.index==j,"single core completes in order"+at);
                    // Base address 0: L2 line j/(2r) < B, so it lives in bank j/(2r).
                    const U bank=j/(2*r);
                    const auto z=edge::analytic::zero_load_read(c,tile,bank);
                    const U want=j%2?z.l1_hit:(j%(2*r)==0?z.l2_miss:z.l2_hit);
                    check(x.done-x.issue==want,"read "+std::to_string(j)+" latency "+
                          std::to_string(x.done-x.issue)+" != closed form "+std::to_string(want)+at);
                    if (j%(2*r)==2) hit_sum+=x.done-x.issue;   // One L2 hit per bank.
                }
                check(hit_sum==edge::analytic::l2_hit_sum_over_banks(c,tile),"sum of L2 hits over banks"+at);
                ++runs; reads+=res.reads.size();
            }
        }
    std::cout<<"N1 zero-load: "<<runs<<" single-core runs, "<<reads<<" reads match closed forms\n";
}
}
int main() {
    try {
        hop_sums();
        goldens();
        zero_load_vs_evaluator();
        std::cout<<"cluster theorem tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
