#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace edge {
inline constexpr auto kernel_commit = "13c97dbb6f781d4aab38ed34e6e441f42b79aff4";
// Fixed path: batch one, DSP, bias present, filter offset zero, per-tensor rounding.
struct FCSpec {
    std::int32_t n, m, input_offset, output_offset, multiplier, shift;
    std::int32_t activation_min = -128, activation_max = 127;
};
struct Layout { std::uint64_t input, weights, bias, output; };
enum class Tensor { input, weights, bias, output };
struct Event {
    Tensor tensor;
    std::uint64_t address;
    std::uint32_t bytes;
    bool write;
    bool operator==(const Event&) const = default;
};
// Compact source schedule; no per-access storage or binary/timing claims.
class KernelIR {
public:
    const FCSpec& spec() const { return spec_; }
    const Layout& layout() const { return layout_; }
    std::uint64_t pairs() const { return static_cast<std::uint64_t>(spec_.m)/2; }
    std::uint64_t odd_rows() const { return static_cast<std::uint64_t>(spec_.m)%2; }
    std::uint64_t groups() const { return static_cast<std::uint64_t>(spec_.n)/4; }
    std::uint64_t tail() const { return static_cast<std::uint64_t>(spec_.n)%4; }
private:
    KernelIR(FCSpec s, Layout l): spec_(s), layout_(l) {}
    FCSpec spec_;
    Layout layout_;
    friend KernelIR derive_fc(FCSpec, Layout);
};
struct WorkSummary {
    std::uint64_t macs, smlad_calls, scalar_tail_macs, requantizations;
    std::uint64_t logical_bytes, input_bytes, weight_bytes, bias_bytes, output_bytes;
    std::uint64_t requested_bytes, tensor_events;
};
KernelIR derive_fc(FCSpec, Layout);
WorkSummary count_work(const KernelIR&);
std::vector<Event> enumerate_events(const KernelIR&, std::uint64_t limit = 1000000);
std::int32_t requantize(std::int32_t value, std::int32_t multiplier, std::int32_t shift);
std::vector<std::int8_t> evaluate(const KernelIR&, std::span<const std::int8_t> input,
    std::span<const std::int8_t> weights, std::span<const std::int32_t> bias);
}
