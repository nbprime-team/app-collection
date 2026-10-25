#ifndef NMARKDOWN_PLATFORM_ALLOCATION_STATS_H
#define NMARKDOWN_PLATFORM_ALLOCATION_STATS_H

#include <cstdint>

namespace nmarkdown {

// Heap counters are available only in the explicitly instrumented Ndless
// build. Keeping the production implementation inline avoids adding any
// allocator interception or bookkeeping cost to the normal reader.
struct AllocationStats {
    bool available = false;
    bool checkpoint_available = false;
    std::uint64_t current_bytes = 0;
    std::uint64_t lifetime_peak_bytes = 0;
    std::uint64_t checkpoint_peak_bytes = 0;
    std::uint64_t allocation_count = 0;
    std::uint64_t allocation_failures = 0;
    std::uint64_t tracking_overflows = 0;
};

#if defined(NMARKDOWN_ALLOCATION_PROBE)
AllocationStats allocation_stats();
void allocation_stats_begin_checkpoint();
#else
inline AllocationStats allocation_stats() { return {}; }
inline void allocation_stats_begin_checkpoint() {}
#endif

}  // namespace nmarkdown

#endif
