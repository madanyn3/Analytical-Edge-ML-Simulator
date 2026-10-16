#include "edge/cluster_config.hpp"
#include "edge/checked.hpp"
#include "ini.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace edge {
namespace {
using U=std::uint64_t;
using checked::add;
using checked::mul;
void fail(const std::string& what) { throw std::invalid_argument(what); }
LevelConfig level(ini::Fields& f,const std::string& section,bool banked) {
    const auto p=section+".";
    LevelConfig l;
    l.geometry={f.number(p+"line_bytes"),f.number(p+"sets"),f.number(p+"ways")};
    l.lookup_cycles=f.number(p+"lookup_cycles");
    l.fill_cycles=f.number(p+"fill_cycles");
    if (banked) l.arbitration=f.take(p+"bank_arbitration");
    // Explicit warm L1/L2 contents are deferred in v2; the key stays mandatory so a
    // future value cannot be silently ignored by an older build.
    f.literal(p+"initial_lines","empty");
    l.provenance=f.take(p+"provenance");
    return l;
}
void check_level(const LevelConfig& l,const char* name) {
    const auto& g=l.geometry;
    if (!g.line_bytes || !g.sets || !g.ways) fail(std::string(name)+": zero cache geometry");
    if (!l.lookup_cycles) fail(std::string(name)+": lookup_cycles must be >= 1");
    if (l.provenance.empty()) fail(std::string(name)+": provenance required");
    (void)mul(mul(g.sets,g.ways),g.line_bytes);
}
}
std::uint64_t config_version(std::string_view text,const std::string& source) {
    auto doc=ini::lex(text,source);
    const auto v=doc.fields.number("model.version");
    if (v!=1 && v!=2) fail(source+": unsupported config version "+std::to_string(v));
    return v;
}
ClusterConfig parse_cluster_config(std::string_view text,const std::string& source) {
    auto doc=ini::lex(text,source);
    auto& f=doc.fields;
    std::vector<std::string> cores;
    for (const auto& s:doc.sections) {
        static const std::set<std::string> fixed{"model","topology","l1","l2","memory","stimulus"};
        if (s.name.starts_with("core.") && ini::identifier(s.name.substr(5))) cores.push_back(s.name.substr(5));
        else if (!fixed.contains(s.name))
            fail(source+":"+std::to_string(s.line)+": unknown section "+s.name);
    }
    ClusterConfig c;
    f.literal("model.version","2");
    c.clock_hz=f.number("model.clock_hz");
    f.literal("model.access","read_only");
    f.literal("model.prefetch","none");
    f.literal("model.cache_concurrency","blocking");
    f.literal("model.coherence","none");
    auto& t=c.topology;
    t.kind=f.take("topology.kind");
    t.k=f.number("topology.k");
    t.mc_tile=f.number("topology.mc_tile");
    t.router_cycles=f.number("topology.router_cycles");
    t.link_cycles=f.number("topology.link_cycles");
    t.flit_bytes=f.number("topology.flit_bytes");
    t.link_arbitration=f.take("topology.link_arbitration");
    t.provenance=f.take("topology.provenance");
    c.l1=level(f,"l1",false);
    c.l2=level(f,"l2",true);
    f.literal("memory.policy","fifo");
    c.memory={f.number("memory.first_beat_cycles"),f.number("memory.beat_bytes"),
              f.number("memory.beat_cycles"),f.number("memory.issue_cycles"),
              f.number("memory.outstanding"),f.take("memory.provenance")};
    for (const auto& name:cores) {
        const auto p="core."+name+".";
        c.cores.push_back({name,f.number(p+"tile"),f.number(p+"start_cycles"),f.number(p+"compute_cycles"),
                           f.number(p+"count"),f.number(p+"address"),f.number(p+"bytes"),
                           f.number(p+"stride"),f.number(p+"address_period")});
    }
    auto& s=c.stimulus;
    s.engine=f.take("stimulus.engine");
    s.max_requests=f.number("stimulus.max_requests");
    s.max_transactions=f.number("stimulus.max_transactions");
    s.max_events=f.number("stimulus.max_events");
    f.reject_leftovers();
    try { validate_cluster_config(c); }
    catch (const std::exception& e) { fail(source+": configuration constraint: "+e.what()); }
    return c;
}
ClusterConfig load_cluster_config(const std::string& path) {
    return parse_cluster_config(ini::read_file(path),path);
}
void validate_cluster_config(const ClusterConfig& c) {
    if (!c.clock_hz) fail("clock_hz must be positive");
    const auto& t=c.topology;
    if (t.kind!="mesh" && t.kind!="torus") fail("topology.kind must be mesh or torus");
    if (t.k<2 || t.k>8) fail("topology.k must be in 2..8");
    // With k=2 a wraparound link would duplicate the existing neighbour link.
    if (t.kind=="torus" && t.k<3) fail("torus requires k >= 3");
    const U tiles=t.k*t.k;
    if (t.mc_tile>=tiles) fail("mc_tile outside the grid");
    // t_l >= 1 guarantees every grant's consequences land strictly later (design §5).
    if (!t.link_cycles) fail("link_cycles must be >= 1");
    if (!t.flit_bytes) fail("flit_bytes must be >= 1");
    if (t.link_arbitration!="fifo" && t.link_arbitration!="round_robin")
        fail("link_arbitration must be fifo or round_robin");
    if (t.provenance.empty()) fail("topology provenance required");
    check_level(c.l1,"l1");
    check_level(c.l2,"l2");
    if (!c.l2.arbitration.empty() && c.l2.arbitration!="fifo" && c.l2.arbitration!="round_robin" &&
        c.l2.arbitration!="fixed_priority")
        fail("bank_arbitration must be fifo, round_robin or fixed_priority");
    if (c.l2.arbitration.empty()) fail("bank_arbitration required");
    if (!c.l1.arbitration.empty()) fail("l1 has no arbitration knob");
    // Nested lines: one L1 line is always inside exactly one L2 line, hence one bank.
    if (c.l2.geometry.line_bytes%c.l1.geometry.line_bytes)
        fail("l2.line_bytes must be a multiple of l1.line_bytes");
    // Largest packet: header + full L2 line. Its link occupancy F*t_l must fit.
    const U flits=add(1,checked::ceil_div(c.l2.geometry.line_bytes,t.flit_bytes));
    (void)mul(flits,t.link_cycles);
    const auto& m=c.memory;
    if (!m.first_beat_cycles || !m.beat_bytes || !m.beat_cycles || !m.issue_cycles ||
        !m.outstanding || m.outstanding>1000000 || m.provenance.empty())
        fail("memory parameters/provenance invalid");
    if (c.cores.empty() || c.cores.size()>tiles) fail("need 1..k*k cores");
    std::set<std::string> names;
    std::set<U> used;
    U requests=0;
    for (const auto& core:c.cores) {
        if (!ini::identifier(core.name) || !names.insert(core.name).second) fail("duplicate or invalid core name "+core.name);
        if (core.tile>=tiles) fail("core "+core.name+": tile outside the grid");
        if (!used.insert(core.tile).second) fail("core "+core.name+": tile already has a core");
        if (!core.bytes || !core.address_period) fail("core "+core.name+": bytes and address_period must be >= 1");
        const U last=core.count?std::min(core.count,core.address_period)-1:0;
        (void)add(add(core.address,mul(last,core.stride)),core.bytes-1);
        requests=add(requests,core.count);
    }
    // Modeled state budget: one L1 copy per core plus one L2 bank per tile.
    const U lines=add(mul(mul(c.l1.geometry.sets,c.l1.geometry.ways),c.cores.size()),
                      mul(mul(c.l2.geometry.sets,c.l2.geometry.ways),tiles));
    if (lines>1000000) throw std::length_error("combined cache state exceeds one million lines");
    const auto& s=c.stimulus;
    if (s.engine!="finite" && s.engine!="periodic_state") fail("engine must be finite or periodic_state");
    if (!s.max_requests || s.max_requests>1000000 || !s.max_transactions || s.max_transactions>10000000 ||
        !s.max_events || s.max_events>100000000)
        fail("invalid stimulus budgets");
    if (s.engine=="finite" && requests>s.max_requests) throw std::length_error("total core reads exceed finite request budget");
}
std::string describe_cluster_config(const ClusterConfig& c) {
    std::ostringstream o;
    const auto& t=c.topology;
    o<<"[model]\nversion=2\nclock_hz="<<c.clock_hz
     <<"\naccess=read_only\nprefetch=none\ncache_concurrency=blocking\ncoherence=none\n"
     <<"[topology]\nkind="<<t.kind<<"\nk="<<t.k<<"\nmc_tile="<<t.mc_tile<<"\nrouter_cycles="<<t.router_cycles
     <<"\nlink_cycles="<<t.link_cycles<<"\nflit_bytes="<<t.flit_bytes<<"\nlink_arbitration="<<t.link_arbitration
     <<"\nprovenance="<<t.provenance<<'\n';
    auto put=[&](const char* name,const LevelConfig& l) {
        o<<'['<<name<<"]\nline_bytes="<<l.geometry.line_bytes<<"\nsets="<<l.geometry.sets
         <<"\nways="<<l.geometry.ways<<"\nlookup_cycles="<<l.lookup_cycles<<"\nfill_cycles="<<l.fill_cycles<<'\n';
        if (!l.arbitration.empty()) o<<"bank_arbitration="<<l.arbitration<<'\n';
        o<<"initial_lines=empty\nprovenance="<<l.provenance<<'\n';
    };
    put("l1",c.l1);
    put("l2",c.l2);
    const auto& m=c.memory;
    o<<"[memory]\npolicy=fifo\nfirst_beat_cycles="<<m.first_beat_cycles<<"\nbeat_bytes="<<m.beat_bytes
     <<"\nbeat_cycles="<<m.beat_cycles<<"\nissue_cycles="<<m.issue_cycles<<"\noutstanding="<<m.outstanding
     <<"\nprovenance="<<m.provenance<<'\n';
    for (const auto& k:c.cores)
        o<<"[core."<<k.name<<"]\ntile="<<k.tile<<"\nstart_cycles="<<k.start_cycles<<"\ncompute_cycles="
         <<k.compute_cycles<<"\ncount="<<k.count<<"\naddress="<<k.address<<"\nbytes="<<k.bytes
         <<"\nstride="<<k.stride<<"\naddress_period="<<k.address_period<<'\n';
    const auto& s=c.stimulus;
    o<<"[stimulus]\nengine="<<s.engine<<"\nmax_requests="<<s.max_requests
     <<"\nmax_transactions="<<s.max_transactions<<"\nmax_events="<<s.max_events<<'\n';
    return o.str();
}
}
