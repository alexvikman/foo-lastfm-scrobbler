#pragma once

#include "submission_record.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lastfm_scrobbler {

inline constexpr std::size_t maximum_queued_scrobbles = 10'000;

struct persisted_scrobble_queue {
    std::string owner_username;
    std::vector<scrobble_record> records;

    [[nodiscard]] bool operator==(
        const persisted_scrobble_queue&
    ) const = default;
};

[[nodiscard]] std::vector<std::uint8_t> serialize_scrobble_queue(
    const persisted_scrobble_queue& queue
);

[[nodiscard]] persisted_scrobble_queue deserialize_scrobble_queue(
    std::span<const std::uint8_t> payload
);

} // namespace lastfm_scrobbler
