#include "submission_service.h"

#include "retry_policy.h"
#include "submission_config.h"
#include "submission_queue.h"
#include "../auth/app_credentials.h"
#include "../auth/session_config.h"
#include "../network/lastfm_http.h"
#include "../network/lastfm_protocol.h"

#include <foobar2000/SDK/foobar2000.h>

#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lastfm_scrobbler {
namespace {

enum class submission_kind {
    now_playing,
    remove_now_playing,
    scrobble,
};

class submission_task;
class retry_main_thread_callback;

class submission_dispatcher final {
public:
    void initialize();
    void shutdown() noexcept;
    void account_connected(const std::string& username);
    void account_disconnected();
    void now_playing(const track_metadata& track);
    void remove_now_playing();
    [[nodiscard]] bool enqueue(const scrobble_record& record);
    [[nodiscard]] submission_diagnostics diagnostics() const;
    void task_done(submission_task& task, bool was_aborted);

    void retry_timer_elapsed_worker() noexcept;
    void retry_timer_elapsed_main(std::uint64_t generation);

private:
    void start_next() noexcept;
    void persist_queue() noexcept;
    void invalidate_session() noexcept;
    void schedule_retry() noexcept;
    void cancel_retry_timer(bool reset_attempts) noexcept;
    void reset_retry_after_final_result() noexcept;
    [[nodiscard]] bool queue_matches_account(
        const std::string& username
    ) const noexcept;

