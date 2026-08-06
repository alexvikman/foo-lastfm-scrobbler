#pragma once

#include <string>
#include <utility>
#include <vector>

namespace lastfm_scrobbler {

using api_parameters = std::vector<std::pair<std::string, std::string>>;

struct api_credentials {
    std::string api_key;
    std::string shared_secret;
};

struct signed_api_request {
    api_parameters parameters;
    std::string api_signature;
};

// Produces the sorted name/value source string described by Last.fm. The
// shared secret is deliberately handled only by make_api_signature().
[[nodiscard]] std::string make_signature_source(
    const api_parameters& parameters
);

[[nodiscard]] std::string make_api_signature(
    const api_parameters& parameters,
    const std::string& shared_secret
);

[[nodiscard]] signed_api_request make_get_token_request(
    const api_credentials& credentials
);

[[nodiscard]] signed_api_request make_get_session_request(
    const api_credentials& credentials,
    const std::string& token
);

[[nodiscard]] std::string make_authorization_url(
    const std::string& api_key,
    const std::string& token
);

} // namespace lastfm_scrobbler

