#include "playback_tracker.h"

#include <cmath>
#include <utility>

namespace lastfm_scrobbler {

playback_tracker::playback_tracker(playback_submission_sink& sink) noexcept :
    sink_(sink) {}

void playback_tracker::starting(bool paused) noexcept {
    playback_is_paused_ = paused;
}

void playback_tracker::new_track(
    double duration_seconds,
    std::int64_t started_at_unix
) noexcept {
    session_.start(duration_seconds);
    if (playback_is_paused_) {
        session_.pause();
    }
    last_reported_time_ = 0.0;
    has_reported_time_ = true;
    started_at_unix_ = started_at_unix;
    current_record_.reset();
    next_enqueue_attempt_.reset();
    enqueue_failure_reported_ = false;
    // Keep the previous publication until metadata replaces it or clears it.
}

void playback_tracker::set_metadata(
    std::optional<track_metadata> metadata
) noexcept {
    const bool changed = metadata.has_value() &&
        (!current_record_.has_value() || current_record_->track != *metadata);
    if (metadata.has_value()) {
        current_record_ = scrobble_record{
            .track = std::move(*metadata),
            .started_at_unix = started_at_unix_,
        };
        if (changed) {
            next_enqueue_attempt_.reset();
        }
    } else {
        current_record_.reset();
    }
    synchronize_now_playing(changed);
}

void playback_tracker::stop() noexcept {
    session_.stop();
    synchronize_now_playing(false);
    current_record_.reset();
    next_enqueue_attempt_.reset();
    has_reported_time_ = false;
    playback_is_paused_ = false;
    started_at_unix_ = 0;
}

void playback_tracker::seek(double position_seconds) noexcept {
    session_.seek(position_seconds);
    last_reported_time_ = position_seconds;
    has_reported_time_ = std::isfinite(position_seconds);
}

void playback_tracker::pause(bool paused) noexcept {
    playback_is_paused_ = paused;
    if (paused) {
        session_.pause();
    } else {
        session_.resume();
    }
    synchronize_now_playing(false);
}

void playback_tracker::synchronize_now_playing(bool metadata_changed) noexcept {
    if (session_.state() != playback_state::playing ||
        !current_record_.has_value()) {
        if (now_playing_requested_) {
            sink_.clear_track();
            now_playing_requested_ = false;
        }
    } else if (metadata_changed || !now_playing_requested_) {
        sink_.publish_track(current_record_->track);
        now_playing_requested_ = true;
    }
}

playback_enqueue_result playback_tracker::time(
    double position_seconds,
    clock::time_point now
) noexcept {
    if (!std::isfinite(position_seconds)) {
        return playback_enqueue_result::none;
    }
    if (has_reported_time_) {
        session_.record_played_interval(position_seconds - last_reported_time_);
    }
    last_reported_time_ = position_seconds;
    has_reported_time_ = true;

    if (session_.state() == playback_state::stopped ||
        !session_.ready_to_scrobble() || !current_record_.has_value() ||
        (next_enqueue_attempt_.has_value() && now < *next_enqueue_attempt_)) {
        return playback_enqueue_result::none;
    }
    if (sink_.enqueue_track(*current_record_)) {
        static_cast<void>(session_.mark_submitted());
        next_enqueue_attempt_.reset();
        return playback_enqueue_result::queued;
    }

    // Account reconnection and temporary persistence failures may recover while
    // the same track is playing. Seeking must not alter this retry interval.
    next_enqueue_attempt_ = now + std::chrono::seconds(5);
    if (!enqueue_failure_reported_) {
        enqueue_failure_reported_ = true;
        return playback_enqueue_result::first_failure;
    }
    return playback_enqueue_result::none;
}

} // namespace lastfm_scrobbler
