#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace lastfm_scrobbler {

struct source_track_metadata {
    std::string artist;
    std::string title;
    std::string album;
    std::string album_artist;
    std::string track_number;
    std::string musicbrainz_track_id;
    double duration_seconds = 0.0;
};

struct track_metadata {
    std::string artist;
    std::string title;
    std::string album;
    std::string album_artist;
    std::optional<std::uint32_t> track_number;
    std::string musicbrainz_track_id;
    double duration_seconds = 0.0;

    [[nodiscard]] bool operator==(const track_metadata&) const = default;
};

enum class metadata_error {
    none,
    missing_artist,
    missing_title,
    filtered_artist,
    filtered_title,
};

struct metadata_result {
    std::optional<track_metadata> value;
    metadata_error error = metadata_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return value.has_value();
    }
};

[[nodiscard]] metadata_result normalize_track_metadata(
    const source_track_metadata& source
);

[[nodiscard]] const char* metadata_error_message(metadata_error error) noexcept;

} // namespace lastfm_scrobbler
