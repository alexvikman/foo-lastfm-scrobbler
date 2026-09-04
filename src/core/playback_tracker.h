#pragma once

#include "scrobble_policy.h"
#include "../submission/submission_record.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace lastfm_scrobbler {

class playback_submission_sink {
public:
    virtual ~playback_submission_sink() = default;
    virtual void publish_track(const track_metadata& track) noexcept = 0;
    virtual void clear_track() noexcept = 0;
    [[nodiscard]] virtual bool enqueue_track(
        const scrobble_record& record
    ) noexcept = 0;
};

enum class playback_enqueue_result {
    none,
    first_failure,
    queued,
};

// Tracks listening time and coordinates submissions with playback events.
class playback_tracker final {
public:
    using clock = std::chrono::steady_clock;

    explicit playback_tracker(playback_submission_sink& sink) noexcept;

    void starting(bool paused) noexcept;
    void new_track(double duration_seconds, std::int64_t started_at_unix) noexcept;
    void set_metadata(std::optional<track_metadata> metadata) noexcept;
    void stop() noexcept;
    void seek(double position_seconds) noexcept;
    void pause(bool paused) noexcept;
    [[nodiscard]] playback_enqueue_result time(
        double position_seconds,
        clock::time_point now
    ) noexcept;

private:
    void synchronize_now_playing(bool metadata_changed) noexcept;

    playback_submission_sink& sink_;
    listening_session session_;
    std::optional<scrobble_record> current_record_;
    std::optional<clock::time_point> next_enqueue_attempt_;
    double last_reported_time_ = 0.0;
    std::int64_t started_at_unix_ = 0;
    bool has_reported_time_ = false;
    bool playback_is_paused_ = false;
    bool now_playing_requested_ = false;
    bool enqueue_failure_reported_ = false;
};

} // namespace lastfm_scrobbler
