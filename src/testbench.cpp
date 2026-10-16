#include "edge/testbench.hpp"
#include "edge/checked.hpp"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace edge {
namespace {
using U=std::uint64_t;
using checked::add;
using checked::mul;
std::string trim(std::string s) {
    const auto a=s.find_first_not_of(" \t\r");
    return a==std::string::npos?"":s.substr(a,s.find_last_not_of(" \t\r")-a+1);
}
}
BenchResult run_testbench(const ModelConfig& c,const std::vector<Request>* reads) {
    validate_config(c);
    const auto& s=c.stimulus;
    if (reads && (s.engine!="finite" || s.mode!="open_loop" || reads->size()>s.max_requests))
        throw std::invalid_argument("explicit reads require finite open_loop within request budget");
    BenchResult out;
    out.first_arrival=reads?(reads->empty()?0:reads->front().arrival):s.arrival_cycles;
    if (!reads && s.engine=="periodic") {
        const auto p=periodic_memory(c.memory,{s.arrival_cycles,s.address,s.bytes},s.count,s.spacing_cycles);
        out.requests=p.requests; out.requested_bytes=p.requested_bytes;
        out.last_done=p.last_done; out.min_latency=p.min_latency; out.max_latency=p.max_latency;
        out.latency_sum=p.mean_latency*p.requests;
        out.skipped_requests=p.requests;
        out.stats.levels.resize(c.caches.size());
        out.stats.memory_requests=p.requests; out.stats.memory_requested_bytes=p.requested_bytes;
        out.stats.memory_wire_bytes=p.wire_bytes; out.memory_state=p.final_state;
        for (const auto& l:c.caches) {
            auto state=l.initial; if (state.empty()) state.resize(l.geometry.sets);
            out.cache_state.push_back(std::move(state));
        }
        return out;
    }
    Hierarchy model(c);
    const U count=reads?reads->size():s.count;
    U i=0,arrival=s.arrival_cycles;
    while (i<count) {
        // Acceleration is only offered for complete repeated address blocks with
        // closed-loop arrivals. Open-loop queue growth is NOT assumed periodic.
        const bool candidate=!reads && s.engine=="stable_blocks" && i%s.address_period==0 &&
                             count-i>=s.address_period;
        const U block=candidate?s.address_period:1;
        std::vector<U> before_key;
        const U before_time=arrival;
        const auto before_stats=model.stats();
        const auto before_latency=out.latency_sum;
        if (candidate) before_key=model.boundary_key(arrival);
        for (U j=0;j<block;++j) {
            if (out.evaluated_requests==s.max_requests) throw std::length_error("no proven acceleration before finite request budget");
            Request r;
            if (reads) r=(*reads)[i];
            else {
                if (s.mode=="open_loop") arrival=add(s.arrival_cycles,mul(i,s.spacing_cycles));
                r={arrival,add(s.address,mul(i%s.address_period,s.stride)),s.bytes};
            }
            const auto done=model.submit(s.entry,r);
            const U latency=done.done-r.arrival;
            out.min_latency=out.requests?std::min(out.min_latency,latency):latency;
            out.max_latency=std::max(out.max_latency,latency);
            out.latency_sum+=latency;
            out.last_done=std::max(out.last_done,done.done);
            out.requested_bytes=add(out.requested_bytes,r.bytes);
            ++out.requests; ++out.evaluated_requests; ++i;
            out.evaluated.push_back(done);
            if (!reads && s.mode=="closed_loop") arrival=add(done.done,s.spacing_cycles);
        }
        if (candidate && model.boundary_key(arrival)==before_key) {
            const U copies=(count-i)/block;
            if (!copies) continue;
            const U elapsed=arrival-before_time;
            const U skipped=mul(copies,block);
            const U delta=mul(copies,elapsed);
            // Equality includes MRU order at every level and residual service issue
            // readiness. Determinism + identical future stimulus proves by induction
            // that counters and elapsed time repeat. This is not a hit-rate heuristic.
            model.skip_fixed_blocks(arrival,elapsed,before_stats,copies);
            out.latency_sum+=(out.latency_sum-before_latency)*copies;
            out.requests=add(out.requests,skipped);
            out.skipped_requests=add(out.skipped_requests,skipped);
            out.requested_bytes=add(out.requested_bytes,mul(skipped,s.bytes));
            out.last_done=add(out.last_done,delta);
            arrival=add(arrival,delta);
            i=add(i,skipped);
        }
    }
    out.stats=model.stats(); out.cache_state=model.cache_state(); out.memory_state=model.memory_state();
    return out;
}
std::vector<Request> parse_stimulus(std::string_view text,const std::string& source,U limit) {
    if (text.size()>64*1024*1024) throw std::length_error("stimulus exceeds 64 MiB");
    std::istringstream input{std::string(text)};
    std::string row;
    if (!std::getline(input,row) || trim(row)!="arrival_cycles,address,bytes")
        throw std::invalid_argument(source+":1: expected arrival_cycles,address,bytes header");
    std::vector<Request> out;
    U line=1;
    while (std::getline(input,row)) {
        ++line;
        if (trim(row).empty()) continue;
        if (row.size()>4096 || out.size()>=limit) throw std::length_error(source+": stimulus budget exceeded");
        U fields[3]{};
        std::istringstream tokens(row);
        for (auto& value:fields) {
            std::string token;
            if (!std::getline(tokens,token,',')) throw std::invalid_argument(source+":"+std::to_string(line)+": missing field");
            token=trim(token);
            const bool hex=token.starts_with("0x");
            const auto* first=token.data()+(hex?2:0);
            const auto* last=token.data()+token.size();
            const auto parsed=std::from_chars(first,last,value,hex?16:10);
            if (first==last || parsed.ec!=std::errc{} || parsed.ptr!=last)
                throw std::invalid_argument(source+":"+std::to_string(line)+": invalid unsigned integer");
        }
        std::string extra;
        if (row.back()==',' || std::getline(tokens,extra,',')) throw std::invalid_argument(source+":"+std::to_string(line)+": extra field");
        if (!fields[2] || (!out.empty() && fields[0]<out.back().arrival)) throw std::invalid_argument(source+":"+std::to_string(line)+": invalid size/arrival order");
        (void)add(fields[1],fields[2]-1);
        out.push_back({fields[0],fields[1],fields[2]});
    }
    return out;
}
std::string report_testbench(const ModelConfig& c,const BenchResult& r) {
    std::ostringstream o;
    o<<std::setprecision(12)<<"classification=exact_under_configured_abstraction; hardware_unvalidated\n"
     <<"engine="<<c.stimulus.engine<<"\nentry="<<c.stimulus.entry<<"\nrequests="<<r.requests
     <<"\nevaluated_requests="<<r.evaluated_requests<<"\nanalytically_skipped_requests="<<r.skipped_requests
     <<"\nrequested_bytes="<<r.requested_bytes<<"\nlast_completion_cycles="<<r.last_done
     <<"\nmin_latency_cycles="<<r.min_latency<<"\nmax_latency_cycles="<<r.max_latency
     <<"\nmean_latency_cycles="<<(r.requests?r.latency_sum/r.requests:0)<<'\n';
    const U duration=r.requests?r.last_done-r.first_arrival:0;
    o<<"duration_cycles="<<duration<<"\nrequested_bytes_per_second_including_drain="
     <<(duration?static_cast<long double>(r.requested_bytes)*c.clock_hz/duration:0)<<'\n';
    o<<"memory_requests="<<r.stats.memory_requests<<"\nmemory_requested_bytes="<<r.stats.memory_requested_bytes
     <<"\nmemory_wire_bytes="<<r.stats.memory_wire_bytes<<"\nlevel,references,hits,misses,evictions,invalidations,fill_bytes\n";
    for (std::size_t i=0;i<r.stats.levels.size();++i) {
        const auto& x=r.stats.levels[i];
        o<<c.caches[i].name<<','<<x.references<<','<<x.hits<<','<<x.misses<<','<<x.evictions<<','<<x.invalidations<<','<<x.fill_bytes<<'\n';
    }
    o<<"evaluated_arrival_cycles,start_cycles,completion_cycles,memory_wire_bytes\n";
    for (const auto& x:r.evaluated) o<<x.arrival<<','<<x.start<<','<<x.done<<','<<x.wire_bytes<<'\n';
    o<<"final_cache_state_MRU_to_LRU\n";
    for (std::size_t i=0;i<r.cache_state.size();++i) for (std::size_t s=0;s<r.cache_state[i].size();++s) {
        o<<c.caches[i].name<<".set"<<s<<'=';
        for (auto line:r.cache_state[i][s]) o<<line<<',';
        o<<'\n';
    }
    o<<"memory_next_issue_cycles="<<r.memory_state.next_issue<<"\nmemory_pending_completion_cycles=";
    for (auto done:r.memory_state.pending) o<<done<<',';
    o<<"\nmemory_last_offered_arrival_cycles="<<r.memory_state.last_arrival
     <<"\n# Effective configuration and parameter provenance\n"<<describe_config(c);
    return o.str();
}
}
