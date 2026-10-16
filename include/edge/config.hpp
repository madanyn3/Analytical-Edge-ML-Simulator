#pragma once
#include "edge/memory.hpp"
#include <string_view>

namespace edge {
// All times are integer ticks in the single model clock. No implicit unit conversion.
struct ServiceConfig {
    std::uint64_t first_beat_cycles{}, beat_bytes{}, beat_cycles{}, issue_cycles{}, outstanding{};
    std::string provenance;
};
struct CacheConfig {
    std::string name;
    CacheGeometry geometry{};
    std::uint64_t lookup_cycles{}, fill_cycles{};
    CacheState initial;
    std::string provenance;
};
struct StimulusConfig {
    std::string entry, mode, engine;
    std::uint64_t count{}, address{}, bytes{}, stride{}, address_period{}, arrival_cycles{}, spacing_cycles{};
    std::uint64_t max_requests{}, max_transactions{};
};
struct ModelConfig {
    std::uint64_t clock_hz{};
    std::string inclusion; // "inclusive" or "noninclusive"; no coherence/writes.
    ServiceConfig memory;
    std::vector<CacheConfig> caches; // Ordered upper to lower, section declaration order.
    StimulusConfig stimulus;
};
// Strict, small INI dialect, not a general TOML/INI implementation. Diagnostics include
// source, line and key. Unknown/duplicate keys, sections and unsupported policies fail.
ModelConfig parse_config(std::string_view text, const std::string& source = "<config>");
ModelConfig load_config(const std::string& path);
void validate_config(const ModelConfig&);
std::string describe_config(const ModelConfig&); // Deterministic effective knobs/provenance.
}
