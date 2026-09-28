// aquawm Lua config bridge: loads an init.lua file into a Config.
//
// Config language (see examples/init.lua):
//
//   config = { gaps = 8, mfact = 0.6, nmaster = 1, workspaces = 4,
//                layout = "master", terminal = "kitty" }
//
//   bind("Super", "Return", "spawn-terminal")
//   bind("Super", "j", "focus-next")
//   bind("Super", "k", "focus-prev")
//   bind("Super", "t", "toggle-floating")
//   bind("Super", "q", "close")
//   bind("Super", "m", "quit")
//   bind("Super", "l", "cycle-layout")
//   bind("Super+Shift", "r", "reload-config")
//   for i = 1, 4 do
//       bind("Super", tostring(i), "workspace", i)
//       bind("Super+Shift", tostring(i), "move-to-workspace", i)
//   end
//
// Actions: spawn-terminal, close, quit, focus-next, focus-prev,
// toggle-floating, cycle-layout, workspace (arg: 1-based number),
// move-to-workspace (arg: 1-based number), reload-config.

#include "config.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <lua.hpp>
#include <xkbcommon/xkbcommon.h>

namespace aquawm {
namespace {

bool iequals(const std::string &a, const char *b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            static_cast<unsigned char>(b[i])) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

std::string trim(const std::string &s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

int get_int_field(lua_State *L, const char *name, int dflt) {
    lua_getfield(L, -1, name);
    int v = lua_isnoneornil(L, -1) ? dflt : static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return v;
}

float get_float_field(lua_State *L, const char *name, float dflt) {
    lua_getfield(L, -1, name);
    float v =
        lua_isnoneornil(L, -1) ? dflt : static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return v;
}

std::string get_string_field(lua_State *L, const char *name, const std::string &dflt) {
    lua_getfield(L, -1, name);
    std::string v = dflt;
    if (lua_type(L, -1) == LUA_TSTRING) {
        v = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    return v;
}

bool get_bool_field(lua_State *L, const char *name, bool dflt) {
    lua_getfield(L, -1, name);
    bool v = dflt;
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        v = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    return v;
}

// bind(mods, key, action [, arg]) exposed to Lua. The destination vector is
// passed as a lightuserdata upvalue.
int l_bind(lua_State *L) {
    auto *out = static_cast<std::vector<Keybind> *>(
        lua_touserdata(L, lua_upvalueindex(1)));
    const char *mods = luaL_checkstring(L, 1);
    const char *key = luaL_checkstring(L, 2);
    const char *action = luaL_checkstring(L, 3);
    int arg = static_cast<int>(luaL_optinteger(L, 4, 0));

    bool ok = false;
    uint32_t m = parse_mods(mods, ok);
    if (!ok) {
        return luaL_error(L, "bind: bad modifier spec '%s'", mods);
    }
    uint32_t sym =
        xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym == XKB_KEY_NoSymbol) {
        return luaL_error(L, "bind: unknown key name '%s'", key);
    }
    if (!known_action(action)) {
        return luaL_error(L, "bind: unknown action '%s'", action);
    }
    out->push_back(Keybind{m, sym, action, arg});
    return 0;
}

// rule({ match = {...}, float = ..., workspace = N }) exposed to Lua.
// Destination vector passed as lightuserdata upvalue, like l_bind.
int l_rule(lua_State *L) {
    auto *out = static_cast<std::vector<Rule> *>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "rule: expected a table");
    }
    Rule rule;
    lua_getfield(L, 1, "match");
    if (!lua_isnoneornil(L, -1)) {
        if (!lua_istable(L, -1)) {
            return luaL_error(L, "rule: match must be a table");
        }
        lua_getfield(L, -1, "app_id");
        if (lua_type(L, -1) == LUA_TSTRING) {
            rule.match.app_id = lua_tostring(L, -1);
        } else if (!lua_isnoneornil(L, -1)) {
            return luaL_error(L, "rule: app_id must be a string");
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "title");
        if (lua_type(L, -1) == LUA_TSTRING) {
            rule.match.title = lua_tostring(L, -1);
        } else if (!lua_isnoneornil(L, -1)) {
            return luaL_error(L, "rule: title must be a string");
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "xwayland");
        if (lua_type(L, -1) == LUA_TBOOLEAN) {
            rule.match.xwayland = lua_toboolean(L, -1) != 0;
        } else if (!lua_isnoneornil(L, -1)) {
            return luaL_error(L, "rule: xwayland must be a boolean");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // match (or nil)
    lua_getfield(L, 1, "float");
    if (lua_type(L, -1) == LUA_TBOOLEAN) {
        rule.floating = lua_toboolean(L, -1) != 0;
    } else if (!lua_isnoneornil(L, -1)) {
        return luaL_error(L, "rule: float must be a boolean");
    }
    lua_pop(L, 1);
    lua_getfield(L, 1, "workspace");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        int ws = static_cast<int>(lua_tointeger(L, -1));
        if (ws < 1 || ws > 9) {
            return luaL_error(L, "rule: workspace out of range 1..9");
        }
        rule.workspace = ws;
    } else if (!lua_isnoneornil(L, -1)) {
        return luaL_error(L, "rule: workspace must be a number");
    }
    lua_pop(L, 1);
    if (!rule.floating.has_value() && rule.workspace == 0) {
        return luaL_error(L, "rule: nothing to apply (need float and/or workspace)");
    }
    out->push_back(std::move(rule));
    return 0;
}

} // namespace

uint32_t parse_mods(const std::string &spec, bool &ok) {
    uint32_t mods = 0;
    ok = true;
    std::size_t pos = 0;
    while (pos <= spec.size()) {
        std::size_t end = pos;
        while (end < spec.size() && spec[end] != '+' && spec[end] != '-' &&
               spec[end] != '|') {
            ++end;
        }
        std::string tok = trim(spec.substr(pos, end - pos));
        if (!tok.empty()) {
            if (iequals(tok, "shift")) {
                mods |= MOD_SHIFT;
            } else if (iequals(tok, "ctrl") || iequals(tok, "control")) {
                mods |= MOD_CTRL;
            } else if (iequals(tok, "alt")) {
                mods |= MOD_ALT;
            } else if (iequals(tok, "super") || iequals(tok, "logo") ||
                       iequals(tok, "win") || iequals(tok, "mod4")) {
                mods |= MOD_SUPER;
            } else {
                ok = false;
                return 0;
            }
        }
        pos = end + 1;
    }
    return mods;
}

bool known_action(const std::string &action) {
    return action == "spawn-terminal" || action == "close" ||
           action == "quit" || action == "focus-next" ||
           action == "focus-prev" || action == "toggle-floating" ||
           action == "workspace" || action == "move-to-workspace" ||
           action == "reload-config" || action == "cycle-layout";
}

Config default_config() {
    Config c;
    c.wallpaper = default_wallpaper_path();
    bool ok = false;
    auto add = [&](const char *mods, const char *key, const char *action,
                   int arg = 0) {
        c.keys.push_back(Keybind{parse_mods(mods, ok),
                                 xkb_keysym_from_name(
                                     key, XKB_KEYSYM_CASE_INSENSITIVE),
                                 action, arg});
    };
    add("Super", "Return", "spawn-terminal");
    add("Super", "j", "focus-next");
    add("Super", "k", "focus-prev");
    add("Super", "t", "toggle-floating");
    add("Super", "q", "close");
    add("Super", "m", "quit"); // back to the login manager (the only quit bind)
    add("Super+Shift", "r", "reload-config");
    for (int i = 1; i <= 4; ++i) {
        char key[2] = {static_cast<char>('0' + i), '\0'};
        add("Super", key, "workspace", i);
        add("Super+Shift", key, "move-to-workspace", i);
    }
    (void)ok;
    return c;
}

std::string config_base_dir() {
    // XDG Base Directory: $XDG_CONFIG_HOME when set to an absolute path,
    // otherwise $HOME/.config. Identical lookup on Arch, Fedora and
    // NixOS; only the environment differs per machine.
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') {
        return xdg;
    }
    const char *home = std::getenv("HOME");
    std::string base = (home != nullptr && home[0] != '\0') ? home : "/tmp";
    return base + "/.config";
}

