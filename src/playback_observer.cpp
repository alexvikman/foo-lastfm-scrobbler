#include <foobar2000/SDK/foobar2000.h>

#include "core/scrobble_policy.h"
#include "core/track_metadata.h"
#include "submission/submission_service.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>

namespace {

class playback_observer : public play_callback_static {
public:
    unsigned get_flags() override {
        return flag_on_playback_starting |
            flag_on_playback_new_track |
            flag_on_playback_stop |
            flag_on_playback_seek |
            flag_on_playback_pause |
            flag_on_playback_edited |
            flag_on_playback_time;
    }

    void on_playback_starting(
        play_control::t_track_command,
        bool paused
    ) noexcept override {
        playback_is_paused_ = paused;
    }

    void on_playback_new_track(metadb_handle_ptr track) noexcept override {
        session_.start(track->get_length());
        if (playback_is_paused_) {
            session_.pause();
        }

        last_reported_time_ = 0.0;
        has_reported_time_ = true;
        submission_attempted_ = false;
        now_playing_requested_ = false;
        started_at_unix_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        capture_metadata(track);
    }

    void on_playback_stop(play_control::t_stop_reason) noexcept override {
        if (now_playing_requested_) {
            lastfm_scrobbler::remove_now_playing();
        }
        session_.stop();
        has_reported_time_ = false;
        playback_is_paused_ = false;
        submission_attempted_ = false;
        now_playing_requested_ = false;
        started_at_unix_ = 0;
        current_metadata_.reset();
    }

    void on_playback_seek(double time) noexcept override {
        session_.seek(time);
        last_reported_time_ = time;
        has_reported_time_ = std::isfinite(time);
    }

    void on_playback_pause(bool paused) noexcept override {
        playback_is_paused_ = paused;
        if (paused) {
            session_.pause();
            if (now_playing_requested_) {
                lastfm_scrobbler::remove_now_playing();
                now_playing_requested_ = false;
            }
        } else {
            session_.resume();
            if (current_metadata_.has_value() && !now_playing_requested_) {
                lastfm_scrobbler::submit_now_playing(*current_metadata_);
                now_playing_requested_ = true;
            }
        }
    }

    void on_playback_time(double time) noexcept override {
        if (!std::isfinite(time)) {
            return;
        }

        if (has_reported_time_) {
            const auto elapsed = time - last_reported_time_;
            session_.record_played_interval(elapsed);
        }

        last_reported_time_ = time;
        has_reported_time_ = true;

        if (session_.ready_to_scrobble() && !submission_attempted_) {
            submission_attempted_ = true;
            if (current_metadata_.has_value()) {
                const lastfm_scrobbler::scrobble_record record{
                    .track = *current_metadata_,
                    .started_at_unix = started_at_unix_,
                };
                if (lastfm_scrobbler::enqueue_scrobble(record)) {
                    static_cast<void>(session_.mark_submitted());
                    console::print(
                        "foo_lastfm_scrobbler: eligible track added to the encrypted scrobble queue"
                    );
                } else {
                    console::print(
                        "foo_lastfm_scrobbler: eligible track could not be queued; verify that the Last.fm account is connected"
                    );
                }
            }
        }
    }

    void on_playback_edited(metadb_handle_ptr track) noexcept override {
        capture_metadata(track);
    }
    void on_playback_dynamic_info(const file_info&) noexcept override {}
    void on_playback_dynamic_info_track(const file_info&) noexcept override {}
    void on_volume_change(float) noexcept override {}

private:
    [[nodiscard]] static std::string read_joined_tag(
        const file_info& info,
        const char* name
    ) {
        pfc::string8 formatted;
        if (!info.meta_format(name, formatted, ", ")) {
            return {};
        }
        return formatted.c_str();
    }

    [[nodiscard]] static std::string read_first_tag(
        const file_info& info,
        const char* name
    ) {
        if (info.meta_get_count_by_name(name) == 0) {
            return {};
        }
        return info.meta_get(name, 0);
    }

    void capture_metadata(metadb_handle_ptr track) noexcept {
        try {
            const auto info_reference = track->get_info_ref();
            const auto& info = info_reference->info();

            lastfm_scrobbler::source_track_metadata source;
            source.artist = read_joined_tag(info, "artist");
            source.title = read_first_tag(info, "title");
            source.album = read_first_tag(info, "album");
            source.album_artist = read_joined_tag(info, "album artist");
            source.track_number = read_first_tag(info, "tracknumber");
            source.musicbrainz_track_id = read_first_tag(
                info,
                "musicbrainz_trackid"
            );
            source.duration_seconds = track->get_length();

            auto normalized = lastfm_scrobbler::normalize_track_metadata(source);
            current_metadata_ = std::move(normalized.value);
            if (!current_metadata_.has_value()) {
                FB2K_console_formatter() <<
                    "foo_lastfm_scrobbler: current track will not be scrobbled (" <<
                    lastfm_scrobbler::metadata_error_message(normalized.error) << ')';
            } else if (!playback_is_paused_ && !now_playing_requested_) {
                lastfm_scrobbler::submit_now_playing(*current_metadata_);
                now_playing_requested_ = true;
            }
        } catch (const std::exception& exception) {
            current_metadata_.reset();
            console::complain(
                "foo_lastfm_scrobbler: could not read current track metadata",
                exception
            );
        } catch (...) {
            current_metadata_.reset();
            console::print(
                "foo_lastfm_scrobbler: could not read current track metadata"
            );
        }
    }

    lastfm_scrobbler::listening_session session_;
    std::optional<lastfm_scrobbler::track_metadata> current_metadata_;
    double last_reported_time_ = 0.0;
    bool has_reported_time_ = false;
    bool playback_is_paused_ = false;
    bool submission_attempted_ = false;
    bool now_playing_requested_ = false;
    std::int64_t started_at_unix_ = 0;
};

play_callback_static_factory_t<playback_observer> playback_observer_factory;

} // namespace
