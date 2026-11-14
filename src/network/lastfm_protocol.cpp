#include "lastfm_protocol.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lastfm_scrobbler {
namespace {

constexpr std::size_t maximum_response_size = 64 * 1024;
constexpr unsigned maximum_json_depth = 32;
constexpr std::size_t maximum_username_size = 256;
constexpr std::size_t maximum_display_message_size = 256;

[[nodiscard]] bool is_control_byte(unsigned char byte) noexcept {
    return byte < 0x20 || byte == 0x7f;
}

// The account name is stored, shown in the preferences page and binds the
// offline queue to an account, so it must be a short single-line value.
[[nodiscard]] bool is_plausible_username(std::string_view value) noexcept {
    if (value.empty() || value.size() > maximum_username_size) {
        return false;
    }
    for (const auto raw_byte : value) {
        if (is_control_byte(static_cast<unsigned char>(raw_byte))) {
            return false;
        }
    }
    return true;
}

// Text supplied by the server ends up in the console and the preferences
// page. Keep it bounded and on one line without splitting a UTF-8 sequence.
[[nodiscard]] std::string sanitize_message(std::string_view value) {
    const auto kept = std::min(value.size(), maximum_display_message_size);
    std::string output;
    output.reserve(kept + 3);
    for (const auto raw_byte : value.substr(0, kept)) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        output.push_back(is_control_byte(byte) ? ' ' : raw_byte);
    }

