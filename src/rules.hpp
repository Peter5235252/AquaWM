#pragma once

// Window rules, slice 1 of the Lua API plan: plain C++ values plus a pure
// matcher, no Lua or wlroots types here, so this header stays unit-testable
// anywhere. config.cpp bridges these to rule() calls in aquawm.lua;
// main.cpp evaluates them when a window maps.
//
// A rule matches when every present match field matches (absent fields are
// wildcards); all matching rules apply in file order, later ones winning.
// Matching is substring-based in v1 (documented limitation).
//
//   rule({ match = { app_id = "firefox",
//                    title = "Picture-in-Picture",
//                    xwayland = true },
//          float = true, workspace = 2 })

#include <optional>
#include <string>

namespace aquawm {

struct RuleMatch {
    std::string app_id;   // empty = wildcard (xdg app_id / X11 class)
    std::string title;    // empty = wildcard
    std::optional<bool> xwayland; // nullopt = either protocol
};

struct Rule {
    RuleMatch match;
    std::optional<bool> floating; // nullopt = leave alone
    int workspace = 0; // 0 = leave alone, else 1-based workspace number
};

inline bool rule_matches(const Rule &rule, const std::string &app_id,
    const std::string &title, bool is_xwayland) {
    const RuleMatch &m = rule.match;
    if (!m.app_id.empty() && app_id.find(m.app_id) == std::string::npos) {
        return false;
    }
    if (!m.title.empty() && title.find(m.title) == std::string::npos) {
        return false;
    }
    if (m.xwayland.has_value() && m.xwayland.value() != is_xwayland) {
        return false;
    }
    return true;
}

} // namespace aquawm