std::string default_config_path() {
    return config_base_dir() + "/aquawm/aquawm.lua";
}

std::string default_wallpaper_path() {
    return config_base_dir() + "/aquawm/wallpaper.jpg";
}

std::string legacy_config_path() {
    return config_base_dir() + "/tilewm/init.lua";
}

std::string legacy_wallpaper_path() {
    return config_base_dir() + "/tilewm/wallpaper.jpg";
}

std::string resolve_config_path(const std::string &explicit_path) {
    namespace fs = std::filesystem;
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    std::string preferred = default_config_path();
    if (fs::exists(preferred)) {
        return preferred;
    }
    std::string legacy = legacy_config_path();
    if (fs::exists(legacy)) {
        return legacy;
    }
    return preferred;
}

std::string resolve_wallpaper_path(const std::string &configured) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // An explicit non-default choice is honored even when missing, so typos
    // fail loudly downstream instead of silently showing another image.
    // The built-in default just means "no choice made", so it participates
    // in the legacy fallback below.
    if (!configured.empty() && configured != default_wallpaper_path()) {
        return configured;
    }
    std::string preferred = default_wallpaper_path();
    if (fs::exists(preferred, ec)) {
        return preferred;
    }
    std::string legacy = legacy_wallpaper_path();
    if (fs::exists(legacy, ec)) {
        return legacy;
    }
    return preferred;
}