    if (kept < value.size()) {
        while (!output.empty() &&
            (static_cast<unsigned char>(output.back()) & 0xc0) == 0x80) {
            output.pop_back();
        }
        if (!output.empty() &&
            static_cast<unsigned char>(output.back()) >= 0xc0) {
            output.pop_back();
        }
        output.append("...");
    }
    return output;
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

[[nodiscard]] bool is_hex_32(std::string_view value) noexcept {
    if (value.size() != 32) {
        return false;
    }
    for (const auto raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        if (std::isxdigit(byte) == 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_authentication_value(std::string_view value) noexcept {
    if (value.size() != 32) {
        return false;
    }
    for (const auto raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        if (byte < 0x21 || byte > 0x7e) {
            return false;
        }
    }
    return true;
}

struct response_fields {
    std::optional<std::string> token;
    std::optional<std::string> username;
    std::optional<std::string> session_key;
    std::optional<int> error_code;
    std::optional<std::string> error_message;
    std::optional<bool> now_playing_object;
    std::optional<bool> scrobbles_object;
    std::optional<unsigned> accepted_count;
    std::optional<unsigned> ignored_count;
    std::optional<unsigned> now_playing_ignored_code;
    std::optional<unsigned> scrobble_ignored_code;
    std::optional<std::string> now_playing_ignored_message;
    std::optional<std::string> scrobble_ignored_message;
};

class json_response_reader final {
public:
    explicit json_response_reader(std::string_view input) : input_(input) {
        if (input_.empty() || input_.size() > maximum_response_size) {
            throw std::invalid_argument("Last.fm JSON response size is invalid");
        }
    }

    [[nodiscard]] response_fields parse() {
        skip_whitespace();
        if (peek() != '{') {
            throw std::invalid_argument("Last.fm JSON response is not an object");
        }
        parse_object({}, 0);
        skip_whitespace();
        if (offset_ != input_.size()) {
            throw std::invalid_argument("Last.fm JSON response has trailing data");
        }
        return std::move(fields_);
    }

private:
    void skip_whitespace() noexcept {
        while (offset_ < input_.size()) {
            const auto byte = input_[offset_];
            if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') {
                break;
            }
            ++offset_;
        }
    }

    [[nodiscard]] char peek() const noexcept {
        return offset_ < input_.size() ? input_[offset_] : '\0';
    }

    void require(char expected) {
        if (peek() != expected) {
            throw std::invalid_argument("Last.fm JSON response is malformed");
        }
        ++offset_;
    }

    [[nodiscard]] std::uint32_t read_hex_quad() {
        if (offset_ > input_.size() || input_.size() - offset_ < 4) {
            throw std::invalid_argument("Last.fm JSON Unicode escape is truncated");
        }

        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            const auto byte = input_[offset_++];
            value <<= 4;
            if (byte >= '0' && byte <= '9') {
                value |= static_cast<std::uint32_t>(byte - '0');
            } else if (byte >= 'a' && byte <= 'f') {
                value |= static_cast<std::uint32_t>(byte - 'a' + 10);
            } else if (byte >= 'A' && byte <= 'F') {
                value |= static_cast<std::uint32_t>(byte - 'A' + 10);
            } else {
                throw std::invalid_argument("Last.fm JSON Unicode escape is invalid");
            }
        }
        return value;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0x10ffff) {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            throw std::invalid_argument("Last.fm JSON codepoint is invalid");
        }
    }

    [[nodiscard]] std::string parse_string() {
        require('"');
        std::string output;
        while (offset_ < input_.size()) {
            const auto byte = static_cast<unsigned char>(input_[offset_++]);
            if (byte == '"') {
                return output;
            }
            if (byte < 0x20) {
                throw std::invalid_argument("Last.fm JSON string has a control byte");
            }
            if (byte != '\\') {
                output.push_back(static_cast<char>(byte));
                continue;
            }

            if (offset_ >= input_.size()) {
                throw std::invalid_argument("Last.fm JSON escape is truncated");
            }
            const auto escape = input_[offset_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                auto codepoint = read_hex_quad();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (offset_ > input_.size() || input_.size() - offset_ < 6 ||
                        input_[offset_] != '\\' || input_[offset_ + 1] != 'u') {
                        throw std::invalid_argument("Last.fm JSON surrogate pair is invalid");
                    }
                    offset_ += 2;
                    const auto low = read_hex_quad();
                    if (low < 0xdc00 || low > 0xdfff) {
                        throw std::invalid_argument("Last.fm JSON surrogate pair is invalid");
                    }
                    codepoint = 0x10000 +
                        ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    throw std::invalid_argument("Last.fm JSON surrogate pair is invalid");
                }
                append_utf8(output, codepoint);
                break;
            }
            default:
                throw std::invalid_argument("Last.fm JSON escape is invalid");
            }
        }
        throw std::invalid_argument("Last.fm JSON string is unterminated");
    }

    [[nodiscard]] std::string child_path(
        std::string_view parent,
        std::string_view key
    ) const {
        if (parent.empty()) {
            return std::string(key);
        }
        std::string output(parent);
        output.push_back('.');
        output.append(key);
        return output;
    }

    template<typename Value>
    static void set_unique(
        std::optional<Value>& target,
        Value value
    ) {
        if (target.has_value()) {
            throw std::invalid_argument("Last.fm JSON response has duplicate fields");
        }
        target = std::move(value);
    }

    static void capture_unsigned(
        std::optional<unsigned>& target,
        std::string_view value
    ) {
        unsigned parsed = 0;
        const auto result = std::from_chars(
            value.data(),
            value.data() + value.size(),
            parsed
        );
        if (result.ec != std::errc{} ||
            result.ptr != value.data() + value.size()) {
            throw std::invalid_argument(
                "Last.fm JSON response count is invalid"
            );
        }
        set_unique(target, parsed);
    }

    void capture_object(std::string_view path) {
        if (path == "nowplaying") {
            set_unique(fields_.now_playing_object, true);
        } else if (path == "scrobbles") {
            set_unique(fields_.scrobbles_object, true);
        }
    }

    void capture_string(std::string_view path, std::string value) {
        if (path == "token") {
            set_unique(fields_.token, std::move(value));
        } else if (path == "session.name") {
            set_unique(fields_.username, std::move(value));
        } else if (path == "session.key") {
            set_unique(fields_.session_key, std::move(value));
        } else if (path == "message") {
            set_unique(fields_.error_message, std::move(value));
        } else if (path == "scrobbles.@attr.accepted") {
            capture_unsigned(fields_.accepted_count, value);
        } else if (path == "scrobbles.@attr.ignored") {
            capture_unsigned(fields_.ignored_count, value);
        } else if (path == "nowplaying.ignoredMessage.code") {
            capture_unsigned(fields_.now_playing_ignored_code, value);
        } else if (path == "scrobbles.scrobble.ignoredMessage.code") {
            capture_unsigned(fields_.scrobble_ignored_code, value);
        } else if (path == "nowplaying.ignoredMessage.#text") {
            set_unique(
                fields_.now_playing_ignored_message,
                std::move(value)
            );
        } else if (path == "scrobbles.scrobble.ignoredMessage.#text") {
            set_unique(fields_.scrobble_ignored_message, std::move(value));
        }
    }

    void capture_number(std::string_view path, std::string_view value) {
        if (path == "error") {
            int code = 0;
            const auto result = std::from_chars(
                value.data(),
                value.data() + value.size(),
                code
            );
            if (result.ec != std::errc{} ||
                result.ptr != value.data() + value.size()) {
                throw std::invalid_argument("Last.fm error code is invalid");
            }
            set_unique(fields_.error_code, code);
        } else if (path == "scrobbles.@attr.accepted") {
            capture_unsigned(fields_.accepted_count, value);
        } else if (path == "scrobbles.@attr.ignored") {
            capture_unsigned(fields_.ignored_count, value);
        } else if (path == "nowplaying.ignoredMessage.code") {
            capture_unsigned(fields_.now_playing_ignored_code, value);
        } else if (path == "scrobbles.scrobble.ignoredMessage.code") {
            capture_unsigned(fields_.scrobble_ignored_code, value);
        }
    }

    void parse_number(std::string_view path) {
        const auto start = offset_;
        if (peek() == '-') {
            ++offset_;
        }
        if (peek() == '0') {
            ++offset_;
        } else {
            if (peek() < '1' || peek() > '9') {
                throw std::invalid_argument("Last.fm JSON number is invalid");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++offset_;
            }
        }
        if (peek() == '.') {
            ++offset_;
            if (peek() < '0' || peek() > '9') {
                throw std::invalid_argument("Last.fm JSON number is invalid");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++offset_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++offset_;
            if (peek() == '+' || peek() == '-') {
                ++offset_;
            }
            if (peek() < '0' || peek() > '9') {
                throw std::invalid_argument("Last.fm JSON number is invalid");
            }
            while (peek() >= '0' && peek() <= '9') {
                ++offset_;
            }
        }
        capture_number(path, input_.substr(start, offset_ - start));
    }

    void parse_literal(std::string_view literal) {
        if (offset_ > input_.size() || input_.size() - offset_ < literal.size() ||
            input_.substr(offset_, literal.size()) != literal) {
            throw std::invalid_argument("Last.fm JSON literal is invalid");
        }
        offset_ += literal.size();
    }

    void parse_array(std::string_view path, unsigned depth) {
        if (depth >= maximum_json_depth) {
            throw std::invalid_argument("Last.fm JSON response is too deeply nested");
        }
        require('[');
        skip_whitespace();
        if (peek() == ']') {
            ++offset_;
            return;
        }
        for (;;) {
            parse_value(path, depth + 1);
            skip_whitespace();
            if (peek() == ']') {
                ++offset_;
                return;
            }
            require(',');
            skip_whitespace();
        }
    }

    void parse_object(std::string_view path, unsigned depth) {
        if (depth >= maximum_json_depth) {
            throw std::invalid_argument("Last.fm JSON response is too deeply nested");
        }
        capture_object(path);
        require('{');
        skip_whitespace();
        if (peek() == '}') {
            ++offset_;
            return;
        }
        for (;;) {
            if (peek() != '"') {
                throw std::invalid_argument("Last.fm JSON object key is invalid");
            }
            const auto key = parse_string();
            skip_whitespace();
            require(':');
            skip_whitespace();
            const auto nested_path = child_path(path, key);
            parse_value(nested_path, depth + 1);
            skip_whitespace();
            if (peek() == '}') {
                ++offset_;
                return;
            }
            require(',');
            skip_whitespace();
        }
    }

    void parse_value(std::string_view path, unsigned depth) {
        if (depth > maximum_json_depth) {
            throw std::invalid_argument("Last.fm JSON response is too deeply nested");
        }
        switch (peek()) {
        case '{': parse_object(path, depth); return;
        case '[': parse_array(path, depth); return;
        case '"': capture_string(path, parse_string()); return;
        case 't': parse_literal("true"); return;
        case 'f': parse_literal("false"); return;
        case 'n': parse_literal("null"); return;
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9')) {
                parse_number(path);
                return;
            }
            throw std::invalid_argument("Last.fm JSON value is invalid");
        }
    }

    std::string_view input_;
    std::size_t offset_ = 0;
    response_fields fields_;
};

