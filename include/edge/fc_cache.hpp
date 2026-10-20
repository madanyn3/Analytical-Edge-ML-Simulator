#pragma once
#include "edge/memory.hpp"

namespace edge {
// Theorem A1 (docs/fc-cache-theorem.md §3): one cold call of the FC flash stream (the
// weight and bias events of enumerate_events, in order), without enumerating it.
// Misses = distinct lines + re-misses. Every re-reference falls into exactly one kind:
//   cursor   - a row cursor continues on its current line; gap = the other cursor's access;
//   middle   - row j+1's cursor touched the line shared with row j first; row j's cursor
//              returns at the end of the pair; gap = a contiguous line range;
//   boundary - the line shared by consecutive regions; gap = the next region's bias lines;
//   bias     - a bias line reused by the next region; gap = the previous region's weights.
// A re-reference misses iff at least W other lines of its set occur in its gap.
// Exact under the synthetic contract of memory-models.md; cost independent of n.
struct FcFlashCounts {
    CacheCounts counts;   // references, hits, misses, evictions, fill_bytes, final_state.
    std::uint64_t distinct_lines{};
    std::uint64_t cursor_re_misses{}, middle_re_misses{}, boundary_re_misses{}, bias_re_misses{};
};
// Supported domain (anything else throws std::invalid_argument):
//   n >= 2L + 8, so the two cursors' concurrent lines are disjoint and row j+1's cursor
//   leaves the shared middle line before row j's cursor reaches it;
//   the weight and bias line ranges are disjoint.
FcFlashCounts analyze_fc_flash(CacheGeometry, const KernelIR&);
}
