#include "api_signature.h"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace lastfm_scrobbler {
namespace {

class algorithm_handle final {
public:
    algorithm_handle() {
        if (BCryptOpenAlgorithmProvider(
                &handle_,
                BCRYPT_MD5_ALGORITHM,
                nullptr,
                0
            ) < 0) {
            throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
        }
    }

    ~algorithm_handle() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }

    algorithm_handle(const algorithm_handle&) = delete;
    algorithm_handle& operator=(const algorithm_handle&) = delete;

    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept {
        return handle_;
    }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class hash_handle final {
public:
    explicit hash_handle(BCRYPT_ALG_HANDLE algorithm) {
        if (BCryptCreateHash(
                algorithm,
                &handle_,
                nullptr,
                0,
                nullptr,
                0,
                0
            ) < 0) {
            throw std::runtime_error("BCryptCreateHash failed");
        }
    }

    ~hash_handle() {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }

    hash_handle(const hash_handle&) = delete;
    hash_handle& operator=(const hash_handle&) = delete;

    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept {
        return handle_;
    }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

[[nodiscard]] std::string md5_hex(std::string_view input) {
    if (input.size() > std::numeric_limits<ULONG>::max()) {
        throw std::length_error("MD5 input is too large");
    }

    algorithm_handle algorithm;
    hash_handle hash(algorithm.get());

    if (BCryptHashData(
            hash.get(),
            reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
            static_cast<ULONG>(input.size()),
            0
        ) < 0) {
        throw std::runtime_error("BCryptHashData failed");
    }

    std::array<std::uint8_t, 16> digest{};
    if (BCryptFinishHash(
            hash.get(),
            digest.data(),
            static_cast<ULONG>(digest.size()),
            0
        ) < 0) {
        throw std::runtime_error("BCryptFinishHash failed");
    }

    constexpr std::array<char, 16> hex = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string output;
    output.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        output.push_back(hex[byte >> 4]);
        output.push_back(hex[byte & 0x0f]);
    }
    return output;
}

[[nodiscard]] bool should_exclude_from_signature(
    std::string_view name
) noexcept {
    return name == "format" || name == "callback";
}

void validate_credentials(const api_credentials& credentials) {
    if (credentials.api_key.size() != 32 ||
        credentials.shared_secret.empty()) {
        throw std::invalid_argument(
            "Last.fm API credentials are missing or malformed"
        );
    }
}

[[nodiscard]] bool is_unreserved(unsigned char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') ||
        (byte >= 'a' && byte <= 'z') ||
        (byte >= '0' && byte <= '9') ||
        byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

[[nodiscard]] std::string percent_encode(std::string_view value) {
    constexpr std::array<char, 16> hex = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
    };

    std::string output;
    output.reserve(value.size());
    for (const auto raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        if (is_unreserved(byte)) {
            output.push_back(raw_byte);
        } else {
            output.push_back('%');
            output.push_back(hex[byte >> 4]);
            output.push_back(hex[byte & 0x0f]);
        }
    }
    return output;
}

} // namespace

std::string make_signature_source(const api_parameters& parameters) {
    auto sorted = parameters;
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        }
    );

    std::string output;
    std::string_view previous_name;
    for (const auto& [name, value] : sorted) {
        if (name.empty()) {
            throw std::invalid_argument("Last.fm parameter name is empty");
        }
        if (name == "api_sig") {
            throw std::invalid_argument("api_sig cannot sign itself");
        }
        if (!previous_name.empty() && name == previous_name) {
            throw std::invalid_argument("duplicate Last.fm parameter name");
        }
        previous_name = name;

        if (!should_exclude_from_signature(name)) {
            output.append(name);
            output.append(value);
        }
    }
    return output;
}

std::string make_api_signature(
    const api_parameters& parameters,
    const std::string& shared_secret
) {
    if (shared_secret.empty()) {
        throw std::invalid_argument("Last.fm shared secret is empty");
    }

    auto source = make_signature_source(parameters);
    source.append(shared_secret);
    try {
        auto signature = md5_hex(source);
        SecureZeroMemory(source.data(), source.size());
        return signature;
    } catch (...) {
        SecureZeroMemory(source.data(), source.size());
        throw;
    }
}

signed_api_request make_get_token_request(const api_credentials& credentials) {
    validate_credentials(credentials);
    signed_api_request request;
    request.parameters = {
        {"api_key", credentials.api_key},
        {"format", "json"},
        {"method", "auth.getToken"},
    };
    request.api_signature = make_api_signature(
        request.parameters,
        credentials.shared_secret
    );
    return request;
}

signed_api_request make_get_session_request(
    const api_credentials& credentials,
    const std::string& token
) {
    validate_credentials(credentials);
    if (token.empty()) {
        throw std::invalid_argument("Last.fm request token is empty");
    }

    signed_api_request request;
    request.parameters = {
        {"api_key", credentials.api_key},
        {"format", "json"},
        {"method", "auth.getSession"},
        {"token", token},
    };
    request.api_signature = make_api_signature(
        request.parameters,
        credentials.shared_secret
    );
    return request;
}

std::string make_authorization_url(
    const std::string& api_key,
    const std::string& token
) {
    if (api_key.empty() || token.empty()) {
        throw std::invalid_argument(
            "Last.fm API key and request token are required"
        );
    }

    return "https://www.last.fm/api/auth/?api_key=" +
        percent_encode(api_key) + "&token=" + percent_encode(token);
}

} // namespace lastfm_scrobbler

