#include "edge/config.hpp"
#include "edge/checked.hpp"
#include "ini.hpp"
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace edge {
namespace {
using U=std::uint64_t;
using ini::identifier;
using ini::trim;
using Fields=ini::Fields;
}
ModelConfig parse_config(std::string_view text,const std::string& source) {
    auto doc=ini::lex(text,source);
    auto& f=doc.fields;
    std::vector<std::string> caches;
    for (const auto& sec:doc.sections) {
        const auto& section=sec.name;
        if (section.starts_with("cache.") && identifier(section.substr(6))) caches.push_back(section.substr(6));
        else if (section!="model" && section!="memory" && section!="stimulus")
            throw std::invalid_argument(source+":"+std::to_string(sec.line)+": unknown section "+section);
    }
    ModelConfig c;
    f.literal("model.version","1");
    c.clock_hz=f.number("model.clock_hz");
    c.inclusion=f.take("model.inclusion");
    f.literal("model.access","read_only");
    f.literal("model.prefetch","none");
    f.literal("model.cache_concurrency","blocking");
    f.literal("memory.policy","fifo");
    c.memory={f.number("memory.first_beat_cycles"),f.number("memory.beat_bytes"),
              f.number("memory.beat_cycles"),f.number("memory.issue_cycles"),
              f.number("memory.outstanding"),f.take("memory.provenance")};
    for (const auto& name:caches) {
        const auto p="cache."+name+".";
        f.literal(p+"replacement","lru");
        CacheConfig level;
        level.name=name;
        level.geometry={f.number(p+"line_bytes"),f.number(p+"sets"),f.number(p+"ways")};
        level.lookup_cycles=f.number(p+"lookup_cycles");
        level.fill_cycles=f.number(p+"fill_cycles");
        level.provenance=f.take(p+"provenance");
        const auto init=f.take(p+"initial_lines");
        if (init!="empty") {
            if (!level.geometry.sets || level.geometry.sets>1000000)
                throw std::invalid_argument(source+": invalid sets before initial state allocation");
            level.initial.resize(level.geometry.sets);
            std::istringstream parts(init);
            std::string token;
            if (init.back()==',') throw std::invalid_argument(source+": trailing initial_lines comma");
            while (std::getline(parts,token,',')) {
                const U block=Fields::integer(trim(token),source+": "+p+"initial_lines");
                level.initial[block%level.geometry.sets].push_back(block);
            }
        }
        c.caches.push_back(std::move(level));
    }
    auto& s=c.stimulus;
    s.entry=f.take("stimulus.entry"); s.mode=f.take("stimulus.mode"); s.engine=f.take("stimulus.engine");
    s.count=f.number("stimulus.count"); s.address=f.number("stimulus.address"); s.bytes=f.number("stimulus.bytes");
    s.stride=f.number("stimulus.stride"); s.address_period=f.number("stimulus.address_period");
    s.arrival_cycles=f.number("stimulus.arrival_cycles"); s.spacing_cycles=f.number("stimulus.spacing_cycles");
    s.max_requests=f.number("stimulus.max_requests"); s.max_transactions=f.number("stimulus.max_transactions");
    f.reject_leftovers();
    try { validate_config(c); }
    catch (const std::exception& e) { throw std::invalid_argument(source+": configuration constraint: "+e.what()); }
    return c;
}
ModelConfig load_config(const std::string& path) {
    return parse_config(ini::read_file(path),path);
}
void validate_config(const ModelConfig& c) {
    if (!c.clock_hz || (c.inclusion!="inclusive" && c.inclusion!="noninclusive") || c.caches.size()>8)
        throw std::invalid_argument("clock, inclusion policy or level count invalid");
    const auto& m=c.memory;
    if (!m.first_beat_cycles || !m.beat_bytes || !m.beat_cycles || !m.issue_cycles ||
        !m.outstanding || m.outstanding>1000000 || m.provenance.empty())
        throw std::invalid_argument("memory parameters/provenance invalid");
    std::set<std::string> names;
    U total_lines=0,prev_line=0;
    for (const auto& l:c.caches) {
        if (!identifier(l.name) || l.name=="memory" || !names.insert(l.name).second || !l.lookup_cycles || l.provenance.empty())
            throw std::invalid_argument("cache name, lookup or provenance invalid");
        const auto g=l.geometry;
        if (!g.line_bytes || !g.sets || !g.ways) throw std::invalid_argument("zero cache geometry");
        total_lines=checked::add(total_lines,checked::mul(g.sets,g.ways));
        if (total_lines>1000000) throw std::length_error("combined cache state exceeds one million lines");
        (void)checked::mul(checked::mul(g.sets,g.ways),g.line_bytes);
        if (prev_line && g.line_bytes%prev_line!=0)
            throw std::invalid_argument("lower lines must be integer multiples of upper lines");
        prev_line=g.line_bytes;
        // Reuse only the existing input validator here, not its hit/miss logic.
        (void)analyze_reuse(g,{},l.initial);
    }
    if (c.inclusion=="inclusive") for (std::size_t i=0;i+1<c.caches.size();++i) {
        const auto& upper=c.caches[i]; const auto& lower=c.caches[i+1];
        for (const auto& set:upper.initial) for (auto line:set) {
            const auto parent=checked::mul(line,upper.geometry.line_bytes)/lower.geometry.line_bytes;
            if (lower.initial.empty()) throw std::invalid_argument("initial inclusion violated");
            const auto& target=lower.initial[parent%lower.geometry.sets];
            if (std::find(target.begin(),target.end(),parent)==target.end())
                throw std::invalid_argument("initial inclusion violated");
        }
    }
    const auto& s=c.stimulus;
    if (s.entry!="memory" && !names.contains(s.entry)) throw std::invalid_argument("unknown injection entry");
    if ((s.mode!="open_loop" && s.mode!="closed_loop") ||
        (s.engine!="finite" && s.engine!="periodic" && s.engine!="stable_blocks"))
        throw std::invalid_argument("unsupported stimulus mode/engine");
    if (!s.bytes || !s.address_period || !s.max_requests || s.max_requests>1000000 ||
        !s.max_transactions || s.max_transactions>10000000)
        throw std::invalid_argument("invalid stimulus sizes/budgets");
    if (s.engine=="finite" && s.count>s.max_requests) throw std::length_error("stimulus exceeds finite request budget");
    if (s.engine=="periodic" && (s.entry!="memory" || s.mode!="open_loop" || (s.stride && s.address_period!=1)))
        throw std::invalid_argument("periodic engine requires open-loop same-address direct memory stimulus");
    if (s.engine=="stable_blocks" && (s.mode!="closed_loop" || s.entry=="memory" || s.address_period>s.max_requests))
        throw std::invalid_argument("stable_blocks requires closed-loop cache stimulus with bounded block size");
    const U largest_index=s.count?std::min(s.count,s.address_period)-1:0;
    (void)checked::add(checked::add(s.address,checked::mul(largest_index,s.stride)),s.bytes-1);
    if (s.mode=="open_loop" && s.count)
        (void)checked::add(s.arrival_cycles,checked::mul(s.count-1,s.spacing_cycles));
}
std::string describe_config(const ModelConfig& c) {
    // Preserve every accepted field and its scope in a parseable effective config.
    std::ostringstream o;
    o<<"[model]\nversion=1\nclock_hz="<<c.clock_hz<<"\ninclusion="<<c.inclusion
     <<"\naccess=read_only\nprefetch=none\ncache_concurrency=blocking\n[memory]\npolicy=fifo\n"
     <<"first_beat_cycles="<<c.memory.first_beat_cycles<<"\nbeat_bytes="<<c.memory.beat_bytes
     <<"\nbeat_cycles="<<c.memory.beat_cycles<<"\nissue_cycles="<<c.memory.issue_cycles
     <<"\noutstanding="<<c.memory.outstanding<<"\nprovenance="<<c.memory.provenance<<'\n';
    for (const auto& l:c.caches) {
        o<<"[cache."<<l.name<<"]\nreplacement=lru\nline_bytes="<<l.geometry.line_bytes
         <<"\nsets="<<l.geometry.sets<<"\nways="<<l.geometry.ways<<"\nlookup_cycles="<<l.lookup_cycles
         <<"\nfill_cycles="<<l.fill_cycles<<"\nprovenance="<<l.provenance<<"\ninitial_lines=";
        bool empty=true;
        for (const auto& set:l.initial) for (auto line:set) { if (!empty) o<<','; o<<line; empty=false; }
        if (empty) o<<"empty";
        o<<'\n';
    }
    const auto& s=c.stimulus;
    o<<"[stimulus]\nentry="<<s.entry<<"\nmode="<<s.mode<<"\nengine="<<s.engine
     <<"\ncount="<<s.count<<"\naddress="<<s.address<<"\nbytes="<<s.bytes<<"\nstride="<<s.stride
     <<"\naddress_period="<<s.address_period<<"\narrival_cycles="<<s.arrival_cycles
     <<"\nspacing_cycles="<<s.spacing_cycles<<"\nmax_requests="<<s.max_requests
     <<"\nmax_transactions="<<s.max_transactions<<'\n';
    return o.str();
}
}
