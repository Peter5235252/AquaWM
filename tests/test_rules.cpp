#include "rules.hpp"

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

using aquawm::Rule;

static Rule make(const std::string &app_id, const std::string &title,
    std::optional<bool> xwayland, std::optional<bool> floating, int ws) {
    Rule r;
    r.match.app_id = app_id;
    r.match.title = title;
    r.match.xwayland = xwayland;
    r.floating = floating;
    r.workspace = ws;
    return r;
}

int main() {
    // Empty rule matches everything (harmless; effects decide).
    CHECK(aquawm::rule_matches(make("", "", std::nullopt, true, 0), "foot",
        "term", false));
    CHECK(aquawm::rule_matches(make("", "", std::nullopt, true, 0), "xterm",
        "", true));

    // app_id substring, case-sensitive.
    CHECK(aquawm::rule_matches(make("fire", "", std::nullopt, true, 0),
        "firefox", "", false));
    CHECK(!aquawm::rule_matches(make("Fire", "", std::nullopt, true, 0),
        "firefox", "", false));
    CHECK(!aquawm::rule_matches(make("chrom", "", std::nullopt, true, 0),
        "firefox", "", false));

    // Title substring; empty client title matches only wildcard rules.
    CHECK(aquawm::rule_matches(
        make("", "Picture-in-Picture", std::nullopt, true, 0), "firefox",
        "Some Video - Picture-in-Picture", false));
    CHECK(!aquawm::rule_matches(
        make("", "Picture-in-Picture", std::nullopt, true, 0), "firefox", "",
        false));
    CHECK(aquawm::rule_matches(make("", "", std::nullopt, true, 0), "firefox",
        "", false));

    // Protocol gate.
    CHECK(aquawm::rule_matches(make("", "", true, true, 0), "xterm", "", true));
    CHECK(!aquawm::rule_matches(make("", "", true, true, 0), "foot", "", false));
    CHECK(!aquawm::rule_matches(make("", "", false, true, 0), "xterm", "",
        true));
    CHECK(aquawm::rule_matches(make("", "", false, true, 0), "foot", "",
        false));

    // Combined fields AND together.
    CHECK(aquawm::rule_matches(
        make("firefox", "Picture", false, true, 0), "firefox",
        "Vid - Picture-in-Picture", false));
    CHECK(!aquawm::rule_matches(
        make("firefox", "Picture", true, true, 0), "firefox",
        "Vid - Picture-in-Picture", false));

    // Defaults: wildcard match, no effects, workspace unset.
    {
        Rule r;
        CHECK(r.match.app_id.empty() && r.match.title.empty());
        CHECK(!r.match.xwayland.has_value());
        CHECK(!r.floating.has_value());
        CHECK(r.workspace == 0);
    }

    if (failures == 0) {
        std::puts("test_rules: all checks passed");
        return 0;
    }
    std::printf("test_rules: %d check(s) failed\n", failures);
    return 1;
}
