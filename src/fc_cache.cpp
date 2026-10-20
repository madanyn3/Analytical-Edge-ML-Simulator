#include "edge/fc_cache.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>

namespace edge {
namespace {
using U = std::uint64_t;
using Key = std::array<U,4>;   // (region, slot, cursor, line); slot 0 = bias, 1+k = access k.
U checked_add(U a, U b) {
    if (b > std::numeric_limits<U>::max()-a) throw std::overflow_error("address overflow");
    return a+b;
}
U checked_mul(U a, U b) {
    if (a && b > std::numeric_limits<U>::max()/a) throw std::overflow_error("count overflow");
    return a*b;
}
struct Contribution { U references{}, cursor{}, middle{}, boundary{}, bias{}; };

// Region p covers rows 2p, 2p+1 (or the final odd row). Row-relative byte offset o lies in
// access k(o); access k starts at t(k) and has s(k) bytes (groups of 4, then 1-byte tails).
// Each region issues its bias words, then A(0) B(0) A(1) B(1) ..., where A reads row 2p at
// B_p + t(k) and B reads row 2p+1 at B_p + n + t(k) (input reads are SRAM, not cached).
struct Stream {
    U L, S, W, n, m, pairs, G, K, w, b;
    U t(U k) const { return k<G ? 4*k : 4*G+(k-G); }
    U s(U k) const { return k<G ? 4 : 1; }
    U access(U o) const { return o<4*G ? o/4 : G+(o-4*G); }
    U line(U a) const { return a/L; }
    U rows(U p) const { return p<pairs ? 2 : 1; }
    // #{y in [lo, hi] : y ≡ r (mod S)}.
    U congruent(U lo, U hi, U r) const {
        if (lo>hi) return 0;
        auto upto=[&](U z){ return z>=r ? (z-r)/S+1 : U{0}; };
        return upto(hi)-(lo ? upto(lo-1) : 0);
    }
    // Lines of [a, a+bytes) in the set of line x.
    U same_set(U a, U bytes, U x) const { return congruent(line(a),line(a+bytes-1),x%S); }
    // Sum of f(k) over k < count when f has period P (shifting every address by a multiple
    // of S*L shifts every line by a multiple of S, which changes no set or line offset).
    template<class F> U periodic(U count, U P, F f) const {
        U sum=0;
        for (U r=0;r<std::min(count,P);++r) sum+=f(r)*((count-r+P-1)/P);
        return sum;
    }
    Contribution region(U p) const {
        Contribution c;
        const U rows_=rows(p), B=w+2*p*n, bp=b+8*p;
        const U P=S*L/std::gcd(S*L,U{4});   // Period of group-access behaviour in k.
        for (U i=0;i<rows_;++i) c.references+=contiguous_lines(bp+4*i,4,L);
        auto refs=[&](U k){ U r=0; for (U j=0;j<rows_;++j) r+=contiguous_lines(B+j*n+t(k),s(k),L); return r; };
        c.references+=periodic(G,P,refs);
        for (U k=G;k<K;++k) c.references+=refs(k);
        if (rows_==2) {
            // Cursor continuation k -> k+1: A(k+1) starts inside A(k)'s last line x, and
            // only B(k) intervenes (B(k)'s lines exclude x since n >= L + 4). Symmetrically
            // B(k+1) continues B(k)'s line with only A(k+1) in between.
            auto cont=[&](U k) {
                U miss=0;
                const U a1=B+t(k+1), b1=B+n+t(k+1);
                if (a1%L && same_set(B+n+t(k),s(k),line(a1))>=W) ++miss;
                if (b1%L && same_set(a1,s(k+1),line(b1))>=W) ++miss;
                return miss;
            };
            const U group_steps=G ? G-1 : 0;
            c.cursor+=periodic(group_steps,P,cont);
            for (U k=group_steps;k+1<K;++k) c.cursor+=cont(k);
            // Middle line M shared by the rows. B touches it in B(0..k*) (k* holds M's last
            // byte of row j+1), A first touches it in A(ka) (ka holds M's first byte). In
            // between, A scans [lo, M-1] and B scans [M+1, hi]: the gap is a line range.
            if ((B+n)%L) {
                const U M=line(B+n);
                const U kstar=access((M+1)*L-1-(B+n)), ka=access(M*L-B);
                if (ka<=kstar) throw std::logic_error("middle-line order violates the domain");
                const U lo=line(B+t(kstar+1)), hi=line(B+n+t(ka-1)+s(ka-1)-1);
                if (congruent(lo,hi,M%S)-1>=W) ++c.middle;
            }
        }
        if (p>0) {
            // Boundary line X = last line of the previous region's row 2p-1 (touched last by
            // its B cursor) = first line of A(0); only this region's bias lines intervene.
            if (B%L && same_set(bp,4*rows_,line(B))>=W) ++c.boundary;
            // Bias line Y continued from the previous region: its whole weight range intervenes.
            if (bp%L && same_set(B-2*n,2*n,line(bp))>=W) ++c.bias;
        }
        return c;
    }
    // Recency of a weight byte at offset z from w.
    Key byte_key(U z, U x) const {
        const U p=std::min(z/(2*n),pairs), o=z-2*p*n;
        if (rows(p)==2 && o>=n) return {p,1+access(o-n),1,x};
        return {p,1+access(o),0,x};
    }
    // Last reference of weight line x: its last byte, except the middle line, whose bytes in
    // row j are read by A(K-1) after B has left it.
    Key weight_key(U x) const {
        const U first=std::max(x*L,w)-w, z=std::min((x+1)*L-1,w+m*n-1)-w;
        Key k=byte_key(z,x);
        if (k[2]==1) {
            const U za=2*k[0]*n+n-1;
            if (first<=za) k=std::max(k,byte_key(za,x));
        }
        return k;
    }
    Key bias_key(U y) const {
        const U z=std::min((y+1)*L-1,b+4*m-1)-b;
        return {std::min(z/8,pairs),0,0,y};
    }
};
// Largest x <= hi with x ≡ r (mod S), if it is >= lo.
bool top(U lo, U hi, U r, U S, U& x) {
    const U d=(hi%S+S-r)%S;
    if (hi<d || hi-d<lo) return false;
    x=hi-d;
    return true;
}
}

FcFlashCounts analyze_fc_flash(CacheGeometry g, const KernelIR& ir) {
    if (!g.line_bytes || !g.sets || !g.ways) throw std::invalid_argument("zero cache dimension");
    if (checked_mul(g.sets,g.ways)>1000000) throw std::length_error("cache state exceeds supported budget");
    const U n=static_cast<U>(ir.spec().n), m=static_cast<U>(ir.spec().m);
    const U L=g.line_bytes, S=g.sets, W=g.ways;
    (void)checked_mul(checked_mul(S,L),4);
    if (n<checked_add(checked_mul(2,L),8)) throw std::invalid_argument("unsupported FC cache case: n < 2L + 8");
    const U w=ir.layout().weights, b=ir.layout().bias;
    const U w_end=checked_add(w,checked_mul(m,n))-1, b_end=checked_add(b,4*m)-1;
    if (!(w_end/L<b/L || b_end/L<w/L)) throw std::invalid_argument("unsupported FC cache case: weight and bias lines overlap");
    const Stream st{L,S,W,n,m,ir.pairs(),ir.groups(),ir.groups()+ir.tail(),w,b};

    // A region's contribution depends only on (B_p mod SL, bias_p mod SL, rows, p > 0).
    FcFlashCounts out;
    std::map<std::array<U,4>,Contribution> memo;
    const U SL=S*L, regions=st.pairs+ir.odd_rows();
    for (U p=0;p<regions;++p) {
        const std::array<U,4> key{(w+2*p*n)%SL,(b+8*p)%SL,st.rows(p),p>0};
        auto it=memo.find(key);
        if (it==memo.end()) it=memo.emplace(key,st.region(p)).first;
        const auto& c=it->second;
        out.counts.references=checked_add(out.counts.references,c.references);
        out.cursor_re_misses+=c.cursor; out.middle_re_misses+=c.middle;
        out.boundary_re_misses+=c.boundary; out.bias_re_misses+=c.bias;
    }
    out.distinct_lines=contiguous_lines(w,m*n,L)+contiguous_lines(b,4*m,L);
    out.counts.misses=out.distinct_lines+out.cursor_re_misses+out.middle_re_misses+
                      out.boundary_re_misses+out.bias_re_misses;
    out.counts.hits=out.counts.references-out.counts.misses;
    out.counts.fill_bytes=checked_mul(out.counts.misses,L);

    // Final state (Lemma 1.2): per set, the W lines with the latest last reference. Region
    // indices of weight keys do not decrease with the line number, so scanning a set's lines
    // downwards may stop once W candidates come from regions later than the current one.
    out.counts.final_state.resize(S);
    U resident=0;
    for (U set=0;set<S;++set) {
        std::vector<Key> cand;
        U x;
        if (top(w/L,w_end/L,set,S,x)) {
            U region=std::numeric_limits<U>::max(), later=0;
            for (;;) {
                const Key k=st.weight_key(x);
                if (k[0]<region) { region=k[0]; later=cand.size(); }
                if (later>=W) break;
                cand.push_back(k);
                if (x<w/L+S) break;
                x-=S;
            }
        }
        if (top(b/L,b_end/L,set,S,x)) for (U j=0;j<W;++j) {
            cand.push_back(st.bias_key(x));
            if (x<b/L+S) break;
            x-=S;
        }
        std::sort(cand.begin(),cand.end(),std::greater<>());
        if (cand.size()>W) cand.resize(W);
        for (const auto& k:cand) out.counts.final_state[set].push_back(k[3]);
        resident+=cand.size();
    }
    out.counts.evictions=out.counts.misses-resident;
    return out;
}
}
