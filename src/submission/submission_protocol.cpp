#include "submission_protocol.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace lastfm_scrobbler {
namespace {

void append_track_parameters(
    api_parameters& parameters,
    const track_metadata& track
) {
    if (track.artist.empty() || track.title.empty()) {
        throw std::invalid_argument(
            "Last.fm submissions require an artist and track title"
        );
    }

    parameters.emplace_back("artist", track.artist);
    parameters.emplace_back("track", track.title);
    if (!track.album.empty()) {
        parameters.emplace_back("album", track.album);
    }
    if (track.track_number.has_value()) {
        parameters.emplace_back(
            "trackNumber",
            std::to_string(*track.track_number)
        );
    }
    if (!track.musicbrainz_track_id.empty()) {
        parameters.emplace_back("mbid", track.musicbrainz_track_id);
    }
    if (std::isfinite(track.duration_seconds) &&
        track.duration_seconds > 0.0 &&
        track.duration_seconds <=
            static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        parameters.emplace_back(
            "duration",
            std::to_string(
                static_cast<std::int64_t>(std::llround(track.duration_seconds))
            )
        );
    }
    if (!track.album_artist.empty()) {
        parameters.emplace_back("albumArtist", track.album_artist);
    }
}

[[nodiscard]] signed_api_request finish_request(
    api_parameters parameters,
    const api_credentials& credentials,
    const std::string& session_key
) {
    if (session_key.empty()) {
        throw std::invalid_argument("Last.fm session key is empty");
    }
    if (credentials.api_key.size() != 32 ||
        credentials.shared_secret.empty()) {
        throw std::invalid_argument(
            "Last.fm API credentials are missing or malformed"
        );
    }

    parameters.emplace_back("api_key", credentials.api_key);
    parameters.emplace_back("format", "json");
    parameters.emplace_back("sk", session_key);

    signed_api_request request;
    request.parameters = std::move(parameters);
    request.api_signature = make_api_signature(
        request.parameters,
        credentials.shared_secret
    );
    return request;
}

} // namespace

signed_api_request make_now_playing_request(
    const api_credentials& credentials,
    const std::string& session_key,
    const track_metadata& track
) {
    api_parameters parameters{{"method", "track.updateNowPlaying"}};
    append_track_parameters(parameters, track);
    return finish_request(
        std::move(parameters),
        credentials,
        session_key
    );
}

signed_api_request make_remove_now_playing_request(
    const api_credentials& credentials,
    const std::string& session_key
) {
    return finish_request(
        {{"method", "track.removeNowPlaying"}},
        credentials,
        session_key
    );
}

signed_api_request make_scrobble_request(
    const api_credentials& credentials,
    const std::string& session_key,
    const scrobble_record& record
) {
    if (record.started_at_unix <= 0) {
        throw std::invalid_argument(
            "Last.fm scrobble start timestamp is invalid"
        );
    }

    api_parameters parameters{{"method", "track.scrobble"}};
    append_track_parameters(parameters, record.track);
    parameters.emplace_back(
        "timestamp",
        std::to_string(record.started_at_unix)
    );
    return finish_request(
        std::move(parameters),
        credentials,
        session_key
    );
}

} // namespace lastfm_scrobbler
