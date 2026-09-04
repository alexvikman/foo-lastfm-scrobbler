#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/SDK/coreDarkMode.h>

#include "auth/app_credentials.h"
#include "auth/auth_flow.h"
#include "auth/session_config.h"
#include "network/lastfm_http.h"
#include "network/lastfm_protocol.h"
#include "submission/submission_service.h"
#include "resource.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace lastfm_scrobbler {
namespace {

constexpr UINT message_token_request_done = WM_APP + 101;
constexpr UINT message_session_request_done = WM_APP + 102;
constexpr UINT_PTR diagnostics_timer_id = 1;

void wipe_string(std::string& value) noexcept {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

class token_request_task : public threaded_process_callback {
public:
    token_request_task(
        preferences_page_instance::ptr owner,
        HWND target,
        api_credentials credentials
    ) :
        owner_(std::move(owner)),
        target_(target),
        credentials_(std::move(credentials)) {}

    ~token_request_task() {
        wipe_string(token_);
    }

    void run(threaded_process_status& status, abort_callback& abort) override {
        status.set_item("Requesting a Last.fm authorization token...");
        try {
            token_ = request_auth_token(credentials_, abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (const std::exception& exception) {
            error_ = exception.what();
        }
    }

    void on_done(ctx_t, bool was_aborted) override {
        if (owner_->get_wnd() == target_ && ::IsWindow(target_)) {
            SendMessage(
                target_,
                message_token_request_done,
                was_aborted ? TRUE : FALSE,
                reinterpret_cast<LPARAM>(this)
            );
        }
    }

    [[nodiscard]] std::string take_token() {
        return std::move(token_);
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

private:
    preferences_page_instance::ptr owner_;
    HWND target_;
    api_credentials credentials_;
    std::string token_;
    std::string error_;
};

class session_request_task : public threaded_process_callback {
public:
    session_request_task(
        preferences_page_instance::ptr owner,
        HWND target,
        api_credentials credentials,
        std::string token
    ) :
        owner_(std::move(owner)),
        target_(target),
        credentials_(std::move(credentials)),
        token_(std::move(token)) {}

    ~session_request_task() {
        wipe_string(token_);
    }

    void run(threaded_process_status& status, abort_callback& abort) override {
        status.set_item("Creating the persistent Last.fm session...");
        try {
            session_ = request_web_service_session(credentials_, token_, abort);
        } catch (const exception_aborted&) {
            throw;
        } catch (const lastfm_api_error& exception) {
            error_code_ = exception.code();
            error_ = exception.what();
        } catch (const std::invalid_argument& exception) {
            // A valid success response consumes the one-time token even if a
            // An unsupported success response still consumes the one-time token.
            request_token_consumed_ = true;
            error_ = exception.what();
        } catch (const std::exception& exception) {
            error_ = exception.what();
        }
    }

    void on_done(ctx_t, bool was_aborted) override {
        if (!was_aborted && error_.empty() && session_.has_value()) {
            try {
                session_config::save(*session_);
                // Notify the dispatcher even if the preferences page has closed.
                // In-flight requests must no longer affect this new session.
                submission_account_connected(session_->username);
                saved_ = true;
                console::print(
                    "foo_lastfm_scrobbler: Last.fm account connected"
                );
            } catch (const std::exception& exception) {
                error_ = exception.what();
            }
        }

        if (owner_->get_wnd() == target_ && ::IsWindow(target_)) {
            SendMessage(
                target_,
                message_session_request_done,
                was_aborted ? TRUE : FALSE,
                reinterpret_cast<LPARAM>(this)
            );
        }
    }

    [[nodiscard]] bool saved() const noexcept {
        return saved_;
    }

    [[nodiscard]] const std::string& username() const noexcept {
        return session_.has_value() ? session_->username : empty_;
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

    [[nodiscard]] std::optional<int> error_code() const noexcept {
        return error_code_;
    }

    [[nodiscard]] bool request_token_consumed() const noexcept {
        return request_token_consumed_;
    }

private:
    preferences_page_instance::ptr owner_;
    HWND target_;
    api_credentials credentials_;
    std::string token_;
    std::optional<session_credentials> session_;
    std::optional<int> error_code_;
    std::string error_;
    std::string empty_;
    bool saved_ = false;
    bool request_token_consumed_ = false;
};

class auth_preferences : public preferences_page_instance {
public:
    explicit auth_preferences(preferences_page_callback::ptr) {}

    ~auth_preferences() {
        if (::IsWindow(window_)) {
            DestroyWindow(window_);
        }
    }

    void create(HWND parent) {
        const auto created = CreateDialogParam(
            core_api::get_my_instance(),
            MAKEINTRESOURCE(IDD_LASTFM_PREFERENCES),
            parent,
            dialog_proc,
            reinterpret_cast<LPARAM>(this)
        );
        if (created == nullptr) {
            throw std::runtime_error("Could not create Last.fm preferences page");
        }
    }

    [[nodiscard]] t_uint32 get_state() override {
        return preferences_state::dark_mode_supported;
    }

    [[nodiscard]] HWND get_wnd() override {
        return window_;
    }

    void apply() override {}
    void reset() override {}

private:
    static INT_PTR CALLBACK dialog_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam
    ) {
        auth_preferences* self = nullptr;
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<auth_preferences*>(lparam);
            self->window_ = window;
            SetWindowLongPtr(
                window,
                DWLP_USER,
                reinterpret_cast<LONG_PTR>(self)
            );
        } else {
            self = reinterpret_cast<auth_preferences*>(
                GetWindowLongPtr(window, DWLP_USER)
            );
        }

        if (self == nullptr) {
            return FALSE;
        }
        const auto result = self->on_message(message, wparam, lparam);
        if (message == WM_NCDESTROY) {
            KillTimer(window, diagnostics_timer_id);
            SetWindowLongPtr(window, DWLP_USER, 0);
            self->window_ = nullptr;
        }
        return result;
    }

    INT_PTR on_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_INITDIALOG:
            dark_mode_.AddDialogWithControls(window_);
            SetTimer(window_, diagnostics_timer_id, 1000, nullptr);
            credentials_ = app_api_credentials();
            {
                bool stored_session_available = false;
                if (session_config::has_encrypted_session()) {
                    try {
                        auto stored_session = session_config::load();
                        if (stored_session.has_value()) {
                            connected_username_ = stored_session->username;
                            stored_session_available = true;
                        }
                    } catch (const std::exception& exception) {
                        status_override_ =
                            "The protected Last.fm session could not be read: " +
                            std::string(exception.what());
                    }
                }
                flow_.configure(
                    app_api_credentials_available(),
                    stored_session_available
                );
            }
            refresh_controls();
            return TRUE;

        case WM_TIMER:
            if (wparam == diagnostics_timer_id) {
                refresh_submission_status();
                return TRUE;
            }
            return FALSE;

        case WM_COMMAND:
            if (HIWORD(wparam) != BN_CLICKED) {
                return FALSE;
            }
            switch (LOWORD(wparam)) {
            case IDC_LASTFM_CONNECT: on_connect(); return TRUE;
            case IDC_LASTFM_FINISH: on_finish(); return TRUE;
            case IDC_LASTFM_DISCONNECT: on_disconnect(); return TRUE;
            default: return FALSE;
            }

        case message_token_request_done:
            token_request_done(
                *reinterpret_cast<token_request_task*>(lparam),
                wparam != FALSE
            );
            return TRUE;

        case message_session_request_done:
            session_request_done(
                *reinterpret_cast<session_request_task*>(lparam),
                wparam != FALSE
            );
            return TRUE;

        default:
            return FALSE;
        }
    }

    void on_connect() {
        if (busy_) {
            return;
        }
        if (flow_.state() == auth_state::awaiting_user_authorization) {
            open_authorization_page();
            return;
        }
        if (!flow_.begin_token_request()) {
            return;
        }

        status_override_.clear();
        busy_ = true;
        refresh_controls();
        try {
            auto task = fb2k::service_new<token_request_task>(
                preferences_page_instance::ptr(this),
                window_,
                credentials_
            );
            threaded_process::get()->run_modeless(
                task,
                threaded_process::flag_show_abort |
                    threaded_process::flag_show_item |
                    threaded_process::flag_show_delayed,
                window_,
                "Connect Last.fm account"
            );
        } catch (const std::exception& exception) {
            busy_ = false;
            flow_.fail_request();
            set_error(exception.what());
        }
    }

    void on_finish() {
        if (busy_ || !flow_.begin_session_request(auth_flow::clock::now())) {
            refresh_controls();
            return;
        }

        status_override_.clear();
        busy_ = true;
        refresh_controls();
        try {
            auto task = fb2k::service_new<session_request_task>(
                preferences_page_instance::ptr(this),
                window_,
                credentials_,
                flow_.request_token()
            );
            threaded_process::get()->run_modeless(
                task,
                threaded_process::flag_show_abort |
                    threaded_process::flag_show_item |
                    threaded_process::flag_show_delayed,
                window_,
                "Connect Last.fm account"
            );
        } catch (const std::exception& exception) {
            busy_ = false;
            flow_.fail_request();
            set_error(exception.what());
        }
    }

    void on_disconnect() {
        if (busy_) {
            return;
        }
        submission_account_disconnected();
        session_config::clear();
        connected_username_.clear();
        status_override_.clear();
        flow_.disconnect();
        console::print("foo_lastfm_scrobbler: Last.fm account disconnected");
        refresh_controls();
    }

    void token_request_done(token_request_task& task, bool was_aborted) {
        busy_ = false;
        if (was_aborted) {
            flow_.fail_request();
            set_error("Last.fm connection was cancelled.");
            return;
        }
        if (!task.error().empty()) {
            flow_.fail_request();
            set_error(task.error());
            return;
        }
        if (!flow_.receive_request_token(
                task.take_token(),
                auth_flow::clock::now()
            )) {
            flow_.fail_request();
            set_error("Last.fm returned an unusable authorization token.");
            return;
        }

        status_override_.clear();
        open_authorization_page();
    }

    void session_request_done(session_request_task& task, bool was_aborted) {
        busy_ = false;
        if (was_aborted) {
            flow_.fail_request();
            set_error("Last.fm connection was cancelled.");
            return;
        }
        if (!task.saved()) {
            if (task.error_code() == 4 || task.error_code() == 15 ||
                task.request_token_consumed()) {
                flow_.disconnect();
            } else {
                flow_.fail_request();
            }
            set_error(task.error().empty()
                ? "The Last.fm session could not be stored."
                : task.error());
            return;
        }
        if (!flow_.complete_session_request()) {
            set_error("Last.fm connected, but the local state could not be updated.");
            return;
        }

        connected_username_ = task.username();
        status_override_.clear();
        refresh_controls();
    }

    void open_authorization_page() {
        if (flow_.state() != auth_state::awaiting_user_authorization ||
            !flow_.request_token_is_valid(auth_flow::clock::now())) {
            flow_.disconnect();
            set_error("The Last.fm authorization token expired. Please start again.");
            return;
        }

        const auto url = make_authorization_url(
            credentials_.api_key,
            flow_.request_token()
        );
        const auto result = uShellExecute(
            window_,
            "open",
            url.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            set_error(
                "The browser could not be opened. Click Connect account to retry."
            );
            return;
        }

        status_override_.clear();
        refresh_controls();
    }

    void refresh_controls() {
        std::string status;
        if (!status_override_.empty()) {
            status = status_override_;
        } else if (busy_) {
            status = "Contacting Last.fm...";
        } else {
            switch (flow_.state()) {
            case auth_state::unconfigured:
                status = "This build has no Last.fm application API credentials.";
                break;
            case auth_state::disconnected:
                status = "Not connected.";
                break;
            case auth_state::requesting_token:
            case auth_state::requesting_session:
                status = "Contacting Last.fm...";
                break;
            case auth_state::awaiting_user_authorization:
                status = "Approve the application in your browser, then click Finish connection.";
                break;
            case auth_state::connected:
                status = connected_username_.empty()
                    ? "Connected. The protected Last.fm session will be reused."
                    : "Connected to Last.fm as " + connected_username_ + ".";
                break;
            case auth_state::failed:
                status = "Connection failed. Try again.";
                break;
            }
        }
        uSetDlgItemText(window_, IDC_LASTFM_STATUS, status.c_str());

        const auto state = flow_.state();
        const bool can_connect = !busy_ &&
            (state == auth_state::disconnected ||
                state == auth_state::failed ||
                state == auth_state::awaiting_user_authorization);
        const bool can_finish = !busy_ &&
            state == auth_state::awaiting_user_authorization;
        const bool can_disconnect = !busy_ &&
            (state == auth_state::connected ||
                session_config::has_encrypted_session());

        EnableWindow(GetDlgItem(window_, IDC_LASTFM_CONNECT), can_connect);
        EnableWindow(GetDlgItem(window_, IDC_LASTFM_FINISH), can_finish);
        EnableWindow(GetDlgItem(window_, IDC_LASTFM_DISCONNECT), can_disconnect);
        uSetDlgItemText(
            window_,
            IDC_LASTFM_CONNECT,
            state == auth_state::awaiting_user_authorization
                ? "Reopen browser"
                : "Connect account"
        );
        refresh_submission_status();
    }

    void refresh_submission_status() {
        const auto diagnostics = submission_diagnostics_snapshot();
        auto status = "Queue: " +
            std::to_string(diagnostics.queued_scrobbles) + ". ";
        if (diagnostics.sending) {
            status += "Sending. ";
        } else if (diagnostics.retry_scheduled) {
            status += "Retry scheduled. ";
        }
        status += diagnostics.status;
        uSetDlgItemText(
            window_,
            IDC_LASTFM_SUBMISSION_STATUS,
            status.c_str()
        );
    }

    void set_error(std::string message) {
        status_override_ = std::move(message);
        refresh_controls();
    }

    HWND window_ = nullptr;
    api_credentials credentials_;
    auth_flow flow_;
    std::string connected_username_;
    std::string status_override_;
    bool busy_ = false;
    fb2k::CCoreDarkModeHooks dark_mode_;
};

class preferences_page_lastfm : public preferences_page_v3 {
public:
    [[nodiscard]] preferences_page_instance::ptr instantiate(
        HWND parent,
        preferences_page_callback::ptr callback
    ) override {
        auto instance = fb2k::service_new<auth_preferences>(callback);
        instance->create(parent);
        return instance;
    }

    [[nodiscard]] const char* get_name() override {
        return "Last.fm Scrobbler";
    }

    [[nodiscard]] GUID get_guid() override {
        return {
            0xc608183e,
            0x72ef,
            0x47bd,
            {0xa0, 0x53, 0xdb, 0x0e, 0x70, 0xc7, 0x3a, 0x71},
        };
    }

    [[nodiscard]] GUID get_parent_guid() override {
        return guid_tools;
    }
};

preferences_page_factory_t<preferences_page_lastfm> preferences_page_factory;

} // namespace
} // namespace lastfm_scrobbler
