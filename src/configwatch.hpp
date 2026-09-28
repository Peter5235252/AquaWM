#pragma once

// Config auto-reload decision (inotify side): pure logic, no wlroots or
// loop types here, so this header stays unit-testable on any Linux box.
//
// Editors save differently (in-place rewrite vs atomic rename), so the
// compositor watches the config *directory* and filters by basename.
// A save bursts several events; only completion-ish ones for our file
// should arm the debounce timer.

#include <cstdint>
#include <string>
#include <sys/inotify.h>

namespace aquawm {

// True when an inotify event mask for `name` should trigger a (debounced)
// config reload: our watched file was closed after writing, created, or
// moved into place. Attribute-only touches and deletes do not reload
// (a delete with no recreate keeps the running config, as before).
inline bool configwatch_should_reload(uint32_t mask, const std::string &name,
    const std::string &watched_basename) {
    if (name != watched_basename) {
        return false;
    }
    constexpr uint32_t save_events =
        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;
    return (mask & save_events) != 0;
}

// Debounce delay: coalesce a save burst into one reload.
constexpr int CONFIG_RELOAD_DEBOUNCE_MS = 150;

} // namespace aquawm
