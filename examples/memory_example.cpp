#include "edge/memory.hpp"
#include <array>
#include <iostream>
int main() {
    const auto ir=edge::derive_fc({5,3,1,-1,1073741824,-1,-4,6},
        {0x20040000,0x08010000,0x08010020,0x20040020});
    std::vector<edge::Event> flash;
    for (auto e:edge::enumerate_events(ir))
        if (e.tensor==edge::Tensor::weights || e.tensor==edge::Tensor::bias) flash.push_back(e);
    std::cout<<"Synthetic source-level flash-read cache; NOT a verified ART model\n"
             <<"line bytes,sets,ways,cold refs,cold misses,repeated misses,cold fill bytes\n";
    for (std::uint64_t ways: {1U,2U,4U}) {
        const edge::CacheGeometry g{8,2,ways};
        const auto cold=edge::analyze_reuse(g,flash);
        const auto warm=edge::analyze_reuse(g,flash,cold.final_state);
        std::cout<<8<<','<<2<<','<<ways<<','<<cold.references<<','<<cold.misses<<','
                 <<warm.misses<<','<<cold.fill_bytes<<'\n';
    }
    const auto cold=edge::analyze_reuse({8,2,2},flash);
    const auto work=edge::count_work(ir);
    const std::array<edge::BandwidthDemand,2> unknown{{
        {"flash fills",cold.fill_bytes,std::nullopt,"unknown board service ceiling"},
        {"SRAM tensor requests",work.input_bytes+work.output_bytes,std::nullopt,"source schedule"}}};
    const auto board=edge::roofline_bound(work.macs,std::nullopt,"",unknown);
    std::cout<<"Board timing: unknown; missing ceilings="<<board.unknowns.size()<<'\n';
    auto scenario=unknown;
    scenario[0].bytes_per_second=20000000;
    scenario[1].bytes_per_second=100000000;
    for (auto& d:scenario) d.provenance="hypothetical teaching scenario, not L4R5 data";
    const auto bound=edge::roofline_bound(work.macs,100000000,"hypothetical teaching scenario",scenario);
    std::cout<<"Hypothetical scenario only: flash intensity="
             <<static_cast<double>(work.macs)/static_cast<double>(cold.fill_bytes)
             <<" MAC/byte; time >= "<<bound.known_lower_bound_seconds*1e6<<" us\n";
    const auto sweep=edge::analyze_cold_sweep({8,2,2},0,5,1000000000);
    std::cout<<"Symbolic sweep: "<<sweep.references<<" references, "<<sweep.misses
             <<" misses (no reference expansion)\n";
}
