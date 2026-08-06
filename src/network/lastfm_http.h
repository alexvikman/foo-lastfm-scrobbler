#pragma once

#include "../auth/api_signature.h"
#include "../auth/secure_session.h"
#include "../submission/submission_record.h"

#include "lastfm_protocol.h"

#include <foobar2000/SDK/foobar2000.h>

#include <string>

namespace lastfm_scrobbler {

[[nodiscard]] std::string request_auth_token(
    const api_credentials& credentials,
    abort_callback& abort
);

[[nodiscard]] session_credentials request_web_service_session(
    const api_credentials& credentials,
    const std::string& token,
    abort_callback& abort
);

[[nodiscard]] lastfm_submission_response request_now_playing(
    const api_credentials& credentials,
    const std::string& session_key,
    const track_metadata& track,
    abort_callback& abort
);

[[nodiscard]] lastfm_submission_response request_scrobble(
    const api_credentials& credentials,
    const std::string& session_key,
    const scrobble_record& record,
    abort_callback& abort
);

} // namespace lastfm_scrobbler
