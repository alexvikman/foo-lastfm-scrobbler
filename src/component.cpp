#include <foobar2000/SDK/foobar2000.h>

#include "auth/session_config.h"
#include "submission/submission_service.h"

DECLARE_COMPONENT_VERSION(
    "Last.fm Scrobbler",
    "0.4.0",
    "Scrobbles music played in foobar2000 directly to Last.fm.\n"
    "Includes now-playing updates, encrypted queueing, automatic retry, and submission diagnostics."
);

VALIDATE_COMPONENT_FILENAME("foo_lastfm_scrobbler.dll");

namespace {

class component_lifecycle : public initquit {
public:
    void on_init() override {
        console::print("foo_lastfm_scrobbler: component initialized");
        if (lastfm_scrobbler::session_config::has_encrypted_session()) {
            console::print(
                "foo_lastfm_scrobbler: protected Last.fm session is available"
            );
        } else {
            console::print(
                "foo_lastfm_scrobbler: Last.fm account is not connected"
            );
        }
        lastfm_scrobbler::submission_initialize();
    }

    void on_quit() override {
        lastfm_scrobbler::submission_shutdown();
    }
};

FB2K_SERVICE_FACTORY(component_lifecycle);

} // namespace
