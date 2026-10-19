#include "edge/cluster.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace edge {
namespace {
using U=std::uint64_t;
const char* port_name(noc::Port p) {
    switch (p) {
    case noc::Port::local: return "local";
    case noc::Port::minus_x: return "-x";
    case noc::Port::plus_x: return "+x";
    case noc::Port::minus_y: return "-y";
    case noc::Port::plus_y: return "+y";
    }
    return "?";
}
long double ratio(U num,U den) { return den?static_cast<long double>(num)/den:0; }
}
std::string report_cluster(const ClusterConfig& c,const ClusterResult& r) {
    const noc::Topology topo(c.topology);
    std::ostringstream o;
    o<<std::setprecision(12)
     <<"classification=exact_under_configured_abstraction; synthetic_cluster; hardware_unvalidated\n"
     <<"engine="<<c.stimulus.engine<<"\ntopology="<<c.topology.kind<<' '<<c.topology.k<<'x'<<c.topology.k
     <<"\ncores="<<c.cores.size()<<"\nevaluated_reads="<<r.reads.size()
     <<"\nskipped_reads="<<r.skipped_reads<<"\nskipped_periods="<<r.skipped_periods
     <<"\nskipped_cycles="<<r.skipped_cycles<<"\nevents="<<r.events
     <<"\ntransactions="<<r.transactions<<'\n';
    // Makespan T: last completion over all cores. Every utilization below is busy/T over
    // [0, T]; it is a ratio of exact counts, not a steady-state estimate.
    U makespan=0;
    for (const auto& s:r.cores) makespan=std::max(makespan,s.last_done);
    o<<"makespan_cycles="<<makespan<<'\n';

    // Per-core view. Latency = completion - issue of each read; hops_to_mc is the XY
    // distance from the core's tile to the memory controller tile (design §4.1).
    o<<"core,tile,x,y,hops_to_mc,reads,mean_latency,min_latency,max_latency,last_done,"
       "l1_hits,l1_misses,l1_evictions,reads_per_kcycle\n";
    // Jain's index J = (sum x)^2 / (n sum x^2) over per-core throughput
    // x_p = reads_p / (last_done_p - start_p). J = 1 means equal service; 1/n is the
    // worst case. Cores with an empty window are excluded.
    long double sum=0,sum_sq=0,lat_min=0,lat_max=0;
    std::size_t counted=0;
    for (std::size_t p=0;p<c.cores.size();++p) {
        const auto& k=c.cores[p];
        const auto& s=r.cores[p];
        const auto at=topo.coord(k.tile);
        const U window=s.last_done>k.start_cycles?s.last_done-k.start_cycles:0;
        const auto mean=ratio(s.latency_sum,s.reads);
        const auto rate=ratio(s.reads,window);
        o<<k.name<<','<<k.tile<<','<<at.x<<','<<at.y<<','<<topo.hops(k.tile,c.topology.mc_tile)<<','
         <<s.reads<<','<<mean<<','<<s.min_latency<<','<<s.max_latency<<','<<s.last_done<<','
         <<s.l1.hits<<','<<s.l1.misses<<','<<s.l1.evictions<<','<<rate*1000<<'\n';
        if (window) {
            sum+=rate; sum_sq+=rate*rate; ++counted;
            lat_min=counted==1?mean:std::min(lat_min,mean);
            lat_max=counted==1?mean:std::max(lat_max,mean);
        }
    }
    o<<"fairness_jain_throughput="<<(sum_sq?sum*sum/(counted*sum_sq):0)
     <<"\nfairness_mean_latency_max_over_min="<<(lat_min?lat_max/lat_min:0)<<'\n';

    // Per-bank view. Bank b lives on tile b; wait = grant - arrival summed over grants.
    o<<"bank,x,y,references,hits,misses,evictions,grants,wait_cycles,busy_cycles,utilization\n";
    for (U b=0;b<r.banks.size();++b) {
        const auto& s=r.banks[b];
        const auto at=topo.coord(b);
        o<<b<<','<<at.x<<','<<at.y<<','<<s.l2.references<<','<<s.l2.hits<<','<<s.l2.misses<<','
         <<s.l2.evictions<<','<<s.grants<<','<<s.wait_cycles<<','<<s.busy_cycles<<','
         <<ratio(s.busy_cycles,makespan)<<'\n';
    }
    o<<"memory_requests="<<r.memory_requests<<"\nmemory_wire_bytes="<<r.memory_wire_bytes
     <<"\nmemory_wait_cycles="<<r.memory_wait_cycles<<'\n';

    // Per-link view: only links that carried at least one packet. A k×k grid has up to
    // 4k^2 directed links, most of them idle for a given stimulus.
    o<<"link,from_tile,from_x,from_y,port,to_tile,packets,flits,busy_cycles,wait_cycles,utilization\n";
    U used=0,busiest=0,busiest_index=0;
    for (U tile=0;tile<topo.tiles();++tile) for (std::size_t port=1;port<noc::port_count;++port) {
        const noc::Link l{tile,static_cast<noc::Port>(port)};
        if (!topo.has_link(tile,l.out)) continue;
        const auto index=topo.link_index(l);
        const auto& s=r.links[index];
        if (!s.packets) continue;
        ++used;
        if (s.busy_cycles>busiest) { busiest=s.busy_cycles; busiest_index=index; }
        const auto at=topo.coord(tile);
        o<<index<<','<<tile<<','<<at.x<<','<<at.y<<','<<port_name(l.out)<<','<<topo.neighbor(tile,l.out)<<','
         <<s.packets<<','<<s.flits<<','<<s.busy_cycles<<','<<s.wait_cycles<<','<<ratio(s.busy_cycles,makespan)<<'\n';
    }
    o<<"links_used="<<used<<"\nbusiest_link="<<(used?std::to_string(busiest_index):"none")
     <<"\nbusiest_link_utilization="<<ratio(busiest,makespan)<<'\n';

    o<<"final_cache_state_MRU_to_LRU\n";
    auto dump=[&](const std::string& name,const CacheState& state) {
        for (std::size_t s=0;s<state.size();++s) {
            if (state[s].empty()) continue; // Empty sets omitted; every set starts empty.
            o<<name<<".set"<<s<<'=';
            for (auto line:state[s]) o<<line<<',';
            o<<'\n';
        }
    };
    for (std::size_t p=0;p<r.l1_state.size();++p) dump(c.cores[p].name+".l1",r.l1_state[p]);
    for (std::size_t b=0;b<r.l2_state.size();++b) dump("bank"+std::to_string(b)+".l2",r.l2_state[b]);
    o<<"memory_next_issue_cycles="<<r.memory_state.next_issue<<"\nmemory_pending_completion_cycles=";
    for (auto done:r.memory_state.pending) o<<done<<',';
    o<<"\n# Effective configuration and parameter provenance\n"<<describe_cluster_config(c);
    return o.str();
}
}
