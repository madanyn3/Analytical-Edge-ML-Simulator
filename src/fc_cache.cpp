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
struct Window { Key first, last; };
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
    // Window [first, last] of weight line x. Its bytes split into at most three cursor
    // pieces (row 2p by A, row 2p+1 by B, next region's row by A); recency is monotone in
    // the byte within a piece, so each piece contributes its first and last byte. This makes
    // the middle line's last reference A(K-1), after B has left it.
    Window weight_window(U x) const {
        const U hi=std::min((x+1)*L-1,w+m*n-1)-w;
        Window win{{~U{0},0,0,0},{0,0,0,0}};
        for (U z=std::max(x*L,w)-w;z<=hi;) {
            const U p=std::min(z/(2*n),pairs), o=z-2*p*n;
            const U end=std::min(hi,2*p*n+(rows(p)==2 && o<n ? n : rows(p)*n)-1);
            win.first=std::min(win.first,byte_key(z,x));
            win.last=std::max(win.last,byte_key(end,x));
            z=end+1;
        }
        return win;
    }
    Window bias_window(U y) const {
        auto key=[&](U z){ return Key{std::min(z/8,pairs),0,0,y}; };
        return {key(std::max(y*L,b)-b),key(std::min((y+1)*L-1,b+4*m-1)-b)};
    }
    // Weight lines split into runs on which both window ends are non-decreasing in the line:
    // run j holds row 2j-1's lines (cursor B of pair j-1) then row 2j's (cursor A of region
    // j), between the lines s_j = floor((B_j + n)/L) where row 2j+1 starts. When that line is
    // shared with row 2j (the middle line M_j) it is excluded and handled on its own.
    U split(U j) const { return line(w+2*j*n+n); }
    bool shared(U j) const { return (w+2*j*n+n)%L; }
    std::pair<U,U> run(U j) const {
        const U first=j ? split(j-1)+shared(j-1) : line(w);
        const U last=j<pairs ? split(j)-1 : line(w+m*n-1);
        return {first,last};
    }
    // #{y in [a, b], y ≡ r (mod S) : first_y > first_x and last_y < last_x}, for a run on
    // which both window ends are non-decreasing in y: the qualifying y form an interval.
    template<class F> U inside(U a, U b, const Window& x, U r, F window) const {
        if (a>b) return 0;
        U lo=a, hi=b+1;   // Smallest y with first_y > first_x (b+1 if none).
        while (lo<hi) { const U mid=lo+(hi-lo)/2; if (window(mid).first>x.first) hi=mid; else lo=mid+1; }
        const U ya=lo;
        lo=a; hi=b+1;     // Smallest y with last_y >= last_x; yb is the line before it.
        while (lo<hi) { const U mid=lo+(hi-lo)/2; if (window(mid).last>=x.last) hi=mid; else lo=mid+1; }
        return lo>ya ? congruent(ya,lo-1,r) : 0;
    }
    // nested(x): lines of x's set whose windows lie strictly inside x's window. Such lines
    // are referenced only in the regions x's window spans, i.e. in runs first..last+1, the
    // middle lines of regions first..last, and the bias run.
    U nested(const Window& x, U line_x) const {
        const U r=line_x%S;
        U count=0;
        auto ww=[&](U y){ return weight_window(y); };
        for (U j=x.first[0];j<=std::min(x.last[0]+1,pairs);++j) {
            const auto [a,b2]=run(j);
            count+=inside(a,b2,x,r,ww);
        }
        for (U j=x.first[0];j<=x.last[0] && j<pairs;++j) {
            if (!shared(j) || split(j)%S!=r || split(j)==line_x) continue;
            const auto y=weight_window(split(j));
            if (x.first<y.first && y.last<x.last) ++count;
        }
        count+=inside(line(b),line(b+4*m-1),x,r,[&](U y){ return bias_window(y); });
        return count;
    }
};
// Largest x <= hi with x ≡ r (mod S), if it is >= lo.
bool top(U lo, U hi, U r, U S, U& x) {
    const U d=(hi%S+S-r)%S;
    if (hi<d || hi-d<lo) return false;
    x=hi-d;
    return true;
}

