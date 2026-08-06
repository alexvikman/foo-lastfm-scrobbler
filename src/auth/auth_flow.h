#pragma once

#include <chrono>
#include <string>

namespace lastfm_scrobbler {

enum class auth_state {
    unconfigured,
    disconnected,
    requesting_token,
    awaiting_user_authorization,
    requesting_session,
    connected,
    failed,
};

class auth_flow final {
public:
    using clock = std::chrono::system_clock;

    auth_flow() = default;
    ~auth_flow();
    auth_flow(const auth_flow&) = delete;
    auth_flow& operator=(const auth_flow&) = delete;

    void configure(
        bool api_credentials_available,
        bool stored_session_available
    ) noexcept;

    [[nodiscard]] bool begin_token_request() noexcept;
    [[nodiscard]] bool receive_request_token(
        std::string token,
        clock::time_point issued_at
    );
    [[nodiscard]] bool begin_session_request(clock::time_point now) noexcept;
    [[nodiscard]] bool complete_session_request() noexcept;
    void fail_request() noexcept;
    void disconnect() noexcept;

    [[nodiscard]] auth_state state() const noexcept;
    [[nodiscard]] const std::string& request_token() const noexcept;
    [[nodiscard]] bool request_token_is_valid(clock::time_point now) const noexcept;

private:
    void clear_token() noexcept;

    auth_state state_ = auth_state::unconfigured;
    bool api_credentials_available_ = false;
    std::string request_token_;
    clock::time_point token_issued_at_{};
};

} // namespace lastfm_scrobbler
