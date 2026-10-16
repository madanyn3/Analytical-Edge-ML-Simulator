#include "edge/testbench.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {
using U=std::uint64_t;
void check(bool ok,const char* what) { if (!ok) throw std::runtime_error(what); }
template<class F> void rejects(F f) {
    bool caught=false;
    try { f(); } catch (const std::exception&) { caught=true; }
    check(caught,"expected rejection");
}
edge::ModelConfig profile() {
    edge::ModelConfig c;
    c.clock_hz=100000000; c.inclusion="inclusive";
    c.memory={12,8,2,2,4,"synthetic"};
    c.caches={{"L1",{16,2,2},1,1,{},"synthetic"},{"L2",{32,2,2},3,2,{},"synthetic"}};
    c.stimulus={"L1","open_loop","finite",8,0,4,16,4,0,100,10000,100000};
    return c;
}
// Independent discrete-tick reference: wait while the bus/credits are busy.
// It counts byte-addressed beats explicitly and never calls the production
// demand calculation, admission recurrence or periodic formula.
struct TickMemory {
    edge::ServiceConfig c;
    U issue_ready{};
    std::vector<U> pending;
    edge::Completion submit(edge::Request r) {
        std::map<U,bool> beats;
        for (U b=0;b<r.bytes;++b) beats[(r.address+b)/c.beat_bytes]=true;
        U t=r.arrival;
        for (;;) {
            const U active=static_cast<U>(std::count_if(pending.begin(),pending.end(),[&](U d){return d>t;}));
            if (t>=issue_ready && active<c.outstanding) break;
            ++t;
            check(t<1000000,"oracle cycle budget");
        }
        std::erase_if(pending,[&](U d){return d<=t;});
        U done=t+c.first_beat_cycles;
        for (std::size_t b=1;b<beats.size();++b) done+=c.beat_cycles;
        U bus_free=t;
        for (std::size_t b=0;b<beats.size();++b) bus_free+=c.beat_cycles;
        issue_ready=t+c.issue_cycles;
        if (bus_free>issue_ready) issue_ready=bus_free;
        pending.push_back(done);
        std::sort(pending.begin(),pending.end());
        return {r.arrival,t,done,static_cast<U>(beats.size())*c.beat_bytes};
    }
};
// Independent hierarchy oracle uses timestamps in resident maps (not MRU vectors).
// It discovers crossing lines by walking bytes, then routes read misses recursively.
struct Oracle {
    edge::ModelConfig c;
    TickMemory memory;
    std::vector<std::vector<std::map<U,U>>> resident;
    edge::HierarchyStats stats;
    U clock{},ready{};
    explicit Oracle(edge::ModelConfig config):c(std::move(config)),memory{c.memory,0,{}} {
        stats.levels.resize(c.caches.size());
        for (const auto& l:c.caches) {
            resident.emplace_back(l.geometry.sets);
            if (!l.initial.empty()) for (std::size_t s=0;s<l.initial.size();++s)
                for (auto p=l.initial[s].rbegin();p!=l.initial[s].rend();++p) resident.back()[s][*p]=++clock;
        }
    }
    U access(std::size_t level,U address,U bytes,U time) {
        if (level==c.caches.size()) {
            const auto result=memory.submit({time,address,bytes});
            ++stats.memory_requests; stats.memory_requested_bytes+=bytes; stats.memory_wire_bytes+=result.wire_bytes;
            return result.done;
        }
        const auto g=c.caches[level].geometry;
        U previous=std::numeric_limits<U>::max();
        for (U b=0;b<bytes;++b) {
            const U line=(address+b)/g.line_bytes;
            if (b && line==previous) continue;
            previous=line;
            auto& set=resident[level][line%g.sets]; auto& counts=stats.levels[level];
            ++counts.references; time+=c.caches[level].lookup_cycles;
            if (set.contains(line)) ++counts.hits;
            else {
                ++counts.misses;
                time=access(level+1,line*g.line_bytes,g.line_bytes,time)+c.caches[level].fill_cycles;
                counts.fill_bytes+=g.line_bytes;
                if (set.size()==g.ways) {
                    auto victim=std::min_element(set.begin(),set.end(),[](auto a,auto b2){return a.second<b2.second;});
                    const U base=victim->first*g.line_bytes;
                    set.erase(victim); ++counts.evictions;
                    if (c.inclusion=="inclusive") for (std::size_t i=0;i<level;++i)
                        for (auto& upper:resident[i]) {
                            for (auto it=upper.begin();it!=upper.end();) {
                                const U a=it->first*c.caches[i].geometry.line_bytes;
                                if (a>=base && a<base+g.line_bytes) {it=upper.erase(it); ++stats.levels[i].invalidations;}
                                else ++it;
                            }
                        }
                }
            }
            set[line]=++clock;
        }
        return time;
    }
    U submit(edge::Request r,std::size_t entry=0) {ready=access(entry,r.address,r.bytes,std::max(r.arrival,ready)); return ready;}
    std::vector<edge::CacheState> state() const {
        std::vector<edge::CacheState> out;
        for (const auto& level:resident) {
            out.emplace_back();
            for (const auto& set:level) {
                std::vector<std::pair<U,U>> order(set.begin(),set.end());
                std::sort(order.begin(),order.end(),[](auto a,auto b){return a.second>b.second;});
                out.back().emplace_back();
                for (auto p:order) out.back().back().push_back(p.first);
            }
        }
        return out;
    }
};
void equal(const edge::HierarchyStats& a,const edge::HierarchyStats& b) {
    check(a.levels==b.levels && a.memory_requests==b.memory_requests &&
          a.memory_requested_bytes==b.memory_requested_bytes && a.memory_wire_bytes==b.memory_wire_bytes,"hierarchy counters");
}
std::string replace(std::string s,const std::string& a,const std::string& b) {
    const auto pos=s.find(a); check(pos!=std::string::npos,"test replacement key"); s.replace(pos,a.size(),b); return s;
}
}
int main() {
    try {
        auto c=profile();
        // Config round trip, strict syntax, typo/duplicate/policy/range checks.
        const auto text=edge::describe_config(c);
        check(edge::describe_config(edge::parse_config(text))==text,"config round trip");
        for (const auto& mutation:std::vector<std::pair<std::string,std::string>>{
            {"version=1","version=2"},{"sets=2","sets=-1"},{"sets=2","sets=2x"},
            {"sets=2","sets=18446744073709551616"},{"sets=2","sets=0"},
            {"sets=2","set=2"},{"sets=2","sets=2\nsets=2"},
            {"replacement=lru","replacement=random"},{"prefetch=none","prefetch=next_line"},
            {"cache_concurrency=blocking","cache_concurrency=nonblocking"},
            {"line_bytes=32","line_bytes=24"},{"outstanding=4","outstanding=0"},
            {"entry=L1","entry=L3"},{"initial_lines=empty","initial_lines=0,0"},
            {"initial_lines=empty","initial_lines=0,"},{"inclusion=inclusive","inclusion=exclusive"}})
            rejects([&]{edge::parse_config(replace(text,mutation.first,mutation.second));});
        rejects([&]{edge::parse_config(text+"\n[model]\n");});
        rejects([&]{edge::parse_config(text+"\n[unknown]\n");});
        rejects([&]{edge::parse_config(text+"\ntypo=1\n");});
        rejects([&]{edge::parse_config("key=3\n"+text);});
        rejects([&]{edge::parse_config(std::string(1024*1024+1,' '));});
        rejects([&]{edge::parse_config(replace(text,"sets=2","sets=1000001"));});
        auto warm=c; warm.caches[0].initial={{0},{}};
        rejects([&]{edge::validate_config(warm);});
        warm.caches[1].initial={{0},{}};
        edge::validate_config(warm);
        check(edge::parse_config(edge::describe_config(warm)).caches[0].initial==warm.caches[0].initial,"warm round trip");
        // Directed unloaded path: 1 lookup + 3 lookup + 18 memory + 2 + 1 fill =25.
        edge::Hierarchy hand(c);
        check(hand.submit("L1",{0,0,4}).done==25,"cold hand latency");
        check(hand.submit("L1",{100,0,4}).done==101,"L1 hit latency");
        check(hand.submit("L1",{200,16,4}).done==205,"L2 hit latency");
        // A lower cache smaller than L1 forces observable back-invalidations.
        auto small=c; small.caches[1].geometry={32,1,1};
        edge::Hierarchy inc(small);
        inc.submit("L1",{0,0,4}); inc.submit("L1",{100,32,4});
        check(inc.stats().levels[0].invalidations==1,"inclusive back-invalidation");
        // Memory recurrence vs independent tick oracle over widths/alignments/gaps.
        U periodic_cases=0;
        for (U k:{1U,2U,4U}) for (U latency:{1U,7U,20U}) for (U beat:{1U,3U})
        for (U issue:{1U,5U}) for (U gap:{0U,2U,30U}) for (U bytes:{1U,8U,17U}) {
            edge::ServiceConfig m{latency,8,beat,issue,k,"synthetic"};
            edge::MemoryService finite(m); TickMemory oracle{m,0,{}};
            U last=0; long double sum=0;
            for (U i=0;i<19;++i) {
                const edge::Request r{11+i*gap,3,bytes};
                const auto a=finite.submit(r),b=oracle.submit(r);
                check(a.start==b.start && a.done==b.done && a.wire_bytes==b.wire_bytes,"memory tick agreement");
                sum+=a.done-r.arrival; last=a.done;
                const auto prefix=edge::periodic_memory(m,{11,3,bytes},i+1,gap);
                check(prefix.last_done==a.done && prefix.final_state==finite.state() &&
                      std::abs(prefix.mean_latency-sum/(i+1))<1e-12L,"periodic every prefix");
            }
            const auto closed=edge::periodic_memory(m,{11,3,bytes},19,gap);
            check(closed.last_done==last && std::abs(closed.mean_latency-sum/19)<1e-12L,"periodic closed form");
            check(closed.final_state==finite.state(),"periodic outgoing state");
            edge::MemoryService resumed(m,closed.final_state);
            const edge::Request continuation{11+19*gap,5,7};
            const auto next=resumed.submit(continuation),expected=finite.submit(continuation);
            check(next.start==expected.start && next.done==expected.done && resumed.state()==finite.state(),
                  "periodic snapshot continuation");
            ++periodic_cases;
        }
        std::mt19937 rng(8873);
        for (int trial=0;trial<120;++trial) {
            edge::ServiceConfig m{1+rng()%25,1+rng()%8,1+rng()%4,1+rng()%8,1+rng()%5,"synthetic"};
            edge::MemoryService finite(m); TickMemory oracle{m,0,{}};
            U arrival=0;
            for (int i=0;i<30;++i) {
                arrival+=rng()%9;
                const edge::Request r{arrival,rng()%30,1+rng()%40};
                const auto a=finite.submit(r),b=oracle.submit(r);
                check(a.start==b.start && a.done==b.done && a.wire_bytes==b.wire_bytes,"variable memory request");
            }
        }
        // Hierarchy routing, traffic, inclusion, warm state and direct L2 injection.
        for (int trial=0;trial<120;++trial) {
            auto test=trial%3==0?warm:c;
            test.inclusion=trial%2?"inclusive":"noninclusive";
            test.stimulus.entry=trial%4?"L1":"L2";
            edge::Hierarchy real(test); Oracle oracle(test);
            U arrival=0;
            for (int i=0;i<30;++i) {
                arrival+=rng()%70;
                const edge::Request r{arrival,rng()%300,1+rng()%40};
                const auto done=real.submit(test.stimulus.entry,r);
                check(done.done==oracle.submit(r,test.stimulus.entry=="L1"?0:1),"hierarchy oracle completion");
                equal(real.stats(),oracle.stats);
                check(real.cache_state()==oracle.state(),"hierarchy outgoing state");
            }
        }
        // Fixed-state acceleration agrees with finite execution, including conflicts
        // and service issue intervals longer than completion latency.
        for (U period:{1U,3U,4U,9U}) for (U gap:{0U,2U,30U}) for (U issue:{2U,80U}) {
            auto base=c; base.stimulus.mode="closed_loop"; base.stimulus.count=103;
            base.stimulus.address_period=period; base.stimulus.spacing_cycles=gap;
            base.memory.issue_cycles=issue;
            const auto finite=edge::run_testbench(base);
            base.stimulus.engine="stable_blocks";
            const auto fast=edge::run_testbench(base);
            equal(finite.stats,fast.stats);
            check(finite.cache_state==fast.cache_state && finite.last_done==fast.last_done &&
                  finite.latency_sum==fast.latency_sum && finite.min_latency==fast.min_latency &&
                  finite.max_latency==fast.max_latency,"fixed state acceleration");
            // Canonical boundary states must preserve future service behavior.
            edge::MemoryService finite_next(base.memory,finite.memory_state);
            edge::MemoryService fast_next(base.memory,fast.memory_state);
            const edge::Request future{finite.last_done+gap,0,32};
            const auto a=finite_next.submit(future),b=fast_next.submit(future);
            check(a.start==b.start && a.done==b.done,"fixed-state future service");
        }
        auto huge=c; huge.stimulus.mode="closed_loop"; huge.stimulus.engine="stable_blocks";
        huge.stimulus.count=1000000000; huge.stimulus.spacing_cycles=0;
        const auto big=edge::run_testbench(huge);
        check(big.evaluated_requests<20 && big.skipped_requests>999999980,"billion request cache acceleration");
        const auto p=edge::periodic_memory(c.memory,{0,0,32},1000000000,0);
        check(p.last_done==8000000010ULL,"loaded bandwidth drain");
        auto serial=c.memory; serial.outstanding=1;
        check(edge::periodic_memory(serial,{0,0,32},10,0).last_done==180,"outstanding credit bottleneck");
        auto reads=edge::parse_stimulus("arrival_cycles,address,bytes\n0,0x10,4\n0,15,4\n","test",10);
        check(reads.size()==2 && reads[0].address==16,"trace parsing");
        check(edge::run_testbench(c,&reads).requests==2,"direct trace injection");
        for (const auto& bad:{"0,-1,4","0,1,0","0,0x,4","0,1,4,","0,1,4,5","0,1","0,1,4x","1,1,4\n0,1,4"})
            rejects([&]{edge::parse_stimulus(std::string("arrival_cycles,address,bytes\n")+bad,"bad",10);});
        rejects([&]{edge::parse_stimulus("arrival_cycles,address,bytes\n0,0,4\n","bad",0);});
        auto tiny=c; tiny.stimulus.max_transactions=1;
        rejects([&]{edge::run_testbench(tiny);});
        rejects([&]{edge::periodic_memory(c.memory,{0,0,32},std::numeric_limits<U>::max(),1);});
        rejects([&]{edge::MemoryService m(c.memory); m.submit({10,0,4}); m.submit({9,0,4});});
        rejects([&]{edge::Hierarchy h(c); h.submit("memory",{0,0,4});});
        rejects([&]{edge::MemoryService m(c.memory); m.submit({10,0,4});
                     edge::MemoryService restored(c.memory,m.state()); restored.submit({9,0,4});});
        auto no_skip=huge; no_skip.stimulus.max_requests=4;
        rejects([&]{edge::run_testbench(no_skip);}); // Cold transition alone is not proof.
        auto empty=c; empty.stimulus.count=0;
        check(edge::run_testbench(empty).requests==0,"empty stimulus");
        check(edge::periodic_memory(c.memory,{0,0,4},0,0).requests==0,"empty periodic stimulus");
        std::cout<<"Passed "<<periodic_cases<<" periodic/tick cases, 120 variable-memory streams, "
                 <<"120 hierarchy streams, 24 fixed-state comparisons, parser/golden/overflow tests\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