bool load_config_file(const char *path, Config &out, std::string &error) {
    lua_State *L = luaL_newstate();
    if (L == nullptr) {
        error = "could not create Lua state";
        return false;
    }
    luaL_openlibs(L);

    std::vector<Keybind> file_keys;
    lua_pushlightuserdata(L, &file_keys);
    lua_pushcclosure(L, l_bind, 1);
    lua_setglobal(L, "bind");

    std::vector<Rule> file_rules;
    lua_pushlightuserdata(L, &file_rules);
    lua_pushcclosure(L, l_rule, 1);
    lua_setglobal(L, "rule");

    if (luaL_dofile(L, path) != LUA_OK) {
        error = lua_tostring(L, -1);
        lua_close(L);
        return false;
    }

    Config next = out;
    lua_getglobal(L, "config");
    if (lua_istable(L, -1)) {
        int gaps = get_int_field(L, "gaps", next.gaps);
        float mfact = get_float_field(L, "mfact", next.mfact);
        int nmaster = get_int_field(L, "nmaster", next.nmaster);
        int workspaces = get_int_field(L, "workspaces", next.workspaces);
        std::string wallpaper =
            get_string_field(L, "wallpaper", next.wallpaper);
        bool autogenerated_warn =
            get_bool_field(L, "autogenerated_warn", next.autogenerated_warn);
        std::string layout = get_string_field(L, "layout", next.layout);
        float split_ratio =
            get_float_field(L, "split_ratio", next.split_ratio);
        std::string terminal =
            get_string_field(L, "terminal", next.terminal);
        if (gaps < 0) {
            gaps = 0;
        }
        if (gaps > 256) {
            gaps = 256;
        }
        if (mfact < 0.05f) {
            mfact = 0.05f;
        }
        if (mfact > 0.95f) {
            mfact = 0.95f;
        }
        if (nmaster < 1) {
            nmaster = 1;
        }
        if (workspaces < 1) {
            workspaces = 1;
        }
        if (workspaces > 9) {
            workspaces = 9;
        }
        next.gaps = gaps;
        next.mfact = mfact;
        next.nmaster = nmaster;
        next.workspaces = workspaces;
        next.autogenerated_warn = autogenerated_warn;
        // Unknown layout names fall back to master (documented); the rest
        // of the file still applies.
        next.layout = known_layout(layout) ? layout : "master";
        if (split_ratio < 0.1f) {
            split_ratio = 0.1f;
        }
        if (split_ratio > 0.9f) {
            split_ratio = 0.9f;
        }
        next.split_ratio = split_ratio;
        if (!terminal.empty()) {
            next.terminal = terminal;
        }
        if (!wallpaper.empty()) {
            next.wallpaper = wallpaper;
        }
        int vrr = get_int_field(L, "vrr", next.vrr);
        if (vrr < 0) {
            vrr = 0;
        }
        if (vrr > 2) {
            vrr = 2;
        }
        next.vrr = vrr;
        // keyboard = { layout = "us,hu", variant = "", options = ... }:
        // absent fields keep their previous values.
        lua_getfield(L, -1, "keyboard");
        if (lua_istable(L, -1)) {
            next.keyboard.layout =
                get_string_field(L, "layout", next.keyboard.layout);
            next.keyboard.variant =
                get_string_field(L, "variant", next.keyboard.variant);
            next.keyboard.options =
                get_string_field(L, "options", next.keyboard.options);
        }
        lua_pop(L, 1); // keyboard (or nil)
        // outputs = { { name = "DP-1", width = 2560, height = 1440,
        //               refresh = 144 }, ... }: a present table replaces
        // the list wholesale (so it can also be cleared).
        lua_getfield(L, -1, "outputs");
        if (lua_istable(L, -1)) {
            std::vector<OutputMode> modes;
            const std::size_t n = lua_rawlen(L, -1);
            for (std::size_t i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, static_cast<lua_Integer>(i));
                if (lua_istable(L, -1)) {
                    OutputMode m;
                    m.name = get_string_field(L, "name", "");
                    m.width = get_int_field(L, "width", 0);
                    m.height = get_int_field(L, "height", 0);
                    m.refresh = get_float_field(L, "refresh", 0);
                    if (m.width < 0) {
                        m.width = 0;
                    }
                    if (m.height < 0) {
                        m.height = 0;
                    }
                    if (m.refresh < 0) {
                        m.refresh = 0;
                    }
                    modes.push_back(std::move(m));
                }
                lua_pop(L, 1); // entry (or non-table)
            }
            next.outputs = std::move(modes);
        }
        lua_pop(L, 1); // outputs (or nil)
    }
    lua_pop(L, 1); // config (or the non-table global)

    // A file that binds nothing keeps the previous keymap (defaults on
    // first load); a file with binds replaces it wholesale. Same for rules.
    if (!file_keys.empty()) {
        next.keys = std::move(file_keys);
    }
    if (!file_rules.empty()) {
        next.rules = std::move(file_rules);
    }

    lua_close(L);
    out = std::move(next);
    return true;
}

} // namespace aquawm
