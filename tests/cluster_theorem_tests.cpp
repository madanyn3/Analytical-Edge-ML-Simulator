// Analytical results for the tiled cluster (docs/cluster-design.md §6) checked
// against the exact event evaluator. The closed forms live in cluster_analytic.cpp
// and never call the evaluator; the evaluator never calls them.
#include "edge/cluster.hpp"
#include "edge/cluster_analytic.hpp"
#include "edge/noc.hpp"
#include "edge/service.hpp"
#include <map>
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

// K3a reference: P closed-loop sources driving the production MemoryService. Requests
// are submitted in arrival order (ties by source index, which is also round-robin with
// pointer 0); each completion schedules that source's next arrival done + Z.
// No recurrence is used here; returns admissions and the source of each request.
struct Driven { std::vector<U> start, source, arrival; };
Driven drive(const edge::ServiceConfig& m,U line,U P,U Z,U count) {
    edge::MemoryService service(m);
    std::multimap<std::pair<U,U>,int> ready;   // (arrival, source)
    for (U p=0;p<P;++p) ready.emplace(std::pair{Z,p},0);
    Driven d;
    while (d.start.size()<count) {
        const auto [arrival,source]=ready.begin()->first;
        ready.erase(ready.begin());
        const auto c=service.submit({arrival,0,line});
        d.start.push_back(c.start); d.source.push_back(source); d.arrival.push_back(arrival);
        ready.emplace(std::pair{c.done+Z,source},0);
    }
    return d;
}

void k3a() {
    namespace a=edge::analytic;
    // Design worked example: P=4, Z=10, D=12, I=2, K=2 -> lambda 6, R = 4*6 - 10 = 14.
    {
        const auto s=a::k3a_admissions(4,10,12,2,2,8);
        check((s==std::vector<U>{10,12,22,24,34,36,46,48}),"worked example admissions");
        check((a::k3a_cycle_time(4,10,12,2,2)==a::Rational{6,1}),"worked example lambda = 6");
        const auto d=drive({12,16,1,2,2,"test"},16,4,10,8);
        check(d.start==s,"worked example on MemoryService");
    }
    std::mt19937_64 rng(20261020);
    auto pick=[&](U lo,U hi){ return std::uniform_int_distribution<U>(lo,hi)(rng); };
    const U N=600,tail=300;
    U cases=0,longest=0,memory_bound=0,issue_bound=0,source_bound=0;
    for (int trial=0;trial<3000;++trial) {
        const U beat_bytes[]={8,16,32};
        const edge::ServiceConfig m{pick(1,60),beat_bytes[pick(0,2)],pick(1,8),pick(1,20),pick(1,6),"test"};
        const U line=U{16}<<pick(0,2);
        const U P=pick(1,8),Z=pick(0,80);
        // D and I for one aligned line, from the service contract (not from the service).
        const U beats=(line+m.beat_bytes-1)/m.beat_bytes;
        const U D=m.first_beat_cycles+(beats-1)*m.beat_cycles;
        const U I=std::max(m.issue_cycles,beats*m.beat_cycles);
        const std::string at=" [trial "+std::to_string(trial)+" P="+std::to_string(P)+" Z="+std::to_string(Z)+
                             " D="+std::to_string(D)+" I="+std::to_string(I)+" K="+std::to_string(m.outstanding)+"]";
        const auto s=a::k3a_admissions(P,Z,D,I,m.outstanding,N);
        const auto d=drive(m,line,P,Z,N);
        // Every prefix: admission n of the service equals the recurrence.
        for (U n=0;n<N;++n) {
            check(d.start[n]==s[n],"admission "+std::to_string(n)+" differs from recurrence"+at);
            check(d.source[n]==n%P,"cyclic source order"+at);
        }
        // Eventual slope: some period q has s_(n+q) - s_n = q*lambda on the whole tail.
        const auto lambda=a::k3a_cycle_time(P,Z,D,I,m.outstanding);
        U q=0;
        for (U cand=lambda.den;cand<=200 && !q;cand+=lambda.den) {
            bool ok=true;
            for (U n=tail;n+cand<N && ok;++n) ok=s[n+cand]-s[n]==cand*lambda.num/lambda.den;
            if (ok) q=cand;
        }
        check(q!=0,"no period with slope lambda in the tail"+at);
        // Transient: first n0 from which the periodic relation holds through the end.
        U n0=tail;
        while (n0>0 && s[n0-1+q]-s[n0-1]==q*lambda.num/lambda.den) --n0;
        longest=std::max(longest,n0);
        // Response time over one period: sum (s_n + D - a_n) = q (P lambda - Z).
        U response=0;
        for (U n=tail;n<tail+q;++n) response+=d.start[n]+D-d.arrival[n];
        check(response*lambda.den==q*(P*lambda.num-Z*lambda.den),"mean response R = P*lambda - Z"+at);
        // Which term binds (ties counted once, memory credits first).
        if (lambda==a::Rational{I,1}) ++issue_bound;
        else if (lambda.num*m.outstanding==D*lambda.den) ++memory_bound;
        else ++source_bound;
        ++cases;
    }
    std::cout<<"K3a: "<<cases<<" configurations x "<<N<<" admissions match MemoryService; binding term "
             <<"I "<<issue_bound<<", D/K "<<memory_bound<<", (Z+D)/P "<<source_bound
             <<"; longest transient "<<longest<<" admissions\n";
}
}
int main() {
    try {
        hop_sums();
        goldens();
        zero_load_vs_evaluator();
        k3a();
        std::cout<<"cluster theorem tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
