#include "submission_config.h"

#include "submission_queue.h"
#include "../auth/secure_session.h"

#include <foobar2000/SDK/foobar2000.h>

#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace lastfm_scrobbler::submission_config {
namespace {

constexpr GUID queue_blob_guid = {
    0x8f5b221d,
    0x0e5b,
    0x4bd5,
    {0xa2, 0x44, 0x12, 0xd8, 0x53, 0x83, 0x79, 0x36},
};
constexpr std::size_t maximum_protected_blob_size = 32 * 1024 * 1024;

void wipe(std::vector<std::uint8_t>& bytes) noexcept {
    if (!bytes.empty()) {
        SecureZeroMemory(bytes.data(), bytes.size());
    }
}

class encrypted_queue_config final : public cfg_var {
public:
    explicit encrypted_queue_config(const GUID& guid) : cfg_var(guid) {}

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
            stream->read_object(
                replacement.data(),
                replacement.size(),
                abort
            );
        }
        bytes_ = std::move(replacement);
    }

    std::vector<std::uint8_t> bytes_;
};

encrypted_queue_config encrypted_queue_blob(queue_blob_guid);

} // namespace

persisted_scrobble_queue load() {
    const auto& blob = encrypted_queue_blob.get();
    if (blob.empty()) {
        return {};
    }

    auto plaintext = unprotect_for_current_user(blob);
    try {
        auto records = deserialize_scrobble_queue(plaintext);
        wipe(plaintext);
        return records;
    } catch (...) {
        wipe(plaintext);
        throw;
    }
}

void save(const persisted_scrobble_queue& queue) {
    if (queue.records.empty()) {
        clear();
        return;
    }

    auto plaintext = serialize_scrobble_queue(queue);
    try {
        auto ciphertext = protect_for_current_user(plaintext);
        wipe(plaintext);
        encrypted_queue_blob.set(std::move(ciphertext));
    } catch (...) {
        wipe(plaintext);
        throw;
    }
}

void clear() {
    encrypted_queue_blob.clear();
}

} // namespace lastfm_scrobbler::submission_config
