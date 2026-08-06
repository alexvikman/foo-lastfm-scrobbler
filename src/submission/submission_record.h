#pragma once

#include "../core/track_metadata.h"

#include <cstdint>

namespace lastfm_scrobbler {

struct scrobble_record {
    track_metadata track;
    std::int64_t started_at_unix = 0;

    [[nodiscard]] bool operator==(const scrobble_record&) const = default;
};

} // namespace lastfm_scrobbler
