#include "lastfm_http.h"

#include "lastfm_protocol.h"
#include "../submission/submission_protocol.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace lastfm_scrobbler {
namespace {

constexpr char api_endpoint[] = "https://ws.audioscrobbler.com/2.0/";
constexpr std::size_t maximum_response_size = 64 * 1024;

[[nodiscard]] std::string perform_request(
    const signed_api_request& signed_request,
    abort_callback& abort
) {
    const auto body = make_form_body(signed_request);
    auto request = http_client::get()->create_request("POST");

    http_request_post_v2::ptr post_request;
    if (!request->service_query_t(post_request)) {
        throw std::runtime_error(
            "foobar2000 HTTP client does not support form POST requests"
        );
    }

    post_request->set_post_data(
        body.data(),
        body.size(),
        "application/x-www-form-urlencoded; charset=utf-8"
    );
    request->add_header("Accept", "application/json");
    request->add_header("User-Agent", "foo_lastfm_scrobbler/0.4");

    auto response_stream = request->run_ex(api_endpoint, abort);
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
        abort.check();
        const auto bytes_read = response_stream->read(
            buffer.data(),
            buffer.size(),
            abort
        );
        if (bytes_read == 0) {
            break;
        }
        if (response.size() > maximum_response_size - bytes_read) {
            throw std::length_error("Last.fm response is too large");
        }
        response.append(buffer.data(), bytes_read);
    }

    if (response.empty()) {
        throw std::runtime_error("Last.fm returned an empty response");
    }
    return response;
}

} // namespace

std::string request_auth_token(
    const api_credentials& credentials,
    abort_callback& abort
) {
    return parse_token_response(
        perform_request(make_get_token_request(credentials), abort)
    );
}

session_credentials request_web_service_session(
    const api_credentials& credentials,
    const std::string& token,
    abort_callback& abort
) {
    return parse_session_response(
        perform_request(make_get_session_request(credentials, token), abort)
    );
}

lastfm_submission_response request_now_playing(
    const api_credentials& credentials,
    const std::string& session_key,
    const track_metadata& track,
    abort_callback& abort
) {
    return parse_now_playing_response(
        perform_request(
            make_now_playing_request(credentials, session_key, track),
            abort
        )
    );
}

lastfm_submission_response request_scrobble(
    const api_credentials& credentials,
    const std::string& session_key,
    const scrobble_record& record,
    abort_callback& abort
) {
    return parse_scrobble_response(
        perform_request(
            make_scrobble_request(credentials, session_key, record),
            abort
        )
    );
}

} // namespace lastfm_scrobbler
