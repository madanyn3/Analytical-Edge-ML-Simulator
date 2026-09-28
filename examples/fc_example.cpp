#include "edge/fc.hpp"
#include <array>
#include <iostream>
int main() {
    const auto ir=edge::derive_fc({5,3,1,-1,1073741824,-1,-4,6},
                                 {0x20040000,0x08010000,0x08010020,0x20040020});
    const std::array<std::int8_t,5> x{-3,-1,0,2,4};
    const std::array<std::int8_t,15> w{1,2,-1,0,2,-1,1,2,-2,1,2,0,-3,1,2};
    const std::array<std::int32_t,3> b{2,-12,34};
    const auto c=edge::count_work(ir);
    std::cout<<"source_tensor_schedule; exact under the documented source abstraction\noutput:";
    for(auto y:edge::evaluate(ir,x,w,b)) std::cout<<' '<<static_cast<int>(y);
    std::cout<<"\nMACs: "<<c.macs<<"\nlogical bytes: "<<c.logical_bytes
             <<"\nrequested tensor bytes: "<<c.requested_bytes<<"\nevents: "<<c.tensor_events
             <<"\ncycles/cache misses/physical traffic: unsupported\n";
    for(const auto& e:edge::enumerate_events(ir))
        std::cout<<(e.write?"write ":"read  ")<<"0x"<<std::hex<<e.address<<std::dec<<" bytes="<<e.bytes<<'\n';
}
