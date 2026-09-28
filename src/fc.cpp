#include "edge/fc.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace edge {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
std::int32_t checked32(std::int64_t x) {
    require(x >= INT32_MIN && x <= INT32_MAX, "int32 arithmetic domain exceeded");
    return static_cast<std::int32_t>(x);
}
std::int64_t floor_div(std::int64_t x, std::int64_t d) {
    return x/d - (x%d < 0 ? 1 : 0);
}
void validate_quant(std::int32_t m, std::int32_t s) {
    require(m > 0 && s >= -30 && s <= 30, "unsupported multiplier/shift");
}
}
KernelIR derive_fc(FCSpec s, Layout l) {
    require(s.n > 0 && s.m > 0, "dimensions must be positive");
    validate_quant(s.multiplier, s.shift);
    require(s.input_offset >= -127 && s.input_offset <= 128, "invalid input offset");
    require(s.output_offset >= -128 && s.output_offset <= 127, "invalid output offset");
    require(s.activation_min >= -128 && s.activation_max <= 127 &&
            s.activation_min <= s.activation_max, "invalid activation bounds");
    const auto n=static_cast<std::uint64_t>(s.n), m=static_cast<std::uint64_t>(s.m);
    const std::array<std::uint64_t,4> bases{l.input,l.weights,l.bias,l.output};
    const std::array<std::uint64_t,4> sizes{n,n*m,4*m,m};
    // int32 dimensions bound products in uint64; each object must fit a 32-bit address space.
    for (unsigned i=0;i<4;++i) {
        require(bases[i] <= UINT32_MAX && bases[i]%16==0, "invalid/alignment-violating base");
        require(sizes[i] <= (UINT64_C(1)<<32)-bases[i], "target address overflow");
        for (unsigned j=0;j<i;++j)
            require(bases[i]+sizes[i]<=bases[j] || bases[j]+sizes[j]<=bases[i], "overlapping buffers");
    }
    return KernelIR(s,l);
}
WorkSummary count_work(const KernelIR& ir) {
    const auto n=static_cast<std::uint64_t>(ir.spec().n), m=static_cast<std::uint64_t>(ir.spec().m);
    const auto g=ir.pairs()+ir.odd_rows();
    return {n*m,2*m*ir.groups(),m*ir.tail(),m,n+n*m+5*m,
            g*n,m*n,4*m,m,g*n+m*n+5*m,(g+m)*(ir.groups()+ir.tail())+2*m};
}
std::vector<Event> enumerate_events(const KernelIR& ir, std::uint64_t limit) {
    require(count_work(ir).tensor_events <= limit, "enumeration limit exceeded; use analytical counts");
    std::vector<Event> out;
    const auto& l=ir.layout();
    const auto n=static_cast<std::uint64_t>(ir.spec().n);
    auto emit=[&](Tensor t,std::uint64_t a,std::uint32_t b,bool w=false){out.push_back({t,a,b,w});};
    auto region=[&](std::uint64_t row,std::uint64_t rows) {
        for(std::uint64_t j=0;j<rows;++j) emit(Tensor::bias,l.bias+4*(row+j),4);
        for(std::uint64_t g=0;g<ir.groups();++g) {
            emit(Tensor::input,l.input+4*g,4);
            for(std::uint64_t j=0;j<rows;++j) emit(Tensor::weights,l.weights+(row+j)*n+4*g,4);
        }
        for(std::uint64_t i=4*ir.groups();i<n;++i) {
            emit(Tensor::input,l.input+i,1);
            for(std::uint64_t j=0;j<rows;++j) emit(Tensor::weights,l.weights+(row+j)*n+i,1);
        }
        for(std::uint64_t j=0;j<rows;++j) emit(Tensor::output,l.output+row+j,1,true);
    };
    for(std::uint64_t p=0;p<ir.pairs();++p) region(2*p,2);
    if(ir.odd_rows()) region(2*ir.pairs(),1);
    return out;
}
std::int32_t requantize(std::int32_t value,std::int32_t multiplier,std::int32_t shift) {
    validate_quant(multiplier,shift);
    const auto v=checked32(static_cast<std::int64_t>(value)*(INT64_C(1)<<std::max(shift,0)));
    const auto h=floor_div(static_cast<std::int64_t>(v)*multiplier+(INT64_C(1)<<30),INT64_C(1)<<31);
    const auto divisor=INT64_C(1)<<std::max(-shift,0);
    const auto q=floor_div(h,divisor), remainder=h-q*divisor;
    return checked32(q+(remainder>((divisor-1)/2+(q<0)) ? 1 : 0));
}
std::vector<std::int8_t> evaluate(const KernelIR& ir,std::span<const std::int8_t> x,
        std::span<const std::int8_t> w,std::span<const std::int32_t> b) {
    const auto& s=ir.spec();
    const auto n=static_cast<std::size_t>(s.n),m=static_cast<std::size_t>(s.m);
    require(x.size()==n && w.size()==n*m && b.size()==m,"incorrect tensor extents");
    std::vector<std::int8_t> out(m);
    for(std::size_t j=0;j<m;++j) {
        std::int64_t acc=b[j], budget=std::abs(static_cast<std::int64_t>(b[j]));
        auto term=[&](std::size_t i) {
            const std::int64_t t=(static_cast<std::int64_t>(x[i])+s.input_offset)*w[j*n+i];
            budget+=std::abs(t);
            require(budget<=INT32_MAX,"conservative accumulator domain exceeded");
            acc+=t;
        };
        for(std::size_t i=0;i+4<=n;i+=4) {term(i+1);term(i+3);term(i);term(i+2);}
        for(std::size_t i=n-n%4;i<n;++i) term(i);
        require(budget<=INT32_MAX,"conservative accumulator domain exceeded");
        auto y=checked32(static_cast<std::int64_t>(requantize(checked32(acc),s.multiplier,s.shift))+s.output_offset);
        out[j]=static_cast<std::int8_t>(std::clamp(y,s.activation_min,s.activation_max));
    }
    return out;
}
}
