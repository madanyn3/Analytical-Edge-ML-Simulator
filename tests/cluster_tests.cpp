// Cluster evaluator: hand-derived golden cases on the design's 2x2 mesh
// (docs/cluster-design.md §3, §6 N1, §7.2). t_r = t_l = 1, 16-byte flits,
// L1 32 B lines (RESP = 3 flits), L2 64 B lines (MRESP = 5 flits), MC on tile 0,
// memory D = 40 + 3*2 = 46 cycles per 64-byte line.
#include "edge/cluster.hpp"
#include <iostream>
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
edge::CoreConfig core(const char* name,U tile,U count,U address,U stride,U period) {
    return {name,tile,0,10,count,address,4,stride,period};
}
U latency(const edge::ClusterResult& r,U core,U index) {
    for (const auto& x:r.reads) if (x.core==core && x.index==index) return x.done-x.issue;
    throw std::runtime_error("read not found");
}
}
int main() {
    try {
        // Address 192 = L2 line 3 -> bank 3 (diagonal from tile 0, 2 hops).
        // Reads: 192 (cold), 224 (other L1 line, same L2 line), 192 again.
        {
            auto c=base();
            c.cores={core("c0",0,3,192,32,2)};
            const auto r=edge::run_cluster(c);
            // Cold: L1 1 + REQ T0(2,1)=4 + L2 4 + MREQ 4 + D 46 + MRESP T0(2,5)=8
            //       + L2 fill 2 + RESP T0(2,3)=6 + L1 fill 1 = 76.
            check(latency(r,0,0)==76,"cold diagonal L2 miss = 76");
            check(r.reads[0].issue==10 && r.reads[0].done==86,"issue at start + compute");
            // L1 miss, L2 hit: 1 + 4 + 4 + 6 + 1 = 16.
            check(latency(r,0,1)==16,"diagonal L2 hit = 16");
            check(latency(r,0,2)==1,"L1 hit = lookup only");
            check(r.reads[1].issue==96 && r.reads[2].issue==122,"a_(k+1) = d_k + compute");
            const auto& b=r.banks[3];
            check(b.l2.references==2 && b.l2.hits==1 && b.l2.misses==1 && b.l2.fill_bytes==64,"bank 3 counters");
            check(b.busy_cycles==(4+4+46+8+2)+4,"bank busy: whole miss round trip, then one lookup");
            check(r.memory_requests==1 && r.memory_wire_bytes==64,"one line from memory");
            check(r.cores[0].l1.misses==2 && r.cores[0].l1.hits==1,"L1 counters");
            check(r.l2_state[3][0]==std::vector<U>{3},"L2 line 3 resident in bank 3 set 0");
            check(r.l1_state[0][2]==std::vector<U>{6} && r.l1_state[0][3]==std::vector<U>{7},"L1 lines 6 and 7");
        }
        // Local bank (core on tile 3, bank 3): no network for REQ/RESP.
        {
            auto c=base();
            c.cores={core("c3",3,2,192,0,1)};
            const auto r=edge::run_cluster(c);
            check(latency(r,0,0)==1+4+4+46+8+2+1,"local bank cold miss = 66");
            check(latency(r,0,1)==1,"then an L1 hit");
        }
        // Bank on the memory-controller tile: no network at all.
        {
            auto c=base();
            c.cores={core("c0",0,1,0,0,1)};
            check(latency(edge::run_cluster(c),0,0)==1+4+46+2+1,"bank 0 on MC tile: 54");
        }
        // Bank conflict: c1 (tile 1) and c2 (tile 2) read line 3 at the same time.
        // Both REQs (1 hop) arrive at bank 3 at t=13. Round-robin pointer 0 reaches
        // tile 1 first. c1: grant 13, lookup 17, MREQ 3->0 delivered 21, memory done 67,
        // MRESP delivered 75, fill 77, RESP 3->1 (1 hop, 3 flits) delivered 81, L1 fill 82.
        // c2 waits for the blocking bank until 77, then hits: lookup 81, RESP 85, fill 86.
        {
            auto c=base();
            c.cores={core("c1",1,1,192,0,1),core("c2",2,1,192,0,1)};
            const auto r=edge::run_cluster(c);
            check(latency(r,0,0)==72,"c1 wins round-robin and misses: 72");
            check(latency(r,1,0)==76,"c2 queues behind the miss, then hits: 76");
            check(r.banks[3].wait_cycles==77-13,"bank wait = 64 cycles");
            // Fixed priority also favours tile 1; FIFO ties break by source tile too.
            for (const char* p:{"fifo","fixed_priority"}) {
                c.l2.arbitration=p;
                const auto q=edge::run_cluster(c);
                check(latency(q,0,0)==72 && latency(q,1,0)==76,std::string("tie order under ")+p);
            }
        }
        // Budgets fail explicitly instead of truncating.
        {
            auto c=base();
            c.cores={core("c0",0,50,0,64,50)};
            c.stimulus.max_events=20;
            bool threw=false;
            try { edge::run_cluster(c); } catch (const std::length_error&) { threw=true; }
            check(threw,"event budget enforced");
        }
        std::cout<<"cluster golden tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
