#pragma once

#include "secure_session.h"

#include <optional>

namespace lastfm_scrobbler::session_config {

[[nodiscard]] bool has_encrypted_session() noexcept;
[[nodiscard]] std::optional<session_credentials> load();
void save(const session_credentials& credentials);
void clear();

} // namespace lastfm_scrobbler::session_config
