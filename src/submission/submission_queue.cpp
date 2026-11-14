#include "submission_queue.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lastfm_scrobbler {
namespace {

constexpr std::array<std::uint8_t, 5> legacy_payload_header = {
    'F', 'L', 'Q', 'U', 1,
};
constexpr std::array<std::uint8_t, 5> payload_header = {
    'F', 'L', 'Q', 'U', 2,
};
constexpr std::size_t maximum_field_size = 16 * 1024;
constexpr std::size_t maximum_plaintext_size = 16 * 1024 * 1024;

// Every track the metadata normalizer accepts must be storable.
static_assert(maximum_metadata_field_bytes <= maximum_field_size);

void ensure_room(const std::vector<std::uint8_t>& output, std::size_t extra) {
    if (extra > maximum_plaintext_size ||
        output.size() > maximum_plaintext_size - extra) {
        throw std::length_error("Last.fm scrobble queue is too large");
    }
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    ensure_room(output, 4);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    ensure_room(output, 8);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_string(std::vector<std::uint8_t>& output, const std::string& value) {
    if (value.size() > maximum_field_size) {
        throw std::length_error("Last.fm scrobble metadata field is too large");
    }
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    ensure_room(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] std::uint32_t read_u32(
    std::span<const std::uint8_t> input,
    std::size_t& offset
) {
    if (offset > input.size() || input.size() - offset < 4) {
        throw std::invalid_argument("Last.fm scrobble queue is truncated");
    }
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(input[offset++]) << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(
    std::span<const std::uint8_t> input,
    std::size_t& offset
) {
    if (offset > input.size() || input.size() - offset < 8) {
        throw std::invalid_argument("Last.fm scrobble queue is truncated");
    }
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(input[offset++]) << shift;
    }
    return value;
}

[[nodiscard]] std::string read_string(
    std::span<const std::uint8_t> input,
    std::size_t& offset
) {
    const auto size = static_cast<std::size_t>(read_u32(input, offset));
    if (size > maximum_field_size ||
        offset > input.size() || input.size() - offset < size) {
        throw std::invalid_argument(
            "Last.fm scrobble queue metadata is malformed"
        );
    }
    std::string output(
        reinterpret_cast<const char*>(input.data() + offset),
        size
    );
    offset += size;
    return output;
}

void validate_record(const scrobble_record& record) {
    if (record.started_at_unix <= 0 ||
        record.track.artist.empty() || record.track.title.empty() ||
        record.track.artist.find('\0') != std::string::npos ||
        record.track.title.find('\0') != std::string::npos ||
        !std::isfinite(record.track.duration_seconds) ||
        record.track.duration_seconds < 0.0) {
        throw std::invalid_argument(
            "Last.fm scrobble queue record is invalid"
        );
    }
}

void validate_owner(
    std::string_view owner_username,
    bool queue_is_empty
) {
    if (owner_username.size() > maximum_field_size ||
        owner_username.find('\0') != std::string_view::npos ||
        (!queue_is_empty && owner_username.empty())) {
        throw std::invalid_argument(
            "Last.fm scrobble queue owner is invalid"
        );
    }
}

void append_record(
    std::vector<std::uint8_t>& output,
    const scrobble_record& record
) {
    validate_record(record);
    append_u64(output, static_cast<std::uint64_t>(record.started_at_unix));
    append_u64(
        output,
        std::bit_cast<std::uint64_t>(record.track.duration_seconds)
    );
    ensure_room(output, 1);
    output.push_back(record.track.track_number.has_value() ? 1 : 0);
    if (record.track.track_number.has_value()) {
        append_u32(output, *record.track.track_number);
    }
    append_string(output, record.track.artist);
    append_string(output, record.track.title);
    append_string(output, record.track.album);
    append_string(output, record.track.album_artist);
    append_string(output, record.track.musicbrainz_track_id);
}

[[nodiscard]] scrobble_record read_record(
    std::span<const std::uint8_t> input,
    std::size_t& offset
) {
    scrobble_record record;
    record.started_at_unix = static_cast<std::int64_t>(read_u64(input, offset));
    record.track.duration_seconds = std::bit_cast<double>(read_u64(input, offset));
    if (offset >= input.size()) {
        throw std::invalid_argument("Last.fm scrobble queue is truncated");
    }
    const auto has_track_number = input[offset++];
    if (has_track_number > 1) {
        throw std::invalid_argument(
            "Last.fm scrobble queue track number flag is invalid"
        );
    }
    if (has_track_number != 0) {
        record.track.track_number = read_u32(input, offset);
    }
    record.track.artist = read_string(input, offset);
    record.track.title = read_string(input, offset);
    record.track.album = read_string(input, offset);
    record.track.album_artist = read_string(input, offset);
    record.track.musicbrainz_track_id = read_string(input, offset);
    validate_record(record);
    return record;
}

} // namespace

std::vector<std::uint8_t> serialize_scrobble_queue(
    const persisted_scrobble_queue& queue
) {
    if (queue.records.size() > maximum_queued_scrobbles) {
        throw std::length_error("Too many queued Last.fm scrobbles");
    }
    validate_owner(queue.owner_username, queue.records.empty());

    std::vector<std::uint8_t> output(
        payload_header.begin(),
        payload_header.end()
    );
    append_string(output, queue.owner_username);
    append_u32(
        output,
        static_cast<std::uint32_t>(queue.records.size())
    );
    for (const auto& record : queue.records) {
        append_record(output, record);
    }
    return output;
}

persisted_scrobble_queue deserialize_scrobble_queue(
    std::span<const std::uint8_t> payload
) {
    const bool is_current = payload.size() >= payload_header.size() &&
        std::equal(
            payload_header.begin(),
            payload_header.end(),
            payload.begin()
        );
    const bool is_legacy = payload.size() >= legacy_payload_header.size() &&
        std::equal(
            legacy_payload_header.begin(),
            legacy_payload_header.end(),
            payload.begin()
        );
    if (payload.size() > maximum_plaintext_size ||
        payload.size() < payload_header.size() + 4 ||
        (!is_current && !is_legacy)) {
        throw std::invalid_argument(
            "Last.fm scrobble queue version is unsupported"
        );
    }

    auto offset = payload_header.size();
    persisted_scrobble_queue queue;
    if (is_current) {
        queue.owner_username = read_string(payload, offset);
    }
    const auto count = static_cast<std::size_t>(read_u32(payload, offset));
    if (count > maximum_queued_scrobbles) {
        throw std::invalid_argument("Last.fm scrobble queue count is invalid");
    }

    queue.records.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        queue.records.push_back(read_record(payload, offset));
    }
    if (offset != payload.size()) {
        throw std::invalid_argument(
            "Last.fm scrobble queue has trailing data"
        );
    }
    if (is_current) {
        validate_owner(queue.owner_username, queue.records.empty());
    }
    return queue;
}

} // namespace lastfm_scrobbler
