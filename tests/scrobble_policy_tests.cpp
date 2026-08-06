#include "../src/auth/api_signature.h"
#include "../src/auth/auth_flow.h"
#include "../src/auth/secure_session.h"
#include "../src/core/scrobble_policy.h"
#include "../src/core/track_metadata.h"
#include "../src/network/lastfm_protocol.h"
#include "../src/submission/submission_protocol.h"
#include "../src/submission/submission_queue.h"
#include "../src/submission/retry_policy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <span>
#include <string_view>
#include <string>
#include <system_error>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

void expect_near(double actual, double expected, std::string_view description) {
    expect(std::abs(actual - expected) < 0.0001, description);
}

void test_policy_boundaries() {
    using namespace lastfm_scrobbler;

    expect_near(required_listening_seconds(30.0), 0.0,
        "a 30 second track is too short");
    expect_near(required_listening_seconds(31.0), 15.5,
        "a track longer than 30 seconds uses half its duration");
    expect_near(required_listening_seconds(600.0), 240.0,
        "long tracks use the four minute cap");
    expect_near(required_listening_seconds(
        std::numeric_limits<double>::quiet_NaN()), 0.0,
        "invalid duration is rejected");

    expect(!is_scrobble_eligible(200.0, 99.999),
        "listening below the threshold is not eligible");
    expect(is_scrobble_eligible(200.0, 100.0),
        "the exact threshold is eligible");
}

void test_pause_resume_and_seek() {
    using namespace lastfm_scrobbler;

    listening_session session;
    session.start(120.0);
    session.record_played_interval(25.0);
    session.pause();
    session.record_played_interval(30.0);

    expect_near(session.listened_seconds(), 25.0,
        "paused time is ignored");

    session.seek(90.0);
    expect_near(session.listened_seconds(), 25.0,
        "seeking forward does not add listening time");
    expect_near(session.playback_position_seconds(), 90.0,
        "seek updates playback position");

    session.resume();
    session.record_played_interval(35.0);
    expect(session.ready_to_scrobble(),
        "active time before and after a pause is accumulated");
}

void test_submission_is_idempotent() {
    using namespace lastfm_scrobbler;

    listening_session session;
    session.start(600.0);
    session.record_played_interval(240.0);

    expect(session.mark_submitted(),
        "an eligible session can be submitted");
    expect(!session.mark_submitted(),
        "the same session cannot be submitted twice");
    expect(session.was_submitted(),
        "submission state is retained");
    expect(!session.ready_to_scrobble(),
        "a submitted session is no longer ready");

    session.start(100.0);
    expect(!session.was_submitted(),
        "starting a new track resets submission state");
    expect(!session.mark_submitted(),
        "a new track cannot be submitted before its threshold");
}

void test_stopped_and_invalid_intervals() {
    using namespace lastfm_scrobbler;

    listening_session session;
    session.start(100.0);
    session.record_played_interval(-5.0);
    session.record_played_interval(
        std::numeric_limits<double>::infinity());
    session.stop();
    session.record_played_interval(50.0);

    expect_near(session.listened_seconds(), 0.0,
        "invalid and stopped intervals are ignored");
}

void test_required_metadata_and_placeholders() {
    using namespace lastfm_scrobbler;

    auto source = source_track_metadata{
        .artist = "Artist",
        .title = "Title",
    };
    expect(static_cast<bool>(normalize_track_metadata(source)),
        "artist and title are sufficient metadata");

    source.artist.clear();
    auto result = normalize_track_metadata(source);
    expect(result.error == metadata_error::missing_artist,
        "a missing artist is rejected without a filename fallback");

    source.artist = "Unknown Artist";
    result = normalize_track_metadata(source);
    expect(result.error == metadata_error::filtered_artist,
        "Last.fm's known unknown-artist placeholder is rejected");

    source.artist = "Artist";
    source.title = "[unknown track]";
    result = normalize_track_metadata(source);
    expect(result.error == metadata_error::filtered_title,
        "a known unknown-track placeholder is rejected");
}

