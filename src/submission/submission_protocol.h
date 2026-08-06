#pragma once

#include "submission_record.h"
#include "../auth/api_signature.h"

#include <cstdint>
#include <string>

namespace lastfm_scrobbler {

[[nodiscard]] signed_api_request make_now_playing_request(
    const api_credentials& credentials,
    const std::string& session_key,
    const track_metadata& track
);

[[nodiscard]] signed_api_request make_remove_now_playing_request(
    const api_credentials& credentials,
    const std::string& session_key
);

[[nodiscard]] signed_api_request make_scrobble_request(
    const api_credentials& credentials,
    const std::string& session_key,
    const scrobble_record& record
);

} // namespace lastfm_scrobbler
