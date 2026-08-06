#include "secure_session.h"

#define NOMINMAX
#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace lastfm_scrobbler {
namespace {

void wipe_string(std::string& value) noexcept {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

} // namespace

session_credentials::session_credentials(
    std::string username_value,
    std::string session_key_value
) :
    username(std::move(username_value)),
    session_key(std::move(session_key_value)) {}

session_credentials::~session_credentials() {
    wipe_string(session_key);
}

session_credentials::session_credentials(const session_credentials& other) :
    username(other.username),
    session_key(other.session_key) {}

session_credentials& session_credentials::operator=(
    const session_credentials& other
) {
    if (this != &other) {
        username = other.username;
        wipe_string(session_key);
        session_key = other.session_key;
    }
    return *this;
}

session_credentials::session_credentials(session_credentials&& other) noexcept :
    username(std::move(other.username)),
    session_key(std::move(other.session_key)) {
    wipe_string(other.session_key);
}

session_credentials& session_credentials::operator=(
    session_credentials&& other
) noexcept {
    if (this != &other) {
        username = std::move(other.username);
        wipe_string(session_key);
        session_key = std::move(other.session_key);
        wipe_string(other.session_key);
    }
    return *this;
}

namespace {

constexpr std::array<std::uint8_t, 5> payload_header = {
    'F', 'L', 'F', 'M', 1,
};
constexpr std::array<std::uint8_t, 30> optional_entropy = {
    'f', 'o', 'o', '_', 'l', 'a', 's', 't', 'f', 'm', '_',
    's', 'c', 'r', 'o', 'b', 'b', 'l', 'e', 'r', '/',
    's', 'e', 's', 's', 'i', 'o', 'n', '/', '1',
};
constexpr std::size_t maximum_field_size = 16 * 1024;

class local_data_blob final {
public:
    explicit local_data_blob(bool sensitive) noexcept : sensitive_(sensitive) {}

    ~local_data_blob() {
        if (blob_.pbData != nullptr) {
            if (sensitive_) {
                SecureZeroMemory(blob_.pbData, blob_.cbData);
            }
            LocalFree(blob_.pbData);
        }
    }

    local_data_blob(const local_data_blob&) = delete;
    local_data_blob& operator=(const local_data_blob&) = delete;

    [[nodiscard]] DATA_BLOB* out() noexcept {
        return &blob_;
    }

    [[nodiscard]] const DATA_BLOB& get() const noexcept {
        return blob_;
    }

private:
    DATA_BLOB blob_{};
    bool sensitive_ = false;
};

void wipe(std::vector<std::uint8_t>& bytes) noexcept {
    if (!bytes.empty()) {
        SecureZeroMemory(bytes.data(), bytes.size());
    }
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xff));
    output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

[[nodiscard]] std::uint32_t read_u32(
    std::span<const std::uint8_t> input,
    std::size_t& offset
) {
    if (offset > input.size() || input.size() - offset < 4) {
        throw std::invalid_argument("protected session payload is truncated");
    }

    const auto value = static_cast<std::uint32_t>(input[offset]) |
        (static_cast<std::uint32_t>(input[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(input[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(input[offset + 3]) << 24);
    offset += 4;
    return value;
}

void append_string(std::vector<std::uint8_t>& output, const std::string& value) {
    if (value.size() > maximum_field_size) {
        throw std::length_error("session credential field is too large");
    }
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

[[nodiscard]] std::string read_string(
    std::span<const std::uint8_t> input,
    std::size_t& offset
) {
    const auto size = static_cast<std::size_t>(read_u32(input, offset));
    if (size > maximum_field_size ||
        offset > input.size() || input.size() - offset < size) {
        throw std::invalid_argument("protected session field is malformed");
    }

    std::string output(
        reinterpret_cast<const char*>(input.data() + offset),
        size
    );
    offset += size;
    return output;
}

[[nodiscard]] std::vector<std::uint8_t> serialize(
    const session_credentials& credentials
) {
    if (credentials.username.empty() || credentials.session_key.empty()) {
        throw std::invalid_argument(
            "Last.fm username and session key are required"
        );
    }

    std::vector<std::uint8_t> output(
        payload_header.begin(),
        payload_header.end()
    );
    append_string(output, credentials.username);
    append_string(output, credentials.session_key);
    return output;
}

[[nodiscard]] session_credentials deserialize(
    std::span<const std::uint8_t> input
) {
    if (input.size() < payload_header.size() ||
        !std::equal(
            payload_header.begin(),
            payload_header.end(),
            input.begin()
        )) {
        throw std::invalid_argument("protected session version is unsupported");
    }

    auto offset = payload_header.size();
    session_credentials output;
    output.username = read_string(input, offset);
    output.session_key = read_string(input, offset);
    if (output.username.empty() || output.session_key.empty() ||
        offset != input.size()) {
        throw std::invalid_argument("protected session payload is malformed");
    }
    return output;
}

[[nodiscard]] DATA_BLOB make_blob(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > std::numeric_limits<DWORD>::max()) {
        throw std::length_error("DPAPI input is too large");
    }

    DATA_BLOB blob{};
    blob.cbData = static_cast<DWORD>(bytes.size());
    blob.pbData = const_cast<BYTE*>(
        reinterpret_cast<const BYTE*>(bytes.data())
    );
    return blob;
}

[[nodiscard]] DATA_BLOB make_entropy_blob() {
    return make_blob(optional_entropy);
}

} // namespace

std::vector<std::uint8_t> protect_for_current_user(
    std::span<const std::uint8_t> plaintext
) {
    if (plaintext.empty()) {
        throw std::invalid_argument("DPAPI plaintext is empty");
    }

    auto input = make_blob(plaintext);
    auto entropy = make_entropy_blob();
    local_data_blob output(false);
    if (!CryptProtectData(
            &input,
            L"foo_lastfm_scrobbler Last.fm session",
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            output.out()
        )) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "CryptProtectData failed"
        );
    }

    const auto& output_blob = output.get();
    return std::vector<std::uint8_t>(
        output_blob.pbData,
        output_blob.pbData + output_blob.cbData
    );
}

std::vector<std::uint8_t> unprotect_for_current_user(
    std::span<const std::uint8_t> ciphertext
) {
    if (ciphertext.empty()) {
        throw std::invalid_argument("DPAPI ciphertext is empty");
    }

    auto input = make_blob(ciphertext);
    auto entropy = make_entropy_blob();
    local_data_blob output(true);
    if (!CryptUnprotectData(
            &input,
            nullptr,
            &entropy,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            output.out()
        )) {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "CryptUnprotectData failed"
        );
    }

    const auto& output_blob = output.get();
    return std::vector<std::uint8_t>(
        output_blob.pbData,
        output_blob.pbData + output_blob.cbData
    );
}

std::vector<std::uint8_t> protect_session_credentials(
    const session_credentials& credentials
) {
    auto plaintext = serialize(credentials);
    try {
        auto ciphertext = protect_for_current_user(plaintext);
        wipe(plaintext);
        return ciphertext;
    } catch (...) {
        wipe(plaintext);
        throw;
    }
}

session_credentials unprotect_session_credentials(
    std::span<const std::uint8_t> ciphertext
) {
    auto plaintext = unprotect_for_current_user(ciphertext);
    try {
        auto credentials = deserialize(plaintext);
        wipe(plaintext);
        return credentials;
    } catch (...) {
        wipe(plaintext);
        throw;
    }
}

} // namespace lastfm_scrobbler
