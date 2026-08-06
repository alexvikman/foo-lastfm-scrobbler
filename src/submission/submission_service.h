#pragma once

#include "submission_record.h"

#include <cstddef>
#include <string>

namespace lastfm_scrobbler {

struct submission_diagnostics {
    std::size_t queued_scrobbles = 0;
    bool sending = false;
    bool retry_scheduled = false;
    unsigned retry_in_seconds = 0;
    std::string status;
};

void submission_initialize();
void submission_shutdown() noexcept;
void submission_account_connected(const std::string& username) noexcept;
void submission_account_disconnected() noexcept;

[[nodiscard]] submission_diagnostics submission_diagnostics_snapshot() noexcept;

void submit_now_playing(const track_metadata& track) noexcept;
void remove_now_playing() noexcept;

// Returns true only after the record has been durably added to the encrypted
// queue. Delivery then happens asynchronously.
[[nodiscard]] bool enqueue_scrobble(const scrobble_record& record) noexcept;

} // namespace lastfm_scrobbler
