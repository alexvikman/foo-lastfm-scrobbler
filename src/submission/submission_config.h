#pragma once

#include "submission_queue.h"

namespace lastfm_scrobbler::submission_config {

[[nodiscard]] persisted_scrobble_queue load();
void save(const persisted_scrobble_queue& queue);
void clear();

} // namespace lastfm_scrobbler::submission_config
