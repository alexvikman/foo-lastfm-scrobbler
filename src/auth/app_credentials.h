#pragma once

#include "api_signature.h"

namespace lastfm_scrobbler {

// The application API account identifies this component to Last.fm. Personal
// user sessions are obtained separately through the browser authorization flow.
[[nodiscard]] api_credentials app_api_credentials();
[[nodiscard]] bool app_api_credentials_available() noexcept;

} // namespace lastfm_scrobbler
