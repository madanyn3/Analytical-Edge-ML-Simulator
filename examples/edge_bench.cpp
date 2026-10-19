#include "edge/cluster.hpp"
#include "edge/testbench.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
    try {
        if (argc!=2 && argc!=4) throw std::invalid_argument("usage: edge_bench CONFIG [--trace READS.csv]");
        // [model] version selects the model: 1 = single-stream hierarchy, 2 = tiled cluster.
        if (edge::config_file_version(argv[1])==2) {
            if (argc!=2) throw std::invalid_argument("--trace is not supported for version 2 cluster configs");
            const auto config=edge::load_cluster_config(argv[1]);
            std::cout<<edge::report_cluster(config,edge::run_cluster(config));
            return 0;
        }
        const auto config=edge::load_config(argv[1]);
        std::vector<edge::Request> reads;
        if (argc==4) {
            if (std::string(argv[2])!="--trace") throw std::invalid_argument("expected --trace");
            std::ifstream file(argv[3],std::ios::binary);
            if (!file) throw std::invalid_argument("cannot open stimulus file");
            std::string text; char ch;
            while (file.get(ch)) {
                if (text.size()==64*1024*1024) throw std::length_error("stimulus file exceeds 64 MiB");
                text+=ch;
            }
            if (!file.eof()) throw std::runtime_error("stimulus read failed");
            reads=edge::parse_stimulus(text,argv[3],config.stimulus.max_requests);
        }
        const auto result=edge::run_testbench(config,argc==4?&reads:nullptr);
        std::cout<<edge::report_testbench(config,result);
    } catch (const std::exception& e) { std::cerr<<"edge_bench: "<<e.what()<<'\n'; return 1; }
}
