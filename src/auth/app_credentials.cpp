#include "app_credentials.h"

#include <string_view>

// A newly created local header must trigger recompilation of this source file.
#if __has_include("api_credentials.local.h")
#include "api_credentials.local.h"
#else
#define FOO_LASTFM_API_KEY ""
#define FOO_LASTFM_SHARED_SECRET ""
#endif

#ifndef FOO_LASTFM_API_KEY
#error "api_credentials.local.h must define FOO_LASTFM_API_KEY"
#endif

#ifndef FOO_LASTFM_SHARED_SECRET
#error "api_credentials.local.h must define FOO_LASTFM_SHARED_SECRET"
#endif

namespace lastfm_scrobbler {
namespace {

constexpr std::string_view api_key = FOO_LASTFM_API_KEY;
constexpr std::string_view shared_secret = FOO_LASTFM_SHARED_SECRET;

} // namespace

api_credentials app_api_credentials() {
    return {
        std::string(api_key),
        std::string(shared_secret),
    };
}

bool app_api_credentials_available() noexcept {
    return api_key.size() == 32 && !shared_secret.empty();
}

} // namespace lastfm_scrobbler
