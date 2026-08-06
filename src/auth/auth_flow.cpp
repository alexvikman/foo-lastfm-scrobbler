#include "auth_flow.h"

#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <utility>

namespace lastfm_scrobbler {
namespace {

constexpr auto request_token_lifetime = std::chrono::minutes{60};

} // namespace

auth_flow::~auth_flow() {
    clear_token();
}

void auth_flow::configure(
    bool api_credentials_available,
    bool stored_session_available
) noexcept {
    api_credentials_available_ = api_credentials_available;
    clear_token();
    if (!api_credentials_available_) {
        state_ = auth_state::unconfigured;
    } else if (stored_session_available) {
        state_ = auth_state::connected;
    } else {
        state_ = auth_state::disconnected;
    }
}

bool auth_flow::begin_token_request() noexcept {
    if (!api_credentials_available_ ||
        (state_ != auth_state::disconnected && state_ != auth_state::failed)) {
        return false;
    }

    clear_token();
    state_ = auth_state::requesting_token;
    return true;
}

bool auth_flow::receive_request_token(
    std::string token,
    clock::time_point issued_at
) {
    if (state_ != auth_state::requesting_token || token.empty()) {
        if (!token.empty()) {
            SecureZeroMemory(token.data(), token.size());
        }
        return false;
    }

    request_token_ = std::move(token);
    token_issued_at_ = issued_at;
    state_ = auth_state::awaiting_user_authorization;
    return true;
}

bool auth_flow::begin_session_request(clock::time_point now) noexcept {
    if (state_ != auth_state::awaiting_user_authorization ||
        !request_token_is_valid(now)) {
        if (state_ == auth_state::awaiting_user_authorization) {
            clear_token();
            state_ = auth_state::disconnected;
        }
        return false;
    }

    state_ = auth_state::requesting_session;
    return true;
}

bool auth_flow::complete_session_request() noexcept {
    if (state_ != auth_state::requesting_session) {
        return false;
    }

    clear_token();
    state_ = auth_state::connected;
    return true;
}

void auth_flow::fail_request() noexcept {
    if (state_ == auth_state::requesting_token) {
        state_ = auth_state::failed;
    } else if (state_ == auth_state::requesting_session) {
        // Keep the one-time request token so the user can finish browser
        // authorization and retry auth.getSession without starting over.
        state_ = auth_state::awaiting_user_authorization;
    }
}

void auth_flow::disconnect() noexcept {
    clear_token();
    state_ = api_credentials_available_
        ? auth_state::disconnected
        : auth_state::unconfigured;
}

auth_state auth_flow::state() const noexcept {
    return state_;
}

const std::string& auth_flow::request_token() const noexcept {
    return request_token_;
}

bool auth_flow::request_token_is_valid(clock::time_point now) const noexcept {
    return !request_token_.empty() &&
        now >= token_issued_at_ &&
        now - token_issued_at_ < request_token_lifetime;
}

void auth_flow::clear_token() noexcept {
    if (!request_token_.empty()) {
        SecureZeroMemory(request_token_.data(), request_token_.size());
        request_token_.clear();
    }
    token_issued_at_ = {};
}

} // namespace lastfm_scrobbler
