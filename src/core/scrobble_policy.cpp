#include "scrobble_policy.h"

#include <algorithm>
#include <cmath>

namespace lastfm_scrobbler {
namespace {

[[nodiscard]] bool is_positive_finite(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

} // namespace

double required_listening_seconds(double duration_seconds) noexcept {
    if (!std::isfinite(duration_seconds) ||
        duration_seconds <= minimum_track_duration_seconds) {
        return 0.0;
    }

    return std::min(
        duration_seconds / 2.0,
        maximum_listening_threshold_seconds
    );
}

bool is_scrobble_eligible(
    double duration_seconds,
    double listened_seconds
) noexcept {
    const auto required = required_listening_seconds(duration_seconds);
    return required > 0.0 &&
        std::isfinite(listened_seconds) &&
        listened_seconds >= required;
}

void listening_session::start(double duration_seconds) noexcept {
    duration_seconds_ = is_positive_finite(duration_seconds)
        ? duration_seconds
        : 0.0;
    listened_seconds_ = 0.0;
    playback_position_seconds_ = 0.0;
    submitted_ = false;
    state_ = playback_state::playing;
}

void listening_session::pause() noexcept {
    if (state_ == playback_state::playing) {
        state_ = playback_state::paused;
    }
}

void listening_session::resume() noexcept {
    if (state_ == playback_state::paused) {
        state_ = playback_state::playing;
    }
}

void listening_session::stop() noexcept {
    state_ = playback_state::stopped;
}

void listening_session::record_played_interval(double seconds) noexcept {
    if (state_ != playback_state::playing || !is_positive_finite(seconds)) {
        return;
    }

    listened_seconds_ += seconds;
    playback_position_seconds_ += seconds;

    if (duration_seconds_ > 0.0) {
        listened_seconds_ = std::min(listened_seconds_, duration_seconds_);
        playback_position_seconds_ = std::min(
            playback_position_seconds_,
            duration_seconds_
        );
    }
}

void listening_session::seek(double position_seconds) noexcept {
    if (!std::isfinite(position_seconds)) {
        return;
    }

    const auto upper_bound = duration_seconds_ > 0.0
        ? duration_seconds_
        : 0.0;
    playback_position_seconds_ = std::clamp(position_seconds, 0.0, upper_bound);
}

playback_state listening_session::state() const noexcept {
    return state_;
}

double listening_session::duration_seconds() const noexcept {
    return duration_seconds_;
}

double listening_session::listened_seconds() const noexcept {
    return listened_seconds_;
}

double listening_session::playback_position_seconds() const noexcept {
    return playback_position_seconds_;
}

bool listening_session::ready_to_scrobble() const noexcept {
    return !submitted_ &&
        is_scrobble_eligible(duration_seconds_, listened_seconds_);
}

bool listening_session::was_submitted() const noexcept {
    return submitted_;
}

bool listening_session::mark_submitted() noexcept {
    if (!ready_to_scrobble()) {
        return false;
    }

    submitted_ = true;
    return true;
}

} // namespace lastfm_scrobbler

