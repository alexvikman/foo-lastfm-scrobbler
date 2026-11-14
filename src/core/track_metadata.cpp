#include "track_metadata.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <string_view>

namespace lastfm_scrobbler {
namespace {

[[nodiscard]] bool is_ascii_space(unsigned char character) noexcept {
    return character == ' ' ||
        character == '\t' ||
        character == '\n' ||
        character == '\r' ||
        character == '\f' ||
        character == '\v';
}

[[nodiscard]] std::string normalize_text(std::string_view input) {
    std::string output;
    output.reserve(input.size());

    bool pending_space = false;
    for (const auto raw_character : input) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (is_ascii_space(character) || character < 0x20 || character == 0x7f) {
            pending_space = !output.empty();
            continue;
        }

        if (pending_space) {
            output.push_back(' ');
            pending_space = false;
        }
        output.push_back(raw_character);
    }

    return output;
}

[[nodiscard]] std::string ascii_lower(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const auto character : input) {
        const auto byte = static_cast<unsigned char>(character);
        output.push_back(static_cast<char>(
            byte < 0x80 ? std::tolower(byte) : byte
        ));
    }
    return output;
}

[[nodiscard]] bool is_filtered_artist(std::string_view artist) {
    const auto value = ascii_lower(artist);
    return value == "unknown artist" || value == "[unknown artist]";
}

[[nodiscard]] bool is_filtered_title(std::string_view title) {
    const auto value = ascii_lower(title);
    return value == "unknown track" || value == "[unknown track]";
}

[[nodiscard]] std::optional<std::uint32_t> parse_track_number(
    std::string_view input
) noexcept {
    if (input.empty() || input.front() < '0' || input.front() > '9') {
        return std::nullopt;
    }

    auto digit_count = std::size_t{0};
    while (digit_count < input.size() &&
        input[digit_count] >= '0' && input[digit_count] <= '9') {
        ++digit_count;
    }

    std::uint32_t value = 0;
    const auto result = std::from_chars(
        input.data(),
        input.data() + digit_count,
        value
    );
    if (result.ec != std::errc{} || value == 0) {
        return std::nullopt;
    }

    return value;
}

[[nodiscard]] bool is_hex_digit(char character) noexcept {
    return (character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'f') ||
        (character >= 'A' && character <= 'F');
}

[[nodiscard]] std::string normalize_musicbrainz_id(std::string_view input) {
    if (input.size() != 36) {
        return {};
    }

    for (std::size_t index = 0; index < input.size(); ++index) {
        const auto is_separator = index == 8 || index == 13 ||
            index == 18 || index == 23;
        if ((is_separator && input[index] != '-') ||
            (!is_separator && !is_hex_digit(input[index]))) {
            return {};
        }
    }

    return ascii_lower(input);
}

} // namespace

metadata_result normalize_track_metadata(const source_track_metadata& source) {
    track_metadata result;
    result.artist = normalize_text(source.artist);
    result.title = normalize_text(source.title);

    if (result.artist.empty()) {
        return {.error = metadata_error::missing_artist};
    }
    if (result.title.empty()) {
        return {.error = metadata_error::missing_title};
    }
    if (result.artist.size() > maximum_metadata_field_bytes) {
        return {.error = metadata_error::oversized_artist};
    }
    if (result.title.size() > maximum_metadata_field_bytes) {
        return {.error = metadata_error::oversized_title};
    }
    if (is_filtered_artist(result.artist)) {
        return {.error = metadata_error::filtered_artist};
    }
    if (is_filtered_title(result.title)) {
        return {.error = metadata_error::filtered_title};
    }

    // Optional fields are dropped rather than truncated, so a submission never
    // carries a partial name.
    result.album = normalize_text(source.album);
    if (result.album.size() > maximum_metadata_field_bytes) {
        result.album.clear();
    }
    result.album_artist = normalize_text(source.album_artist);
    if (result.album_artist.size() > maximum_metadata_field_bytes ||
        ascii_lower(result.album_artist) == ascii_lower(result.artist)) {
        result.album_artist.clear();
    }

    const auto normalized_track_number = normalize_text(source.track_number);
    result.track_number = parse_track_number(normalized_track_number);

    const auto normalized_mbid = normalize_text(source.musicbrainz_track_id);
    result.musicbrainz_track_id = normalize_musicbrainz_id(normalized_mbid);

    if (std::isfinite(source.duration_seconds) && source.duration_seconds > 0.0) {
        result.duration_seconds = source.duration_seconds;
    }

    return {
        .value = std::move(result),
        .error = metadata_error::none,
    };
}

const char* metadata_error_message(metadata_error error) noexcept {
    switch (error) {
    case metadata_error::none:
        return "no metadata error";
    case metadata_error::missing_artist:
        return "artist tag is missing";
    case metadata_error::missing_title:
        return "title tag is missing";
    case metadata_error::filtered_artist:
        return "artist tag contains a known placeholder";
    case metadata_error::filtered_title:
        return "title tag contains a known placeholder";
    case metadata_error::oversized_artist:
        return "artist tag is unreasonably long";
    case metadata_error::oversized_title:
        return "title tag is unreasonably long";
    }

    return "unknown metadata error";
}

} // namespace lastfm_scrobbler