// Validates the supported domain and builds the stream description.
Stream make_stream(CacheGeometry g, const KernelIR& ir) {
    if (!g.line_bytes || !g.sets || !g.ways) throw std::invalid_argument("zero cache dimension");
    if (checked_mul(g.sets,g.ways)>1000000) throw std::length_error("cache state exceeds supported budget");
    const U n=static_cast<U>(ir.spec().n), m=static_cast<U>(ir.spec().m);
    const U L=g.line_bytes, S=g.sets, W=g.ways;
    (void)checked_mul(checked_mul(S,L),4);
    if (n<checked_add(checked_mul(2,L),8)) throw std::invalid_argument("unsupported FC cache case: n < 2L + 8");
    const U w=ir.layout().weights, b=ir.layout().bias;
    const U w_end=checked_add(w,checked_mul(m,n))-1, b_end=checked_add(b,4*m)-1;
    if (!(w_end/L<b/L || b_end/L<w/L)) throw std::invalid_argument("unsupported FC cache case: weight and bias lines overlap");
    return Stream{L,S,W,n,m,ir.pairs(),ir.groups(),ir.groups()+ir.tail(),w,b};
}
}

FcFlashCounts analyze_fc_flash(CacheGeometry g, const KernelIR& ir) {
    const Stream st=make_stream(g,ir);
    const U n=st.n, m=st.m, L=st.L, S=st.S, W=st.W, w=st.w, b=st.b;
    const U w_end=w+m*n-1, b_end=b+4*m-1;

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
                const Key k=st.weight_window(x).last;
                if (k[0]<region) { region=k[0]; later=cand.size(); }
                if (later>=W) break;
                cand.push_back(k);
                if (x<w/L+S) break;
                x-=S;
            }
        }
        if (top(b/L,b_end/L,set,S,x)) for (U j=0;j<W;++j) {
            cand.push_back(st.bias_window(x).last);
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

RepeatedCounts analyze_fc_flash_repeated(CacheGeometry g, const KernelIR& ir, std::uint64_t calls) {
    const Stream st=make_stream(g,ir);
    RepeatedCounts out;
    if (!calls) { out.total.final_state.resize(g.sets); return out; }
    const auto cold=analyze_fc_flash(g,ir);
    out.first=cold.counts;
    out.re_misses=cold.cursor_re_misses+cold.middle_re_misses+cold.boundary_re_misses+cold.bias_re_misses;
    // Theorem 2.2: a warm first reference to x misses iff (k_s - 1) - nested(x) >= W.
    const U S=st.S, SL=st.S*st.L, n=st.n;
    const U w0=st.line(st.w), w1=st.line(st.w+st.m*n-1), b0=st.line(st.b), b1=st.line(st.b+4*st.m-1);
    std::vector<U> k(S);
    for (U s=0;s<S;++s) k[s]=st.congruent(w0,w1,s)+st.congruent(b0,b1,s);
    auto misses=[&](U x, const Window& win){ return k[x%S]-1-st.nested(win,x)>=st.W; };
    // Weight lines by home region (the region of their first byte). Their windows and every
    // window nested in them lie in regions p..p+2, so the count depends only on B_p and
    // bias_p mod SL, the regions remaining after p (clamped at 3) and p > 0 (Lemma 3.4).
    const U regions=st.pairs+ir.odd_rows();
    auto home=[&](U p){ return p ? (st.w+2*p*n+st.L-1)/st.L : w0; };
    std::map<std::array<U,4>,U> memo;
    for (U p=0;p<regions;++p) {
        const std::array<U,4> key{(st.w+2*p*n)%SL,(st.b+8*p)%SL,std::min(regions-p,U{3}),p>0};
        auto it=memo.find(key);
        if (it==memo.end()) {
            U c=0;
            const U last=p+1<regions ? home(p+1)-1 : w1;
            for (U x=home(p);x<=last;++x) c+=misses(x,st.weight_window(x));
            it=memo.emplace(key,c).first;
        }
        out.warm_first_reference_misses+=it->second;
    }
    for (U y=b0;y<=b1;++y) out.warm_first_reference_misses+=misses(y,st.bias_window(y));

    auto& s=out.steady;
    s.references=out.first.references;
    s.misses=checked_add(out.warm_first_reference_misses,out.re_misses);
    s.hits=s.references-s.misses;
    s.evictions=s.misses;   // Corollary 2.3: occupancy is constant in a warm call.
    s.fill_bytes=checked_mul(s.misses,st.L);
    s.final_state=out.first.final_state;   // Theorem 2.1: the cold exit state is the fixed point.
    const U later=calls-1;
    auto& t=out.total;
    t.references=checked_add(out.first.references,checked_mul(later,s.references));
    t.hits=checked_add(out.first.hits,checked_mul(later,s.hits));
    t.misses=checked_add(out.first.misses,checked_mul(later,s.misses));
    t.evictions=checked_add(out.first.evictions,checked_mul(later,s.evictions));
    t.fill_bytes=checked_add(out.first.fill_bytes,checked_mul(later,s.fill_bytes));
    t.final_state=out.first.final_state;
    return out;
}
}
