#include <foobar2000/SDK/foobar2000.h>

#include "core/playback_tracker.h"
#include "core/track_metadata.h"
#include "submission/submission_service.h"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace {

class playback_observer : public play_callback_static,
    private lastfm_scrobbler::playback_submission_sink {
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
        tracker_.starting(paused);
    }

    void on_playback_new_track(metadb_handle_ptr track) noexcept override {
        const auto started_at_unix = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        tracker_.new_track(track->get_length(), started_at_unix);
        capture_metadata(track);
    }

    void on_playback_stop(play_control::t_stop_reason) noexcept override {
        tracker_.stop();
    }

    void on_playback_seek(double time) noexcept override {
        tracker_.seek(time);
    }

    void on_playback_pause(bool paused) noexcept override {
        tracker_.pause(paused);
    }

    void on_playback_time(double time) noexcept override {
        const auto result = tracker_.time(
            time,
            lastfm_scrobbler::playback_tracker::clock::now()
        );
        if (result == lastfm_scrobbler::playback_enqueue_result::queued) {
            console::print(
                "foo_lastfm_scrobbler: eligible track added to the encrypted scrobble queue"
            );
        } else if (result == lastfm_scrobbler::playback_enqueue_result::first_failure) {
            console::print(
                "foo_lastfm_scrobbler: eligible track could not be queued; retrying while the track remains active"
            );
        }
    }

    void on_playback_edited(metadb_handle_ptr track) noexcept override {
        capture_metadata(track);
    }
    void on_playback_dynamic_info(const file_info&) noexcept override {}
    void on_playback_dynamic_info_track(const file_info&) noexcept override {}
    void on_volume_change(float) noexcept override {}

private:
    void publish_track(const lastfm_scrobbler::track_metadata& track) noexcept override {
        lastfm_scrobbler::submit_now_playing(track);
    }

    void clear_track() noexcept override {
        lastfm_scrobbler::remove_now_playing();
    }

    bool enqueue_track(const lastfm_scrobbler::scrobble_record& record) noexcept override {
        return lastfm_scrobbler::enqueue_scrobble(record);
    }

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
            tracker_.set_metadata(std::move(normalized.value));
            if (normalized.error != lastfm_scrobbler::metadata_error::none) {
                FB2K_console_formatter() <<
                    "foo_lastfm_scrobbler: current track will not be scrobbled (" <<
                    lastfm_scrobbler::metadata_error_message(normalized.error) << ')';
            }
        } catch (const std::exception& exception) {
            tracker_.set_metadata(std::nullopt);
            console::complain(
                "foo_lastfm_scrobbler: could not read current track metadata",
                exception
            );
        } catch (...) {
            tracker_.set_metadata(std::nullopt);
            console::print(
                "foo_lastfm_scrobbler: could not read current track metadata"
            );
        }
    }

    lastfm_scrobbler::playback_tracker tracker_{*this};
};

play_callback_static_factory_t<playback_observer> playback_observer_factory;

} // namespace
