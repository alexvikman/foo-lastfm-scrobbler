#pragma once

namespace lastfm_scrobbler {

// failure_count is one-based. Later failures remain capped at 30 minutes.
[[nodiscard]] unsigned retry_delay_seconds(unsigned failure_count) noexcept;

} // namespace lastfm_scrobbler
