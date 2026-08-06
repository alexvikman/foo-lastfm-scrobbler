#include "session_config.h"

#include <foobar2000/SDK/foobar2000.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lastfm_scrobbler::session_config {
namespace {

constexpr GUID session_blob_guid = {
    0x90abf4d1,
    0xb7ca,
    0x4f09,
    {0x98, 0x68, 0x66, 0x86, 0xe1, 0x12, 0xb8, 0x50},
};

constexpr std::size_t maximum_protected_blob_size = 64 * 1024;

class encrypted_session_config final : public cfg_var {
public:
    explicit encrypted_session_config(const GUID& guid) : cfg_var(guid) {}

    [[nodiscard]] const std::vector<std::uint8_t>& get() const noexcept {
        return bytes_;
    }

    void set(std::vector<std::uint8_t> bytes) {
        bytes_ = std::move(bytes);
    }

    void clear() noexcept {
        bytes_.clear();
    }

private:
    void get_data_raw(
        stream_writer* stream,
        abort_callback& abort
    ) override {
        if (!bytes_.empty()) {
            stream->write_object(bytes_.data(), bytes_.size(), abort);
        }
    }

    void set_data_raw(
        stream_reader* stream,
        t_size size_hint,
        abort_callback& abort
    ) override {
        if (size_hint > maximum_protected_blob_size) {
            throw exception_io_data();
        }

        std::vector<std::uint8_t> replacement(size_hint);
        if (!replacement.empty()) {
            stream->read_object(replacement.data(), replacement.size(), abort);
        }
        bytes_ = std::move(replacement);
    }

    std::vector<std::uint8_t> bytes_;
};

encrypted_session_config encrypted_session_blob(session_blob_guid);

} // namespace

bool has_encrypted_session() noexcept {
    return !encrypted_session_blob.get().empty();
}

std::optional<session_credentials> load() {
    const auto& blob = encrypted_session_blob.get();
    if (blob.empty()) {
        return std::nullopt;
    }

    return unprotect_session_credentials(blob);
}

void save(const session_credentials& credentials) {
    encrypted_session_blob.set(protect_session_credentials(credentials));
}

void clear() {
    encrypted_session_blob.clear();
}

} // namespace lastfm_scrobbler::session_config
