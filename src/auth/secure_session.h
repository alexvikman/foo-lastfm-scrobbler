#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lastfm_scrobbler {

struct session_credentials {
    std::string username;
    std::string session_key;

    session_credentials() = default;
    session_credentials(std::string username_value, std::string session_key_value);
    ~session_credentials();
    session_credentials(const session_credentials& other);
    session_credentials& operator=(const session_credentials& other);
    session_credentials(session_credentials&& other) noexcept;
    session_credentials& operator=(session_credentials&& other) noexcept;

    [[nodiscard]] bool operator==(const session_credentials&) const = default;
};

[[nodiscard]] std::vector<std::uint8_t> protect_for_current_user(
    std::span<const std::uint8_t> plaintext
);

[[nodiscard]] std::vector<std::uint8_t> unprotect_for_current_user(
    std::span<const std::uint8_t> ciphertext
);

[[nodiscard]] std::vector<std::uint8_t> protect_session_credentials(
    const session_credentials& credentials
);

[[nodiscard]] session_credentials unprotect_session_credentials(
    std::span<const std::uint8_t> ciphertext
);

} // namespace lastfm_scrobbler
