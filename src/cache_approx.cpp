#include "edge/cache_approx.hpp"
#include <cmath>
#include <map>
#include <stdexcept>

namespace edge {
namespace {
using U = std::uint64_t;
}
std::vector<U> reuse_distances(U line_bytes, std::span<const Event> events) {
    if (!line_bytes) throw std::invalid_argument("zero line size");
    std::vector<U> lines;
    for (const auto& e:events) {
        if (e.write) throw std::invalid_argument("cache writes unsupported");
        for (U j=0;j<contiguous_lines(e.address,e.bytes,line_bytes);++j) lines.push_back(e.address/line_bytes+j);
    }
    // Fenwick tree marks each line's latest position; the distance is the number of marks
    // strictly after the previous position (distinct lines since then), O(R log R).
    const U R=lines.size();
    std::vector<U> tree(R+1,0), out(R,no_reuse);
    auto add=[&](U i,int v){ for (++i;i<=R;i+=i&(~i+1)) tree[i]+=static_cast<U>(v); };
    auto prefix=[&](U i){ U s=0; for (;i>0;i-=i&(~i+1)) s+=tree[i]; return s; };  // marks in [0, i)
    std::map<U,U> last;
    for (U i=0;i<R;++i) {
        const auto it=last.find(lines[i]);
        if (it!=last.end()) {
            out[i]=prefix(i)-prefix(it->second+1);
            add(it->second,-1);
        }
        add(i,1);
        last[lines[i]]=i;
    }
    return out;
}
double random_set_hit_probability(U d, U sets, U ways) {
    if (!sets || !ways) throw std::invalid_argument("zero cache dimension");
    if (sets==1) return d<ways ? 1.0 : 0.0;
    // Sum of binomial terms k < W, each from logs to stay finite for large D.
    const double p=1.0/static_cast<double>(sets), lp=std::log(p), lq=std::log1p(-p);
    double sum=0;
    for (U k=0;k<ways && k<=d;++k) {
        const double dk=static_cast<double>(d), kk=static_cast<double>(k);
        sum+=std::exp(std::lgamma(dk+1)-std::lgamma(kk+1)-std::lgamma(dk-kk+1)+kk*lp+(dk-kk)*lq);
    }
    return std::min(sum,1.0);
}
ApproxCounts approximate_misses(CacheGeometry g, std::span<const Event> call, U call_index, U limit) {
    if (!g.line_bytes || !g.sets || !g.ways) throw std::invalid_argument("zero cache dimension");
    if (call_index>1) throw std::invalid_argument("call_index must be 0 (cold) or 1 (warm)");
    std::vector<Event> stream(call.begin(),call.end());
    if (call_index) stream.insert(stream.end(),call.begin(),call.end());
    const auto d=reuse_distances(g.line_bytes,stream);
    if (d.size()>limit) throw std::length_error("approximation expansion exceeds limit");
    ApproxCounts out;
    const U from=call_index ? d.size()/2 : 0;
    for (U i=from;i<d.size();++i) {
        ++out.references;
        if (d[i]==no_reuse) { out.fully_associative_misses+=1; out.random_set_misses+=1; continue; }
        if (d[i]>=g.sets*g.ways) out.fully_associative_misses+=1;
        out.random_set_misses+=1-random_set_hit_probability(d[i],g.sets,g.ways);
    }
    return out;
}
}
