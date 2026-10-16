#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace edge::checked {
using U = std::uint64_t;
inline U add(U a, U b) {
    if (b > std::numeric_limits<U>::max() - a)
        throw std::overflow_error("unsigned sum exceeds supported 64-bit domain");
    return a + b;
}
inline U mul(U a, U b) {
    if (a && b > std::numeric_limits<U>::max() / a)
        throw std::overflow_error("unsigned product exceeds supported 64-bit domain");
    return a * b;
}
inline U ceil_div(U a, U b) {
    if (!b) throw std::invalid_argument("zero divisor");
    return a / b + static_cast<U>(a % b != 0);
}
}
