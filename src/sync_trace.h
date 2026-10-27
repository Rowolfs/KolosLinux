#pragma once

#include <cstdint>
#include <cstdio>
#include <inttypes.h>
#include <time.h>

namespace kolos {

// The compositor and Android app share CLOCK_MONOTONIC on the same kernel.
inline void traceSync(const char* side, const char* event, uint64_t session,
                      uint64_t generation, uint32_t slot, int64_t serial = -1,
                      int fenceFd = -1) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint64_t ns = uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
    std::fprintf(stderr,
                 "KOLOS_SYNC t_ns=%" PRIu64 " side=%s event=%s session=%" PRIu64
                 " generation=%" PRIu64 " slot=%u serial=%" PRId64 " fence_fd=%d\n",
                 ns, side, event, session, generation, slot, serial, fenceFd);
}

} // namespace kolos
