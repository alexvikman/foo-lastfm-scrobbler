#pragma once

namespace lastfm_scrobbler {

inline constexpr double minimum_track_duration_seconds = 30.0;
inline constexpr double maximum_listening_threshold_seconds = 4.0 * 60.0;

// Returns zero for tracks that Last.fm does not consider eligible.
[[nodiscard]] double required_listening_seconds(double duration_seconds) noexcept;

[[nodiscard]] bool is_scrobble_eligible(
    double duration_seconds,
    double listened_seconds
) noexcept;

enum class playback_state {
    stopped,
    playing,
    paused,
};

class listening_session final {
public:
    void start(double duration_seconds) noexcept;
    void pause() noexcept;
    void resume() noexcept;
    void stop() noexcept;

    // Adds wall-clock time known to have been spent actively playing audio.
    void record_played_interval(double seconds) noexcept;

    // Seeking changes the playback position but must not count as listening.
    void seek(double position_seconds) noexcept;

    [[nodiscard]] playback_state state() const noexcept;
    [[nodiscard]] double duration_seconds() const noexcept;
    [[nodiscard]] double listened_seconds() const noexcept;
    [[nodiscard]] double playback_position_seconds() const noexcept;
    [[nodiscard]] bool ready_to_scrobble() const noexcept;
    [[nodiscard]] bool was_submitted() const noexcept;

    // Returns true exactly once, and only after the listening threshold is met.
    [[nodiscard]] bool mark_submitted() noexcept;

private:
    playback_state state_ = playback_state::stopped;
    double duration_seconds_ = 0.0;
    double listened_seconds_ = 0.0;
    double playback_position_seconds_ = 0.0;
    bool submitted_ = false;
};

} // namespace lastfm_scrobbler

