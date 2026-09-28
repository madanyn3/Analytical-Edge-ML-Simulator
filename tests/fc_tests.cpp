#include "edge/fc.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace edge;
void check(bool v) { if(!v) throw std::runtime_error("check failed"); }
template<class F> void rejects(F f) { bool caught=false; try{f();}catch(const std::invalid_argument&){caught=true;} check(caught); }
// Independent pointer-stepping oracle: no IR domains, address evaluator or count formulas.
std::vector<Event> reference(int n,int m,Layout l) {
    std::vector<Event> out;
    auto add=[&](Tensor t,std::uint64_t p,unsigned b,bool wr=false){out.push_back({t,p,b,wr});};
    auto weight=l.weights,bias=l.bias,dst=l.output;
    int rows=m;
    while(rows) {
        const int batch=std::min(rows,2);
        for(int j=0;j<batch;++j){add(Tensor::bias,bias,4);bias+=4;}
        auto input=l.input, a=weight, b=weight+static_cast<unsigned>(n);
        int left=n;
        while(left) {
            const unsigned width=left>=4?4:1;
            add(Tensor::input,input,width);input+=width;
            add(Tensor::weights,a,width);a+=width;
            if(batch==2){add(Tensor::weights,b,width);b+=width;}
            left-=static_cast<int>(width);
        }
        for(int j=0;j<batch;++j){add(Tensor::output,dst,1,true);++dst;}
        weight+=static_cast<unsigned>(batch*n); rows-=batch;
    }
    return out;
}
// Independent integer rounding via magnitude/remainder rather than signed shifts.
std::int32_t reference_quant(std::int32_t a,std::int32_t mult,int shift) {
    std::int64_t product=static_cast<std::int64_t>(a)*mult*(INT64_C(1)<<std::max(shift,0));
    auto mag=std::abs(product), d=INT64_C(1)<<31;
    auto h=mag/d+(product>=0 ? mag%d>=d/2 : mag%d>d/2);
    if(product<0)h=-h;
    d=INT64_C(1)<<std::max(-shift,0);mag=std::abs(h);
    auto r=mag/d+(d>1 && mag%d>=d/2);
    return static_cast<std::int32_t>(h<0?-r:r);
}
int main() {
    FCSpec s{5,3,1,-1,1073741824,-1,-4,6};
    Layout l{0x20040000,0x08010000,0x08010020,0x20040020};
    auto ir=derive_fc(s,l);
    std::array<std::int8_t,5>x{-3,-1,0,2,4};
    std::array<std::int8_t,15>w{1,2,-1,0,2,-1,1,2,-2,1,2,0,-3,1,2};
    std::array<std::int32_t,3>b{2,-12,34};
    check(evaluate(ir,x,w,b)==std::vector<std::int8_t>({2,-3,6}));
    const auto c=count_work(ir);
    check(c.macs==15 && c.logical_bytes==35 && c.requested_bytes==40 && c.tensor_events==16);
    const std::vector<Event> golden{
        {Tensor::bias,l.bias,4,false},{Tensor::bias,l.bias+4,4,false},
        {Tensor::input,l.input,4,false},{Tensor::weights,l.weights,4,false},{Tensor::weights,l.weights+5,4,false},
        {Tensor::input,l.input+4,1,false},{Tensor::weights,l.weights+4,1,false},{Tensor::weights,l.weights+9,1,false},
        {Tensor::output,l.output,1,true},{Tensor::output,l.output+1,1,true},
        {Tensor::bias,l.bias+8,4,false},{Tensor::input,l.input,4,false},{Tensor::weights,l.weights+10,4,false},
        {Tensor::input,l.input+4,1,false},{Tensor::weights,l.weights+14,1,false},{Tensor::output,l.output+2,1,true}};
    check(enumerate_events(ir)==golden);
    for(int n:{1,3,4,5,7,8,9})for(int m:{1,2,3,4,5}) {
        auto spec=s;spec.n=n;spec.m=m;
        Layout layout{0x20040000,0x08010000,0x08011000,0x20041000};
        auto model=derive_fc(spec,layout);
        auto events=reference(n,m,layout);
        check(events==enumerate_events(model));
        std::uint64_t bytes=0;for(const auto&e:events)bytes+=e.bytes;
        check(bytes==count_work(model).requested_bytes && events.size()==count_work(model).tensor_events);
        std::vector<std::int8_t> inputs(n),weights(n*m);
        std::vector<std::int32_t> biases(m);
        for(int i=0;i<n;++i)inputs[i]=static_cast<std::int8_t>(i%9-4);
        for(int i=0;i<n*m;++i)weights[i]=static_cast<std::int8_t>(i%7-3);
        auto result=evaluate(model,inputs,weights,biases);
        for(int j=0;j<m;++j){int sum=0;for(int i=0;i<n;++i)sum+=(inputs[i]+spec.input_offset)*weights[j*n+i];
            check(result[j]==std::clamp(reference_quant(sum,spec.multiplier,spec.shift)+spec.output_offset,spec.activation_min,spec.activation_max));}
    }
    for(int shift:{-30,-5,-1,0,1,5,30})for(int mult:{1,715827883,1073741824,INT32_MAX})
        for(int a=-31;a<=31;++a) {
            if(shift==30 && (a < -1 || a > 1))continue;
            check(requantize(a,mult,shift)==reference_quant(a,mult,shift));
        }
    for(int bad:{0,-1}){auto t=s;t.n=bad;rejects([&]{derive_fc(t,l);});}
    rejects([&]{auto t=l;t.output=t.input;derive_fc(s,t);});
    rejects([&]{auto t=l;t.input++;derive_fc(s,t);});
    rejects([&]{auto t=l;t.bias=0xfffffff0;auto spec=s;spec.m=5;derive_fc(spec,t);});
    rejects([&]{auto t=s;t.shift=-31;derive_fc(t,l);});
    rejects([&]{auto t=s;t.activation_min=7;derive_fc(t,l);});
    rejects([&]{requantize(INT32_MAX,1,1);});
    rejects([&]{enumerate_events(ir,15);});
    rejects([&]{evaluate(ir,std::span<const std::int8_t>{},w,b);});
    rejects([&]{auto bb=b;bb[0]=INT32_MAX;evaluate(ir,x,w,bb);});
    std::cout<<"Golden outputs/events, 35 shapes, rounding domain and rejection checks passed\n";
}