void test_metadata_normalization() {
    using namespace lastfm_scrobbler;

    const auto result = normalize_track_metadata({
        .artist = "  Björk\t  ",
        .title = "Jóga\r\nLive",
        .album = "  Homogenic  ",
        .album_artist = "BJÖRK",
        .track_number = "07/12",
        .musicbrainz_track_id = "A1B2C3D4-1111-2222-3333-ABCDEF123456",
        .duration_seconds = 301.25,
    });

    expect(static_cast<bool>(result),
        "valid UTF-8 metadata is accepted");
    if (!result.value.has_value()) {
        return;
    }

    const auto& track = *result.value;
    expect(track.artist == "Björk",
        "outer whitespace is removed without changing UTF-8");
    expect(track.title == "Jóga Live",
        "embedded ASCII whitespace is collapsed");
    expect(track.album == "Homogenic",
        "optional album is normalized");
    expect(track.album_artist == "BJÖRK",
        "non-ASCII artist casing is preserved");
    expect(track.track_number == std::uint32_t{7},
        "the numeric part of a slash track number is parsed");
    expect(track.musicbrainz_track_id ==
        "a1b2c3d4-1111-2222-3333-abcdef123456",
        "a valid MusicBrainz UUID is normalized to lowercase");
    expect_near(track.duration_seconds, 301.25,
        "positive finite duration is retained");
}

void test_optional_metadata_is_safely_omitted() {
    using namespace lastfm_scrobbler;

    const auto result = normalize_track_metadata({
        .artist = "Same Artist",
        .title = "Song",
        .album_artist = "same artist",
        .track_number = "side A",
        .musicbrainz_track_id = "not-a-musicbrainz-id",
        .duration_seconds = std::numeric_limits<double>::infinity(),
    });

    expect(static_cast<bool>(result),
        "invalid optional fields do not discard a valid track");
    if (!result.value.has_value()) {
        return;
    }

    const auto& track = *result.value;
    expect(track.album_artist.empty(),
        "album artist equal to track artist is omitted");
    expect(!track.track_number.has_value(),
        "a non-numeric track number is omitted");
    expect(track.musicbrainz_track_id.empty(),
        "an invalid MusicBrainz ID is omitted");
    expect_near(track.duration_seconds, 0.0,
        "an invalid optional duration is omitted");
}

