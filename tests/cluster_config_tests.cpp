// Version-2 (tiled cluster) configuration: acceptance, exact round trip and one
// rejection per documented constraint (docs/cluster-design.md section 7.1).
#include "edge/cluster_config.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void check(bool ok,const std::string& what) { if (!ok) throw std::runtime_error(what); }
template<class F> void rejects(F f,const std::string& what) {
    bool caught=false;
    try { f(); } catch (const std::exception&) { caught=true; }
    check(caught,"expected rejection: "+what);
}
std::string replace(std::string text,const std::string& from,const std::string& to) {
    const auto at=text.find(from);
    check(at!=std::string::npos,"mutation anchor missing: "+from);
    return text.replace(at,from.size(),to);
}
std::string slurp(const char* path) {
    std::ifstream in(path,std::ios::binary);
    check(static_cast<bool>(in),"cannot open example config");
    std::ostringstream s; s<<in.rdbuf(); return s.str();
}
}
int main(int argc,char** argv) {
    try {
        check(argc==3,"usage: cluster_config_tests CLUSTER_INI V1_INI");
        const auto text=slurp(argv[1]);
        const auto c=edge::parse_cluster_config(text,"example");
        // Golden values from the design document's 2x2 example.
        check(c.topology.kind=="mesh" && c.topology.k==2 && c.topology.mc_tile==0,"topology");
        check(c.topology.router_cycles==1 && c.topology.link_cycles==1 && c.topology.flit_bytes==16,"network timing");
        check(c.l1.geometry.line_bytes==32 && c.l2.geometry.line_bytes==64 && c.l2.geometry.sets==16,"geometry");
        check(c.l2.arbitration=="round_robin" && c.l1.arbitration.empty(),"arbitration");
        check(c.cores.size()==4 && c.cores[3].name=="c3" && c.cores[3].tile==3 && c.cores[3].address==0x30000,"cores");
        check(c.memory.first_beat_cycles==40 && c.memory.outstanding==4,"memory");
        // The effective description is itself a valid config that parses back exactly.
        const auto described=edge::describe_cluster_config(c);
        check(edge::parse_cluster_config(described)==c,"round trip");
        check(edge::describe_cluster_config(edge::parse_cluster_config(described))==described,"stable description");

        check(edge::config_version(text)==2,"version 2 detected");
        check(edge::config_version(slurp(argv[2]))==1,"version 1 detected");
        rejects([&]{edge::config_version(replace(text,"version=2","version=3"));},"version 3");
        rejects([&]{edge::parse_cluster_config(slurp(argv[2]));},"v1 file through v2 parser");
        rejects([&]{edge::parse_config(text);},"v2 file through v1 parser");

        // Each mutation violates exactly one documented rule.
        const std::vector<std::pair<std::string,std::string>> bad{
            {"kind=mesh","kind=ring"},
            {"k=2","k=1"},
            {"k=2","k=9"},
            {"mc_tile=0","mc_tile=4"},
            {"link_cycles=1","link_cycles=0"},
            {"flit_bytes=16","flit_bytes=0"},
            {"link_arbitration=round_robin","link_arbitration=priority"},
            {"bank_arbitration=round_robin","bank_arbitration=lottery"},
            {"line_bytes=64","line_bytes=48"},           // L2 line not a multiple of L1 line.
            {"sets=16","sets=0"},
            {"lookup_cycles=4","lookup_cycles=0"},
            {"initial_lines=empty","initial_lines=1,2"},  // Warm state deferred in v2.
            {"coherence=none","coherence=mesi"},
            {"policy=fifo","policy=frfcfs"},
            {"outstanding=4","outstanding=0"},
            {"tile=3","tile=0"},                          // Two cores on tile 0.
            {"tile=3","tile=4"},
            {"[core.c3]","[core.c2]"},                    // Duplicate section.
            {"bytes=4","bytes=0"},
            {"address_period=64","address_period=0"},
            {"address=0x30000","address=0xffffffffffffffff"},  // Address overflow.
            {"engine=periodic_state","engine=stable_blocks"},
            {"engine=periodic_state","engine=finite"},    // 4000 reads > budget below.
            {"max_requests=100000","max_requests=0"},
            {"max_events=10000000","max_events=100000001"},
            {"provenance=synthetic teaching network","provenance="},
            {"[stimulus]","[stimulus]\ntypo=1"},
            {"[stimulus]","[noc]\n[stimulus]"},
        };
        for (const auto& [from,to]:bad) {
            auto mutated=replace(text,from,to);
            if (to=="engine=finite") mutated=replace(mutated,"max_requests=100000","max_requests=3999");
            rejects([&]{edge::parse_cluster_config(mutated);},from+" -> "+to);
        }
        // Torus is legal from k=3, never at k=2 (duplicate wraparound link).
        rejects([&]{edge::parse_cluster_config(replace(text,"kind=mesh","kind=torus"));},"torus k=2");
        auto torus=replace(replace(text,"kind=mesh","kind=torus"),"k=2","k=3");
        check(edge::parse_cluster_config(torus).topology.kind=="torus","torus k=3 accepted");
        // Fewer cores than tiles is allowed: idle tiles still host a bank and router.
        const auto cut=text.find("[core.c3]");
        auto three=text.substr(0,cut)+text.substr(text.find("[stimulus]"));
        check(edge::parse_cluster_config(three).cores.size()==3,"idle tile accepted");
        // Overflow in packet occupancy F*t_l is rejected, not wrapped.
        rejects([&]{edge::parse_cluster_config(replace(text,"link_cycles=1","link_cycles=0xffffffffffffffff"));},"occupancy overflow");
        std::cout<<"cluster config tests passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
