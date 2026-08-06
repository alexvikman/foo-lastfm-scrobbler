#include "retry_policy.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace lastfm_scrobbler {

unsigned retry_delay_seconds(unsigned failure_count) noexcept {
    constexpr std::array<unsigned, 4> delays = {
        30,
        2 * 60,
        10 * 60,
        30 * 60,
    };
    if (failure_count == 0) {
        return 0;
    }
    const auto index = std::min<std::size_t>(
        static_cast<std::size_t>(failure_count - 1),
        delays.size() - 1
    );
    return delays[index];
}

} // namespace lastfm_scrobbler