    persisted_scrobble_queue queue_;
    std::optional<track_metadata> pending_now_playing_;
    bool pending_remove_now_playing_ = false;
    std::string status_ = "Ready.";
    PTP_TIMER retry_timer_ = nullptr;
    std::atomic_bool timer_callbacks_enabled_{false};
    std::atomic_uint64_t retry_generation_{0};
    std::uint64_t account_generation_ = 0;
    std::uint64_t queue_generation_ = 0;
    unsigned retry_attempts_ = 0;
    unsigned retry_delay_seconds_ = 0;
    bool initialized_ = false;
    bool shutting_down_ = false;
    bool active_ = false;
    bool retry_blocked_ = false;
    bool retry_scheduled_ = false;
};

[[nodiscard]] submission_dispatcher& dispatcher() {
    static submission_dispatcher instance;
    return instance;
}

void wipe_string(std::string& value) noexcept {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

class retry_main_thread_callback : public main_thread_callback {
public:
    explicit retry_main_thread_callback(std::uint64_t generation) :
        generation_(generation) {}

    void callback_run() override {
        dispatcher().retry_timer_elapsed_main(generation_);
    }

private:
    std::uint64_t generation_;
};

void CALLBACK retry_timer_callback(
    PTP_CALLBACK_INSTANCE,
    void* context,
    PTP_TIMER
) noexcept {
    static_cast<submission_dispatcher*>(context)
        ->retry_timer_elapsed_worker();
}

class submission_task : public threaded_process_callback {
public:
    submission_task(
        submission_kind kind,
        std::uint64_t account_generation,
        std::uint64_t queue_generation,
        api_credentials credentials,
        session_credentials session,
        std::optional<track_metadata> track,
        std::optional<scrobble_record> record
    ) :
        kind_(kind),
        account_generation_(account_generation),
        queue_generation_(queue_generation),
        credentials_(std::move(credentials)),
        session_(std::move(session)),
        track_(std::move(track)),
        record_(std::move(record)) {}

    ~submission_task() {
        wipe_string(credentials_.shared_secret);
    }

    void run(threaded_process_status& status, abort_callback& abort) override {
        try {
            if (kind_ == submission_kind::now_playing) {
                status.set_item("Updating Last.fm now playing...");
                if (!track_.has_value()) {
                    throw std::logic_error("Now-playing task has no track");
                }
                response_ = request_now_playing(
                    credentials_,
                    session_.session_key,
                    *track_,
                    abort
                );
            } else if (kind_ == submission_kind::remove_now_playing) {
                status.set_item("Clearing Last.fm now playing...");
                request_remove_now_playing(
                    credentials_,
                    session_.session_key,
                    abort
                );
            } else {
                status.set_item("Submitting Last.fm scrobble...");
                if (!record_.has_value()) {
                    throw std::logic_error("Scrobble task has no record");
                }
                response_ = request_scrobble(
                    credentials_,
                    session_.session_key,
                    *record_,
                    abort
                );
            }
        } catch (const exception_aborted&) {
            throw;
        } catch (const lastfm_api_error& exception) {
            api_error_code_ = exception.code();
            error_ = exception.what();
        } catch (const std::exception& exception) {
            error_ = exception.what();
        }
    }

    void on_done(ctx_t, bool was_aborted) override {
        dispatcher().task_done(*this, was_aborted);
    }

    [[nodiscard]] submission_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] std::uint64_t account_generation() const noexcept {
        return account_generation_;
    }

    [[nodiscard]] std::uint64_t queue_generation() const noexcept {
        return queue_generation_;
    }

    [[nodiscard]] const std::string& username() const noexcept {
        return session_.username;
    }

    [[nodiscard]] const std::optional<lastfm_submission_response>& response()
        const noexcept {
        return response_;
    }

    [[nodiscard]] const std::optional<int>& api_error_code() const noexcept {
        return api_error_code_;
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

    [[nodiscard]] const std::optional<scrobble_record>& record() const noexcept {
        return record_;
    }

private:
    submission_kind kind_;
    std::uint64_t account_generation_;
    std::uint64_t queue_generation_;
    api_credentials credentials_;
    session_credentials session_;
    std::optional<track_metadata> track_;
    std::optional<scrobble_record> record_;
    std::optional<lastfm_submission_response> response_;
    std::optional<int> api_error_code_;
    std::string error_;
};

void submission_dispatcher::initialize() {
    core_api::ensure_main_thread();
    if (initialized_) {
        return;
    }

    initialized_ = true;
    shutting_down_ = false;
    retry_timer_ = CreateThreadpoolTimer(
        retry_timer_callback,
        this,
        nullptr
    );
    timer_callbacks_enabled_.store(
        retry_timer_ != nullptr,
        std::memory_order_release
    );
    if (retry_timer_ == nullptr) {
        console::print(
            "foo_lastfm_scrobbler: automatic retry timer is unavailable"
        );
    }

    try {
        queue_ = submission_config::load();
        if (!queue_.records.empty()) {
            FB2K_console_formatter() <<
                "foo_lastfm_scrobbler: restored " << queue_.records.size() <<
                " encrypted queued scrobble(s)";
        }

        if (!queue_.records.empty() && queue_.owner_username.empty()) {
            auto session = session_config::load();
            if (session.has_value()) {
                queue_.owner_username = session->username;
                persist_queue();
            }
        }
    } catch (const std::exception& exception) {
        queue_ = {};
        status_ = "The encrypted scrobble queue could not be restored.";
        console::complain(
            "foo_lastfm_scrobbler: could not restore the encrypted scrobble queue",
            exception
        );
    }
    start_next();
}

void submission_dispatcher::shutdown() noexcept {
    if (!core_api::is_main_thread()) {
        return;
    }
    shutting_down_ = true;
    pending_now_playing_.reset();
    pending_remove_now_playing_ = false;
    timer_callbacks_enabled_.store(false, std::memory_order_release);
    cancel_retry_timer(false);
    if (retry_timer_ != nullptr) {
        WaitForThreadpoolTimerCallbacks(retry_timer_, TRUE);
        CloseThreadpoolTimer(retry_timer_);
        retry_timer_ = nullptr;
    }
}

void submission_dispatcher::account_connected(
    const std::string& username
) {
    core_api::ensure_main_thread();
    if (!initialized_ || shutting_down_ || username.empty()) {
        return;
    }

    ++account_generation_;
    pending_now_playing_.reset();
    pending_remove_now_playing_ = false;
    cancel_retry_timer(true);

    if (!queue_.records.empty()) {
        if (queue_.owner_username.empty()) {
            queue_.owner_username = username;
            persist_queue();
        } else if (!queue_matches_account(username)) {
            retry_blocked_ = true;
            status_ =
                "Queued scrobbles belong to another account. Disconnect to clear them.";
            return;
        }
    }

    retry_blocked_ = false;
    status_ = "Connected and ready.";
    start_next();
}

void submission_dispatcher::account_disconnected() {
    core_api::ensure_main_thread();
    ++account_generation_;
    ++queue_generation_;
    pending_now_playing_.reset();
    pending_remove_now_playing_ = false;
    queue_ = {};
    retry_blocked_ = true;
    cancel_retry_timer(true);
    submission_config::clear();
    status_ = "Disconnected; queued scrobbles were cleared.";
}

void submission_dispatcher::now_playing(const track_metadata& track) {
    core_api::ensure_main_thread();
    if (!initialized_ || shutting_down_ ||
        !session_config::has_encrypted_session()) {
        return;
    }

    pending_remove_now_playing_ = false;
    pending_now_playing_ = track;
    if (retry_timer_ == nullptr) {
        retry_blocked_ = false;
    }
    start_next();
}

void submission_dispatcher::remove_now_playing() {
    core_api::ensure_main_thread();
    if (!initialized_ || shutting_down_ ||
        !session_config::has_encrypted_session()) {
        return;
    }

    pending_now_playing_.reset();
    pending_remove_now_playing_ = true;
    if (retry_timer_ == nullptr) {
        retry_blocked_ = false;
    }
    start_next();
}

bool submission_dispatcher::enqueue(const scrobble_record& record) {
    core_api::ensure_main_thread();
    if (!initialized_ || shutting_down_ ||
        !session_config::has_encrypted_session() ||
        queue_.records.size() >= maximum_queued_scrobbles) {
        return false;
    }

    auto session = session_config::load();
    if (!session.has_value()) {
        return false;
    }
    if (queue_.records.empty()) {
        queue_.owner_username = session->username;
    } else if (queue_.owner_username.empty()) {
        queue_.owner_username = session->username;
    } else if (!queue_matches_account(session->username)) {
        status_ =
            "Queued scrobbles belong to another account. Disconnect to clear them.";
        return false;
    }

    queue_.records.push_back(record);
    try {
        submission_config::save(queue_);
    } catch (...) {
        queue_.records.pop_back();
        if (queue_.records.empty()) {
            queue_.owner_username.clear();
        }
        throw;
    }

    if (retry_timer_ == nullptr) {
        retry_blocked_ = false;
    }
    status_ = "Scrobble saved in the encrypted queue.";
    start_next();
    return true;
}

submission_diagnostics submission_dispatcher::diagnostics() const {
    core_api::ensure_main_thread();
    return {
        .queued_scrobbles = queue_.records.size(),
        .sending = active_,
        .retry_scheduled = retry_scheduled_,
        .retry_in_seconds = retry_delay_seconds_,
        .status = status_,
    };
}

bool submission_dispatcher::queue_matches_account(
    const std::string& username
) const noexcept {
    return queue_.owner_username.empty() ||
        queue_.owner_username == username;
}

void submission_dispatcher::start_next() noexcept {
    if (!initialized_ || shutting_down_ || active_) {
        return;
    }

    const bool has_remove_now_playing = pending_remove_now_playing_;
    const bool has_now_playing = pending_now_playing_.has_value();
    const bool has_scrobble =
        !queue_.records.empty() && !retry_blocked_;
    if (!has_remove_now_playing && !has_now_playing && !has_scrobble) {
        return;
    }
    if (!app_api_credentials_available() ||
        !session_config::has_encrypted_session()) {
        pending_now_playing_.reset();
        pending_remove_now_playing_ = false;
        return;
    }

    try {
        auto session = session_config::load();
        if (!session.has_value()) {
            pending_now_playing_.reset();
            pending_remove_now_playing_ = false;
            return;
        }

        const auto kind = has_remove_now_playing
            ? submission_kind::remove_now_playing
            : has_now_playing
                ? submission_kind::now_playing
                : submission_kind::scrobble;
        if (kind == submission_kind::scrobble) {
            if (queue_.owner_username.empty()) {
                queue_.owner_username = session->username;
                persist_queue();
            } else if (!queue_matches_account(session->username)) {
                retry_blocked_ = true;
                status_ =
                    "Queued scrobbles belong to another account. Disconnect to clear them.";
                return;
            }
        }

        std::optional<track_metadata> track;
        std::optional<scrobble_record> record;
        if (kind == submission_kind::remove_now_playing) {
            pending_remove_now_playing_ = false;
            status_ = "Clearing now-playing status...";
        } else if (kind == submission_kind::now_playing) {
            track = std::move(pending_now_playing_);
            pending_now_playing_.reset();
            status_ = "Sending now-playing update...";
        } else {
            record = queue_.records.front();
            status_ = "Submitting the oldest queued scrobble...";
        }

        auto task = fb2k::service_new<submission_task>(
            kind,
            account_generation_,
            queue_generation_,
            app_api_credentials(),
            std::move(*session),
            std::move(track),
            std::move(record)
        );
        active_ = true;
        threaded_process::get()->run_modeless(
            task,
            threaded_process::flag_silent,
            core_api::get_main_window(),
            "Last.fm scrobbling"
        );
    } catch (const std::exception& exception) {
        active_ = false;
        if (has_remove_now_playing) {
            console::complain(
                "foo_lastfm_scrobbler: could not start now-playing removal",
                exception
            );
            status_ = "Now-playing status could not be cleared.";
        } else if (has_now_playing) {
            console::complain(
                "foo_lastfm_scrobbler: could not start now-playing update",
                exception
            );
            if (!queue_.records.empty()) {
                schedule_retry();
            } else {
                status_ = "Now-playing update could not be started.";
            }
        } else {
            console::complain(
                "foo_lastfm_scrobbler: could not start scrobble submission",
                exception
            );
            schedule_retry();
        }
    }
}

void submission_dispatcher::persist_queue() noexcept {
    if (queue_.records.empty()) {
        queue_.owner_username.clear();
    }
    try {
        submission_config::save(queue_);
    } catch (const std::exception& exception) {
        console::complain(
            "foo_lastfm_scrobbler: could not update the encrypted scrobble queue",
            exception
        );
    }
}

void submission_dispatcher::invalidate_session() noexcept {
    cancel_retry_timer(false);
    retry_blocked_ = true;
    status_ = "Reconnect the Last.fm account; queued scrobbles are preserved.";
    try {
        session_config::clear();
        console::print(
            "foo_lastfm_scrobbler: Last.fm rejected the saved session; reconnect the account"
        );
    } catch (const std::exception& exception) {
        console::complain(
            "foo_lastfm_scrobbler: could not clear the rejected Last.fm session",
            exception
        );
    }
}

void submission_dispatcher::schedule_retry() noexcept {
    retry_blocked_ = true;
    if (shutting_down_ || queue_.records.empty() || retry_scheduled_) {
        return;
    }
    if (retry_timer_ == nullptr) {
        status_ =
            "Temporary failure; retry will occur on the next playback event.";
        return;
    }

    ++retry_attempts_;
    retry_delay_seconds_ = lastfm_scrobbler::retry_delay_seconds(
        retry_attempts_
    );
    retry_scheduled_ = true;
    retry_generation_.fetch_add(1, std::memory_order_acq_rel);

    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(
        -static_cast<LONGLONG>(retry_delay_seconds_) * 10'000'000LL
    );
    FILETIME due_time{due.LowPart, due.HighPart};
    SetThreadpoolTimer(retry_timer_, &due_time, 0, 0);

    status_ = "Temporary failure; automatic retry in " +
        std::to_string(retry_delay_seconds_) + " seconds.";
}

void submission_dispatcher::cancel_retry_timer(
    bool reset_attempts
) noexcept {
    retry_generation_.fetch_add(1, std::memory_order_acq_rel);
    retry_scheduled_ = false;
    retry_delay_seconds_ = 0;
    if (retry_timer_ != nullptr) {
        SetThreadpoolTimer(retry_timer_, nullptr, 0, 0);
        WaitForThreadpoolTimerCallbacks(retry_timer_, TRUE);
    }
    if (reset_attempts) {
        retry_attempts_ = 0;
    }
}

void submission_dispatcher::reset_retry_after_final_result() noexcept {
    retry_blocked_ = false;
    cancel_retry_timer(true);
}

void submission_dispatcher::retry_timer_elapsed_worker() noexcept {
    if (!timer_callbacks_enabled_.load(std::memory_order_acquire)) {
        return;
    }
    const auto generation = retry_generation_.load(
        std::memory_order_acquire
    );
    try {
        main_thread_callback_add(
            fb2k::service_new<retry_main_thread_callback>(generation)
        );
    } catch (...) {
    }
}

void submission_dispatcher::retry_timer_elapsed_main(
    std::uint64_t generation
) {
    core_api::ensure_main_thread();
    if (shutting_down_ || !retry_scheduled_ ||
        generation != retry_generation_.load(std::memory_order_acquire)) {
        return;
    }

    retry_scheduled_ = false;
    retry_delay_seconds_ = 0;
    retry_blocked_ = false;
    status_ = "Retrying queued scrobble...";
    start_next();
}

void submission_dispatcher::task_done(
    submission_task& task,
    bool was_aborted
) {
    core_api::ensure_main_thread();
    active_ = false;

    // A request can complete after disconnecting or replacing its session.
    // Stale errors must not change the replacement session. A known success
    // can still acknowledge its original queue entry across reauthentication,
    // but never a new queue created by disconnecting/reconnecting.
    if (task.account_generation() != account_generation_) {
        if (task.kind() == submission_kind::scrobble && !was_aborted &&
            task.response().has_value() && task.response()->accepted &&
            task.queue_generation() == queue_generation_ &&
            queue_.owner_username == task.username() &&
            !queue_.records.empty() && task.record().has_value() &&
            queue_.records.front() == *task.record()) {
            queue_.records.erase(queue_.records.begin());
            persist_queue();
            reset_retry_after_final_result();
        }
        start_next();
        return;
    }

    const auto error_code = task.api_error_code();
    const bool invalid_session = error_code == 9;
    const bool retryable_api_error =
        error_code == 11 || error_code == 16 || error_code == 29;
    if (invalid_session) {
        invalidate_session();
    }

    if (task.kind() == submission_kind::now_playing) {
        if (was_aborted) {
            console::print(
                "foo_lastfm_scrobbler: Last.fm now-playing update was cancelled"
            );
            if (!shutting_down_ && !queue_.records.empty()) {
                schedule_retry();
            } else if (!shutting_down_) {
                status_ = "Now-playing update was cancelled.";
            }
        } else if (!task.error().empty()) {
            console::complain(
                "foo_lastfm_scrobbler: Last.fm now-playing update failed",
                task.error().c_str()
            );
            if (!invalid_session &&
                (retryable_api_error || !error_code.has_value()) &&
                !queue_.records.empty()) {
                schedule_retry();
            } else if (!invalid_session &&
                (retryable_api_error || !error_code.has_value())) {
                status_ = "Now-playing update failed temporarily.";
            } else if (!invalid_session) {
                status_ = "Now-playing update failed.";
            }
        } else if (task.response().has_value() &&
            task.response()->accepted) {
            status_ = "Now playing updated.";
            console::print(
                "foo_lastfm_scrobbler: Last.fm now playing updated"
            );
        } else if (task.response().has_value()) {
            status_ = "Last.fm ignored the now-playing update.";
            FB2K_console_formatter() <<
                "foo_lastfm_scrobbler: Last.fm ignored now playing (" <<
                task.response()->ignored_code << ": " <<
                task.response()->ignored_message.c_str() << ')';
        }
    } else if (task.kind() == submission_kind::remove_now_playing) {
        if (was_aborted) {
            console::print(
                "foo_lastfm_scrobbler: Last.fm now-playing removal was cancelled"
            );
            if (!shutting_down_) {
                status_ = "Now-playing removal was cancelled.";
            }
        } else if (!task.error().empty()) {
            console::complain(
                "foo_lastfm_scrobbler: Last.fm now-playing removal failed",
                task.error().c_str()
            );
            if (!invalid_session) {
                status_ = "Now-playing status could not be cleared.";
            }
        } else {
            status_ = "Now-playing status cleared.";
            console::print(
                "foo_lastfm_scrobbler: Last.fm now playing cleared"
            );
        }
    } else {
        bool remove_record = false;
        if (was_aborted) {
            console::print(
                "foo_lastfm_scrobbler: scrobble submission was cancelled and remains queued"
            );
            if (!shutting_down_) {
                schedule_retry();
            }
        } else if (task.response().has_value()) {
            remove_record = true;
            if (task.response()->accepted) {
                status_ = "Last scrobble accepted by Last.fm.";
                console::print(
                    "foo_lastfm_scrobbler: scrobble accepted by Last.fm"
                );
            } else {
                status_ = "Last.fm ignored the last scrobble.";
                FB2K_console_formatter() <<
                    "foo_lastfm_scrobbler: Last.fm ignored scrobble (" <<
                    task.response()->ignored_code << ": " <<
                    task.response()->ignored_message.c_str() << ')';
            }
        } else if (invalid_session) {
            // invalidate_session() already preserved the queue and status.
        } else if (retryable_api_error || !error_code.has_value()) {
            console::complain(
                "foo_lastfm_scrobbler: scrobble remains queued after a temporary failure",
                task.error().c_str()
            );
            schedule_retry();
        } else {
            remove_record = true;
            status_ = "Last.fm permanently rejected the last scrobble.";
            console::complain(
                "foo_lastfm_scrobbler: Last.fm permanently rejected a scrobble",
                task.error().c_str()
            );
        }

        if (remove_record && !queue_.records.empty() &&
            task.record().has_value() &&
            queue_.records.front() == *task.record()) {
            queue_.records.erase(queue_.records.begin());
            persist_queue();
            reset_retry_after_final_result();
        }
    }

    // start_next() applies backoff to scrobbles while allowing a newer
    // playback status (including a pending pause/stop) to proceed.
    start_next();
}

} // namespace

void submission_initialize() {
    dispatcher().initialize();
}

void submission_shutdown() noexcept {
    dispatcher().shutdown();
}

void submission_account_connected(const std::string& username) noexcept {
    try {
        dispatcher().account_connected(username);
    } catch (const std::exception& exception) {
        console::complain(
            "foo_lastfm_scrobbler: could not resume queued scrobbles after connecting",
            exception
        );
    }
}

void submission_account_disconnected() noexcept {
    try {
        dispatcher().account_disconnected();
    } catch (const std::exception& exception) {
        console::complain(
            "foo_lastfm_scrobbler: could not clear queued scrobbles while disconnecting",
            exception
        );
    }
}

submission_diagnostics submission_diagnostics_snapshot() noexcept {
    try {
        return dispatcher().diagnostics();
    } catch (const std::exception& exception) {
        return {
            .status = std::string("Submission status is unavailable: ") +
                exception.what(),
        };
    }
}

void submit_now_playing(const track_metadata& track) noexcept {
    try {
        dispatcher().now_playing(track);
    } catch (const std::exception& exception) {
        console::complain(
            "foo_lastfm_scrobbler: could not queue now-playing update",
            exception
        );
    }
}

void remove_now_playing() noexcept {
    try {
        dispatcher().remove_now_playing();
    } catch (const std::exception& exception) {
        console::complain(
            "foo_lastfm_scrobbler: could not queue now-playing removal",
            exception
        );
    }
}

bool enqueue_scrobble(const scrobble_record& record) noexcept {
    try {
        return dispatcher().enqueue(record);
    } catch (const std::exception& exception) {
        console::complain(
            "foo_lastfm_scrobbler: could not persist the scrobble queue",
            exception
        );
        return false;
    }
}

} // namespace lastfm_scrobbler