[[nodiscard]] response_fields parse_response(std::string_view json) {
    auto fields = json_response_reader(json).parse();
    if (fields.error_code.has_value()) {
        throw lastfm_api_error(
            *fields.error_code,
            sanitize_message(
                fields.error_message.value_or("Unknown Last.fm error")
            )
        );
    }
    return fields;
}

} // namespace

lastfm_api_error::lastfm_api_error(int code, std::string message) :
    std::runtime_error(
        "Last.fm error " + std::to_string(code) + ": " + message
    ),
    code_(code) {}

int lastfm_api_error::code() const noexcept {
    return code_;
}

std::string make_form_body(const signed_api_request& request) {
    if (!is_hex_32(request.api_signature)) {
        throw std::invalid_argument("Last.fm API signature is malformed");
    }

    std::string output;
    auto append_field = [&output](std::string_view name, std::string_view value) {
        if (name.empty()) {
            throw std::invalid_argument("Last.fm form field name is empty");
        }
        if (!output.empty()) {
            output.push_back('&');
        }
        output.append(percent_encode(name));
        output.push_back('=');
        output.append(percent_encode(value));
    };

    for (const auto& [name, value] : request.parameters) {
        if (name == "api_sig") {
            throw std::invalid_argument("Last.fm request already contains api_sig");
        }
        append_field(name, value);
    }
    append_field("api_sig", request.api_signature);
    return output;
}