void test_lastfm_signature() {
    using namespace lastfm_scrobbler;

    const auto parameters = api_parameters{
        {"token", "0123456789abcdef0123456789abcdef"},
        {"format", "json"},
        {"method", "auth.getSession"},
        {"api_key", "0123456789abcdef0123456789abcdef"},
        {"callback", "ignored"},
    };

    expect(make_signature_source(parameters) ==
        "api_key0123456789abcdef0123456789abcdef"
        "methodauth.getSession"
        "token0123456789abcdef0123456789abcdef",
        "signature parameters are ASCII-sorted and omit format/callback");
    expect(make_api_signature(parameters, "mysecret") ==
        "fc8a17f947209a48c0190f9d536af10a",
        "Last.fm signature uses the expected lowercase MD5 digest");

    bool duplicate_rejected = false;
    try {
        static_cast<void>(make_signature_source({
            {"method", "one"},
            {"method", "two"},
        }));
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    expect(duplicate_rejected,
        "duplicate parameter names are rejected before signing");
}

void test_auth_request_builders() {
    using namespace lastfm_scrobbler;

    const api_credentials credentials{
        .api_key = "0123456789abcdef0123456789abcdef",
        .shared_secret = "shared-secret",
    };
    const auto token_request = make_get_token_request(credentials);
    expect(token_request.api_signature.size() == 32,
        "getToken request has a 32-character signature");

    const auto session_request = make_get_session_request(
        credentials,
        "request-token"
    );
    expect(session_request.api_signature.size() == 32,
        "getSession request has a 32-character signature");

    expect(make_authorization_url("key/part", "tok en") ==
        "https://www.last.fm/api/auth/?api_key=key%2Fpart&token=tok%20en",
        "authorization URL uses HTTPS and RFC 3986 escaping");
}

void test_lastfm_form_and_json_protocol() {
    using namespace lastfm_scrobbler;

    const signed_api_request request{
        .parameters = {
            {"method", "auth.getToken"},
            {"format", "json"},
            {"note", "Björk & more"},
        },
        .api_signature = "0123456789abcdef0123456789abcdef",
    };
    expect(make_form_body(request) ==
        "method=auth.getToken&format=json&note="
        "Bj%C3%B6rk%20%26%20more&api_sig="
        "0123456789abcdef0123456789abcdef",
        "Last.fm POST fields use deterministic UTF-8 form encoding");

    expect(parse_token_response(
        R"({"token":"0123456789abcdefghijklmnopqrstuv"})"
    ) == "0123456789abcdefghijklmnopqrstuv",
        "the JSON token response accepts Last.fm's non-hex ASCII tokens");

    const auto session = parse_session_response(
        R"({"session":{"subscriber":"0","key":"0123456789abcdefghijklmnopqrstuv","name":"Bj\u00f6rk"}})"
    );
    expect(session.username == "Björk",
        "JSON Unicode escapes are decoded as UTF-8");
    expect(session.session_key == "0123456789abcdefghijklmnopqrstuv",
        "the persistent non-hex ASCII session key is parsed");

    bool api_error_reported = false;
    try {
        static_cast<void>(parse_session_response(
            R"({"error":14,"message":"This token has not been authorized"})"
        ));
    } catch (const lastfm_api_error& error) {
        api_error_reported = error.code() == 14;
    }
    expect(api_error_reported,
        "Last.fm JSON errors retain their numeric API code");

    bool malformed_response_rejected = false;
    try {
        static_cast<void>(parse_token_response(
            R"({"token":"too-short"} trailing)"
        ));
    } catch (const std::invalid_argument&) {
        malformed_response_rejected = true;
    }
    expect(malformed_response_rejected,
        "malformed or trailing JSON is rejected");
}

[[nodiscard]] std::string parameter_value(
    const lastfm_scrobbler::api_parameters& parameters,
    std::string_view name
) {
    for (const auto& [candidate, value] : parameters) {
        if (candidate == name) {
            return value;
        }
    }
    return {};
}

void test_submission_request_builders() {
    using namespace lastfm_scrobbler;

    const api_credentials credentials{
        .api_key = "0123456789abcdef0123456789abcdef",
        .shared_secret = "shared-secret",
    };
    const track_metadata track{
        .artist = "Artist",
        .title = "Title",
        .album = "Album",
        .album_artist = "Album Artist",
        .track_number = 7,
        .musicbrainz_track_id =
            "a1b2c3d4-1111-2222-3333-abcdef123456",
        .duration_seconds = 301.4,
    };

    const auto now_playing = make_now_playing_request(
        credentials,
        "0123456789abcdefghijklmnopqrstuv",
        track
    );
    expect(parameter_value(now_playing.parameters, "method") ==
        "track.updateNowPlaying",
        "now-playing uses the correct Last.fm method");
    expect(parameter_value(now_playing.parameters, "duration") == "301",
        "now-playing duration is rounded to whole seconds");
    expect(parameter_value(now_playing.parameters, "trackNumber") == "7",
        "now-playing includes normalized track number metadata");
    expect(parameter_value(now_playing.parameters, "sk") ==
        "0123456789abcdefghijklmnopqrstuv",
        "now-playing includes the persistent session key");
    expect(now_playing.api_signature.size() == 32,
        "now-playing request is signed");

    const auto scrobble = make_scrobble_request(
        credentials,
        "0123456789abcdefghijklmnopqrstuv",
        {.track = track, .started_at_unix = 1'700'000'000}
    );
    expect(parameter_value(scrobble.parameters, "method") ==
        "track.scrobble",
        "scrobble uses the correct Last.fm method");
    expect(parameter_value(scrobble.parameters, "timestamp") == "1700000000",
        "scrobble sends the UTC track-start timestamp");
    expect(scrobble.api_signature.size() == 32,
        "scrobble request is signed");
}

void test_submission_responses() {
    using namespace lastfm_scrobbler;

    const auto now_playing = parse_now_playing_response(
        R"({"nowplaying":{"ignoredMessage":{"#text":"","code":"0"},"track":{"#text":"Title"}}})"
    );
    expect(now_playing.accepted,
        "a zero ignoredMessage code accepts now-playing");

    const auto accepted = parse_scrobble_response(
        R"({"scrobbles":{"@attr":{"accepted":"1","ignored":"0"},"scrobble":{"ignoredMessage":{"#text":"","code":"0"}}}})"
    );
    expect(accepted.accepted,
        "an accepted single-scrobble response is recognized");

    const auto ignored = parse_scrobble_response(
        R"({"scrobbles":{"@attr":{"accepted":"0","ignored":"1"},"scrobble":{"ignoredMessage":{"#text":"Timestamp too old","code":"1"}}}})"
    );
    expect(!ignored.accepted && ignored.ignored_code == 1 &&
        ignored.ignored_message == "Timestamp too old",
        "an ignored scrobble retains its code and explanation");

    bool inconsistent_rejected = false;
    try {
        static_cast<void>(parse_scrobble_response(
            R"({"scrobbles":{"@attr":{"accepted":"1","ignored":"0"},"scrobble":{"ignoredMessage":{"#text":"bad","code":"1"}}}})"
        ));
    } catch (const std::invalid_argument&) {
        inconsistent_rejected = true;
    }
    expect(inconsistent_rejected,
        "inconsistent Last.fm scrobble counts are rejected");
}

void test_scrobble_queue_serialization() {
    using namespace lastfm_scrobbler;

    const std::vector<scrobble_record> original{
        {
            .track = {
                .artist = "First Artist",
                .title = "First Title",
                .album = "Album",
                .track_number = 3,
                .duration_seconds = 123.5,
            },
            .started_at_unix = 1'700'000'000,
        },
        {
            .track = {
                .artist = "Second Artist",
                .title = "Second Title",
                .album_artist = "Various Artists",
                .musicbrainz_track_id =
                    "a1b2c3d4-1111-2222-3333-abcdef123456",
                .duration_seconds = 456.0,
            },
            .started_at_unix = 1'700'000'500,
        },
    };

    const persisted_scrobble_queue queue{
        .owner_username = "test-user",
        .records = original,
    };
    auto payload = serialize_scrobble_queue(queue);
    expect(deserialize_scrobble_queue(payload) == queue,
        "account-bound queued scrobbles survive a versioned round trip in order");

    const auto owner_size = queue.owner_username.size();
    std::vector<std::uint8_t> legacy_payload(
        payload.begin(),
        payload.begin() + 5
    );
    legacy_payload[4] = 1;
    legacy_payload.insert(
        legacy_payload.end(),
        payload.begin() + 5 + 4 + owner_size,
        payload.end()
    );
    const auto migrated = deserialize_scrobble_queue(legacy_payload);
    expect(migrated.owner_username.empty() && migrated.records == original,
        "version 1 queues load without an owner so the connected account can adopt them");

    payload.push_back(0xff);
    bool corruption_rejected = false;
    try {
        static_cast<void>(deserialize_scrobble_queue(payload));
    } catch (const std::invalid_argument&) {
        corruption_rejected = true;
    }
    expect(corruption_rejected,
        "trailing queue corruption is rejected");
}

void test_retry_backoff() {
    using namespace lastfm_scrobbler;

    expect(retry_delay_seconds(0) == 0,
        "zero failures have no retry delay");
    expect(retry_delay_seconds(1) == 30,
        "the first temporary failure retries after 30 seconds");
    expect(retry_delay_seconds(2) == 120,
        "the second temporary failure retries after two minutes");
    expect(retry_delay_seconds(3) == 600,
        "the third temporary failure retries after ten minutes");
    expect(retry_delay_seconds(4) == 1800 &&
        retry_delay_seconds(100) == 1800,
        "later temporary failures are capped at thirty minutes");
}

void test_auth_flow_state_and_expiry() {
    using namespace lastfm_scrobbler;
    using namespace std::chrono_literals;

    auth_flow flow;
    flow.configure(false, false);
    expect(flow.state() == auth_state::unconfigured,
        "auth is unconfigured without API credentials");
    expect(!flow.begin_token_request(),
        "an unconfigured auth flow cannot start");

    flow.configure(true, false);
    expect(flow.begin_token_request(),
        "a configured disconnected flow can request a token");
    const auto issued = auth_flow::clock::time_point{1000s};
    expect(flow.receive_request_token("temporary-token", issued),
        "a token response advances to browser authorization");
    expect(flow.request_token_is_valid(issued + 59min + 59s),
        "request token is valid before 60 minutes");
    expect(!flow.request_token_is_valid(issued + 60min),
        "request token expires at 60 minutes");
    expect(!flow.begin_session_request(issued + 60min),
        "an expired token cannot create a session");
    expect(flow.state() == auth_state::disconnected,
        "expiry returns the auth flow to disconnected state");

    expect(flow.begin_token_request(),
        "a fresh auth attempt can start after expiry");
    expect(flow.receive_request_token("fresh-token", issued),
        "the fresh request token is accepted");
    expect(flow.begin_session_request(issued + 5min),
        "an authorized, unexpired token can request a session");
    flow.fail_request();
    expect(flow.state() == auth_state::awaiting_user_authorization,
        "a failed session exchange retains the authorized request token");
    expect(flow.begin_session_request(issued + 6min),
        "session exchange can be retried without browser authorization again");
    expect(flow.complete_session_request(),
        "a successful session response marks the account connected");
    expect(flow.request_token().empty(),
        "temporary token is erased after session creation");
}

void test_dpapi_session_round_trip() {
    using namespace lastfm_scrobbler;

    const session_credentials original{
        "Björk-fan",
        "0123456789abcdef0123456789abcdef"
    };
    auto encrypted = protect_session_credentials(original);
    expect(!encrypted.empty(),
        "DPAPI produces a protected session blob");

    const auto session_key_bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(original.session_key.data()),
        original.session_key.size()
    );
    expect(std::search(
        encrypted.begin(),
        encrypted.end(),
        session_key_bytes.begin(),
        session_key_bytes.end()
    ) == encrypted.end(),
        "protected blob does not contain the plaintext session key");

    expect(unprotect_session_credentials(encrypted) == original,
        "DPAPI round-trip restores username and session key");

    encrypted.back() ^= 0xff;
    bool corruption_rejected = false;
    try {
        static_cast<void>(unprotect_session_credentials(encrypted));
    } catch (const std::system_error&) {
        corruption_rejected = true;
    }
    expect(corruption_rejected,
        "DPAPI integrity check rejects a modified session blob");
}

} // namespace

int main() {
    test_policy_boundaries();
    test_pause_resume_and_seek();
    test_submission_is_idempotent();
    test_stopped_and_invalid_intervals();
    test_required_metadata_and_placeholders();
    test_metadata_normalization();
    test_optional_metadata_is_safely_omitted();
    test_lastfm_signature();
    test_auth_request_builders();
    test_lastfm_form_and_json_protocol();
    test_submission_request_builders();
    test_submission_responses();
    test_scrobble_queue_serialization();
    test_retry_backoff();
    test_auth_flow_state_and_expiry();
    test_dpapi_session_round_trip();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "All core tests passed\n";
    return EXIT_SUCCESS;
}
