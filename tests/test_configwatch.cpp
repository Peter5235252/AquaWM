#include "configwatch.hpp"

#include <cstdio>
#include <cstdlib>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

int main() {
    using aquawm::configwatch_should_reload;

    // Completion events for our file reload.
    CHECK(configwatch_should_reload(IN_CLOSE_WRITE, "aquawm.lua", "aquawm.lua"));
    CHECK(configwatch_should_reload(IN_MOVED_TO, "aquawm.lua", "aquawm.lua"));
    CHECK(configwatch_should_reload(IN_CREATE, "aquawm.lua", "aquawm.lua"));
    CHECK(configwatch_should_reload(IN_CLOSE_WRITE | IN_ISDIR, "aquawm.lua",
        "aquawm.lua") == true); // flag bits outside the save set ignored

    // Noise that must not reload.
    CHECK(!configwatch_should_reload(IN_MODIFY, "aquawm.lua", "aquawm.lua"));
    CHECK(!configwatch_should_reload(IN_ATTRIB, "aquawm.lua", "aquawm.lua"));
    CHECK(!configwatch_should_reload(IN_DELETE, "aquawm.lua", "aquawm.lua"));
    CHECK(!configwatch_should_reload(IN_OPEN, "aquawm.lua", "aquawm.lua"));
    CHECK(!configwatch_should_reload(0, "aquawm.lua", "aquawm.lua"));

    // Other files (swap files, wallpapers, other configs) never reload.
    CHECK(!configwatch_should_reload(IN_CLOSE_WRITE, ".aquawm.lua.swp",
        "aquawm.lua"));
    CHECK(!configwatch_should_reload(IN_CLOSE_WRITE, "wallpaper.jpg",
        "aquawm.lua"));
    CHECK(!configwatch_should_reload(IN_CLOSE_WRITE, "other.lua", "aquawm.lua"));
    CHECK(!configwatch_should_reload(IN_MOVED_TO, "aquawm.lua~", "aquawm.lua"));

    // Debounce delay is a sane small value.
    CHECK(aquawm::CONFIG_RELOAD_DEBOUNCE_MS >= 50);
    CHECK(aquawm::CONFIG_RELOAD_DEBOUNCE_MS <= 1000);

    if (failures == 0) {
        std::puts("test_configwatch: all checks passed");
        return 0;
    }
    std::printf("test_configwatch: %d check(s) failed\n", failures);
    return 1;
}