std::string parse_token_response(std::string_view json) {
    auto fields = parse_response(json);
    if (!fields.token.has_value() ||
        !is_authentication_value(*fields.token)) {
        throw std::invalid_argument("Last.fm token response is malformed");
    }
    return std::move(*fields.token);
}

session_credentials parse_session_response(std::string_view json) {
    auto fields = parse_response(json);
    if (!fields.username.has_value() ||
        !is_plausible_username(*fields.username) ||
        !fields.session_key.has_value() ||
        !is_authentication_value(*fields.session_key)) {
        throw std::invalid_argument("Last.fm session response is malformed");
    }
    return {
        std::move(*fields.username),
        std::move(*fields.session_key),
    };
}

lastfm_submission_response parse_now_playing_response(
    std::string_view json
) {
    auto fields = parse_response(json);
    if (!fields.now_playing_object.value_or(false) ||
        !fields.now_playing_ignored_code.has_value()) {
        throw std::invalid_argument(
            "Last.fm now-playing response is malformed"
        );
    }

    return {
        .accepted = *fields.now_playing_ignored_code == 0,
        .ignored_code = *fields.now_playing_ignored_code,
        .ignored_message = sanitize_message(
            fields.now_playing_ignored_message.value_or("")
        ),
    };
}

void parse_remove_now_playing_response(std::string_view json) {
    static_cast<void>(parse_response(json));
}

lastfm_submission_response parse_scrobble_response(
    std::string_view json
) {
    auto fields = parse_response(json);
    if (!fields.scrobbles_object.value_or(false) ||
        !fields.accepted_count.has_value() ||
        !fields.ignored_count.has_value() ||
        !fields.scrobble_ignored_code.has_value() ||
        *fields.accepted_count > 1 || *fields.ignored_count > 1 ||
        *fields.accepted_count + *fields.ignored_count != 1) {
        throw std::invalid_argument(
            "Last.fm scrobble response is malformed"
        );
    }

    const auto accepted = *fields.accepted_count == 1;
    if (accepted != (*fields.scrobble_ignored_code == 0)) {
        throw std::invalid_argument(
            "Last.fm scrobble response is inconsistent"
        );
    }
    return {
        .accepted = accepted,
        .ignored_code = *fields.scrobble_ignored_code,
        .ignored_message = sanitize_message(
            fields.scrobble_ignored_message.value_or("")
        ),
    };
}

} // namespace lastfm_scrobbler
