#pragma once

#include "../auth/api_signature.h"
#include "../auth/secure_session.h"

#include <stdexcept>
#include <string>
#include <string_view>

namespace lastfm_scrobbler {

class lastfm_api_error final : public std::runtime_error {
public:
    lastfm_api_error(int code, std::string message);

    [[nodiscard]] int code() const noexcept;

private:
    int code_;
};

struct lastfm_submission_response {
    bool accepted = false;
    unsigned ignored_code = 0;
    std::string ignored_message;
};

[[nodiscard]] std::string make_form_body(
    const signed_api_request& request
);

[[nodiscard]] std::string parse_token_response(std::string_view json);

[[nodiscard]] session_credentials parse_session_response(
    std::string_view json
);

[[nodiscard]] lastfm_submission_response parse_now_playing_response(
    std::string_view json
);

[[nodiscard]] lastfm_submission_response parse_scrobble_response(
    std::string_view json
);

} // namespace lastfm_scrobbler
