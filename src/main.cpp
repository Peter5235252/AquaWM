// aquawm - Phase 3c: Lua-configured master-stack tiling compositor with
// layer-shell bar support and XWayland.
// Settings (gaps, mfact, nmaster, workspaces) and all keybindings come from
// ~/.config/aquawm/aquawm.lua (see examples/aquawm.lua), reloadable via
// Super+Shift+R or SIGHUP; built-in defaults apply when missing or broken.
// Layer-shell clients (e.g. waybar) render in protocol order above/below
// tiling views and reserve their exclusive zone, which tiling shrinks
// around. Pointer/keyboard input still goes to tiling views only.
// X11 windows run through lazy XWayland and join the same tiling,
// workspace, focus, float and fullscreen flows as xdg-shell windows.
// Pointer: click focuses, Super+Left-drag moves (floating tiled windows
// first), Super+Right-drag resizes, with a default xcursor otherwise.
//
// What works in this phase:
//   * backend autocreate (nested Wayland/X11 window under WSLg, DRM on real hw)
//   * GLES2 renderer + allocator, scene-graph rendering with per-frame commit
//   * single-layout output handling, software cursor with xcursor theme
//   * xdg-shell and XWayland toplevels arranged in a master-stack layout,
//     click-to-focus, Super+Return spawns a terminal, Super+J/K cycles focus,
//     Super+T toggles floating, Super+1..4 switches between 4 workspaces,
//     Super+Shift+1..4 moves the focused window, Super+Q closes, Super+M quits.
//     Override-redirect X11 windows float; X11 fullscreen covers
//     the usable area.
//   * layer-shell bars with exclusive-zone tiling reserve.

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits.h>
#include <linux/input-event-codes.h>
// pthread.h before the keyword hacks below: glibc declares a C++ cleanup
// `class` in it, and it can otherwise be first-pulled by a header inside
// the hack window (seen under -O3), where `class` is macro-renamed.
#include <pthread.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

// wlroots is a C library whose headers carry no `extern "C"` guards, so
// force C linkage here. They also use C-only `static` array bounds (e.g. in
// wlr_scene.h, wlr/render/color.h) which C++ rejects, and `namespace` /
// `class` as struct field names (layer-shell and xwayland headers), which
// are reserved C++ keywords — so neutralize all three for these headers
// only. Benign: it just drops `static` from `static inline` helpers and
// `static const` constants (still valid), and renames the
// `namespace`/`class` identifiers (layout unchanged). Our own C++ below
// the matching #undefs is unaffected.
#define static
#define namespace _aquawm_namespace
#define class _aquawm_class
extern "C" {
#include <wlr/backend.h>
#include <wlr/backend/wayland.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
}
#undef static
#undef namespace
#undef class

#include <algorithm>
#include <vector>

#include <drm_fourcc.h>

#include "config.hpp"
#include "configwatch.hpp"
#include "layers.hpp"
#include "tiling.hpp"
#include "wallpaper.hpp"
#include "warnbar.hpp"

extern "C" {
// wlr_xwayland_surface.class is unnameable in C++ (reserved keyword);
// these C helpers bridge it.
const char *aquawm_xwayland_class(const struct wlr_xwayland_surface *surface);
const char *aquawm_xwayland_title(const struct wlr_xwayland_surface *surface);
}

// freetype has proper extern "C" guards; include normally (needs the
// freetype2/pkg-config include dirs, wired in CMakeLists.txt). fontconfig
// locates a monospace font at runtime on every distro (NixOS scatters
// fonts per-package, so hardcoded paths cannot work there).
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

namespace {

struct Server;
struct View;
struct XView;

struct Output {
    Server *server = nullptr;
    struct wlr_output *wlr_output = nullptr;
    struct wlr_scene_buffer *bg = nullptr; // wallpaper node (bottom layer)
    int bg_w = -1;
    int bg_h = -1;
    // Tiling area after layer-shell exclusive zones (Phase 3b). Equals the
    // full output box until arrange_layers runs.
    struct wlr_box usable_area{};
    bool usable_valid = false;
    // Fallback warning bar (built-in defaults in effect): scene node +
    // backing buffer, sized to the output on (re)creation.
    struct wlr_scene_buffer *warn = nullptr;
    struct wlr_buffer *warn_buf = nullptr;
    int warn_w = -1;
    struct wl_listener frame{};
    struct wl_listener destroy{};
};

struct View {
    Server *server = nullptr;
    struct wlr_xdg_toplevel *toplevel = nullptr;
    struct wlr_scene_tree *scene_tree = nullptr;
    // Tiling state: position in layout coordinates, floating override,
    // workspace index, and last size we configured (to avoid loops).
    int x = 0;
    int y = 0;
    bool floating = false;
    int workspace = 0;
    int applied_w = 0;
    int applied_h = 0;
    bool mapped = false;
    struct wl_listener map{};
    struct wl_listener unmap{};
    struct wl_listener commit{};
    struct wl_listener destroy{};
};

// X11 window (Phase 3c): same tiling state as View, driven by
// wlr_xwayland_surface events instead of xdg-shell ones. The scene node is
// created on associate (only then does xsurface->surface exist).
struct XView {
    Server *server = nullptr;
    struct wlr_xwayland_surface *xsurface = nullptr;
    struct wlr_scene_tree *scene_tree = nullptr;
    int x = 0;
    int y = 0;
    bool floating = false;
    bool fullscreen = false;
    int workspace = 0;
    int applied_w = 0;
    int applied_h = 0;
    bool mapped = false;
    struct wl_listener associate{};
    struct wl_listener dissociate{};
    struct wl_listener destroy{};
    struct wl_listener request_configure{};
    struct wl_listener request_activate{};
    struct wl_listener request_close{};
    struct wl_listener request_move{};
    struct wl_listener request_resize{};
    struct wl_listener request_maximize{};
    struct wl_listener request_fullscreen{};
};

// One stacking entry (Phase 3c): exactly one pointer is set. Front of
// server->tiles is topmost (most recently focused), across both protocols.
struct AnyView {
    View *v = nullptr;
    XView *x = nullptr;
};

// Per-keyboard state: owns the listeners so wl_container_of can reach both
// the wlr_keyboard and our server from any keyboard event. The destroy
// listener hangs off the input device, which is what emits destroy.
struct Keyboard {
    Server *server = nullptr;
    struct wlr_keyboard *kbd = nullptr;
    struct wlr_input_device *device = nullptr;
    bool warned_no_state = false;
    struct wl_listener key{};
    struct wl_listener modifiers{};
    struct wl_listener destroy{};
};

// Layer-shell bar surface (Phase 3b): a client bar (e.g. waybar) rendered
// in protocol layer order. The scene helper (scene) owns map/unmap
// visibility of the node; this wrapper owns configure (exclusive zone) and
// its own lifetime. Input still goes to tiling views only.
struct LayerSurface {
    Server *server = nullptr;
    struct wlr_layer_surface_v1 *layer = nullptr;
    struct wlr_scene_layer_surface_v1 *scene = nullptr;
    struct wl_listener map{};
    struct wl_listener unmap{};
    struct wl_listener commit{};
    struct wl_listener destroy{};
};

// What the pointer is currently doing: passing events through, or
// dragging a grabbed view.
enum class CursorMode {
    Passthrough,
    Move,
    Resize,
};

// Cursor event listeners; owned by the Server for its whole lifetime.
struct CursorEvents {
    Server *server = nullptr;
    struct wl_listener motion{};
    struct wl_listener motion_absolute{};
    struct wl_listener button{};
    struct wl_listener axis{};
    struct wl_listener frame{};
};

// Shared wallpaper image uploaded once into an allocator buffer.
struct Wallpaper {
    struct wlr_buffer *buffer = nullptr;
    int img_w = 0;
    int img_h = 0;
    std::string tried_path; // last path we attempted (avoid open() per frame)
};

struct Server {
    struct wl_display *display = nullptr;
    struct wlr_backend *backend = nullptr;
    struct wlr_session *session = nullptr;
    struct wlr_renderer *renderer = nullptr;
    struct wlr_allocator *allocator = nullptr;
    struct wlr_compositor *compositor = nullptr;
    struct wlr_xwayland *xwayland = nullptr;
    struct wlr_scene *scene = nullptr;
    struct wlr_scene_output_layout *scene_layout = nullptr;
    struct wlr_output_layout *output_layout = nullptr;
    struct wlr_xdg_shell *xdg_shell = nullptr;
    struct wlr_xdg_output_manager_v1 *xdg_output_manager = nullptr;
    struct wlr_layer_shell_v1 *layer_shell = nullptr;
    // One scene tree per protocol layer (Phase 3b), ordered under the scene
    // root as background < bottom < views < top < overlay. Tiling views
    // live in view_tree so focus raise_to_top never covers the top layers.
    struct wlr_scene_tree *view_tree = nullptr;
    struct wlr_scene_tree *layer_trees[4] = {nullptr, nullptr, nullptr, nullptr};
    struct wlr_cursor *cursor = nullptr;
    struct wlr_xcursor_manager *cursor_mgr = nullptr;
    struct wlr_seat *seat = nullptr;
    CursorEvents cursor_events{};
    CursorMode cursor_mode = CursorMode::Passthrough;
    AnyView grabbed_tile{};
    bool has_grab = false;
    // True when the active grab pulled its window out of tiling: dropping
    // it (move release) docks it back. Resize grabs never dock, so an
    // explicit resize keeps its size.
    bool grab_from_tiled = false;
    uint32_t grab_button = 0;
    double grab_lx = 0;
    double grab_ly = 0;
    int grab_vx = 0;
    int grab_vy = 0;
    int grab_vw = 0;
    int grab_vh = 0;
    bool cursor_is_default = true;

    struct wl_listener new_output{};
    struct wl_listener new_input{};
    struct wl_listener new_toplevel{};
    struct wl_listener new_layer_surface{};
    struct wl_listener new_xwayland_surface{};
    struct wl_listener request_cursor{};
    struct wl_listener backend_destroy{};
    // Set when the backend died on its own (host disconnect): its listeners
    // were already detached in on_backend_destroy, so main() teardown must
    // not remove them again (double wl_list_remove corrupts the list).
    bool backend_gone = false;

    // Config auto-reload (save-and-reload): inotify fd watching the config
    // directory, debounce timer, and the watched basename.
    int config_inotify_fd = -1;
    struct wl_event_source *config_timer = nullptr;
    std::string config_watch_name;

    std::vector<Output *> outputs;
    std::vector<LayerSurface *> layers;
    // Tiling stack across both protocols (Phase 3c). Front of the vector
    // is topmost (most recently focused).
    std::vector<AnyView> tiles;

    int active_workspace = 0;
    aquawm::Config config;
    std::string config_path;
    // True while the running config is built-in defaults (missing/broken
    // file). Drives the fallback warning bar; cleared on a good (re)load.
    bool config_fallback = false;
    // freetype handle for the warning bar, initialized once on first use.
    FT_Library ft_lib = nullptr;
    bool ft_ready = false;
    Wallpaper wallpaper;

    const char *socket = nullptr;
};

void focus_view(Server *server, View *view);
void focus_xview(Server *server, XView *xview);
void drop_wallpaper(Server *server);
void arrange(Server *server);
void arrange_layers(Server *server);

// Apply matching window rules (file order, later wins): floating and/or
// target workspace. Runs before arrange+focus so tiled windows land in
// the right layout immediately.
void apply_rules(Server *server, const std::string &app_id,
    const std::string &title, bool is_xwayland, bool &floating, int &workspace) {
    for (const aquawm::Rule &rule : server->config.rules) {
        if (!aquawm::rule_matches(rule, app_id, title, is_xwayland)) {
            continue;
        }
        if (rule.floating.has_value()) {
            floating = rule.floating.value();
        }
        if (rule.workspace >= 1 && rule.workspace <= server->config.workspaces) {
            workspace = rule.workspace - 1;
        }
    }
}
void ensure_warnbar(Server *server, Output *output);
void drop_warnbars(Server *server);
bool warn_bar_active(Server *server);

// --- AnyView helpers (Phase 3c): protocol-agnostic tile access ---------
// A tile whose surface is null (XView before associate) is inert: every
// helper below treats it as unmapped and every setter is a no-op.
struct wlr_surface *any_surface(const AnyView &t) {
    if (t.v != nullptr) {
        return t.v->toplevel->base->surface;
    }
    if (t.x != nullptr && t.x->xsurface->surface != nullptr) {
        return t.x->xsurface->surface;
    }
    return nullptr;
}

struct wlr_scene_tree *any_tree(const AnyView &t) {
    if (t.v != nullptr) {
        return t.v->scene_tree;
    }
    if (t.x != nullptr) {
        return t.x->scene_tree;
    }
    return nullptr;
}

bool any_mapped(const AnyView &t) {
    if (any_surface(t) == nullptr) {
        return false;
    }
    return t.v != nullptr ? t.v->mapped : t.x->mapped;
}

int any_workspace(const AnyView &t) {
    return t.v != nullptr ? t.v->workspace : t.x->workspace;
}

bool any_floating(const AnyView &t) {
    return t.v != nullptr ? t.v->floating : t.x->floating;
}

void any_set_floating(const AnyView &t, bool floating) {
    if (t.v != nullptr) {
        t.v->floating = floating;
    } else if (t.x != nullptr) {
        t.x->floating = floating;
    }
}

bool any_fullscreen(const AnyView &t) {
    return t.x != nullptr && t.x->fullscreen;
}

void any_set_fullscreen(const AnyView &t, bool fullscreen) {
    if (t.x != nullptr) {
        t.x->fullscreen = fullscreen;
    }
}

// Current tile box in layout coordinates (for hit-testing and grabs).
struct wlr_box any_box(const AnyView &t) {
    // Tile box: position plus live client size. Clients with nonzero
    // window-geometry offsets (kitty, Firefox CSD) report them separately;
    // any_set_pos compensates the scene node, so the box always matches
    // the pixels on screen.
    if (t.v != nullptr) {
        struct wlr_box geom = t.v->toplevel->base->geometry;
        return {t.v->x, t.v->y, geom.width, geom.height};
    }
    // Prefer the live surface size: cell-granular X clients (xterm) render
    // smaller than the last configured size, and hit-testing the stale
    // applied size creates a dead zone along the bottom/right edges.
    const int w = t.x->xsurface->width > 0 ? t.x->xsurface->width
        : (t.x->applied_w > 0 ? t.x->applied_w : 640);
    const int h = t.x->xsurface->height > 0 ? t.x->xsurface->height
        : (t.x->applied_h > 0 ? t.x->applied_h : 480);
    return {t.x->x, t.x->y, w, h};
}

void any_set_pos(const AnyView &t, int x, int y) {
    struct wlr_scene_tree *tree = any_tree(t);
    if (tree == nullptr) {
        return;
    }
    if (t.v != nullptr) {
        t.v->x = x;
        t.v->y = y;
        // The window rect lives at geometry offset inside the surface:
        // shift the node so geometry lands exactly on the tile.
        struct wlr_box geom = t.v->toplevel->base->geometry;
        wlr_scene_node_set_position(&tree->node, x - geom.x, y - geom.y);
    } else {
        t.x->x = x;
        t.x->y = y;
        wlr_scene_node_set_position(&tree->node, x, y);
    }
}

void any_commit_size(const AnyView &t, int w, int h) {
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    if (t.v != nullptr) {
        if (t.v->applied_w == w && t.v->applied_h == h) {
            return;
        }
        t.v->applied_w = w;
        t.v->applied_h = h;
        wlr_xdg_toplevel_set_size(t.v->toplevel, w, h);
    } else if (t.x != nullptr && t.x->xsurface->surface != nullptr) {
        if (t.x->applied_w == w && t.x->applied_h == h) {
            return;
        }
        t.x->applied_w = w;
        t.x->applied_h = h;
        wlr_xwayland_surface_configure(t.x->xsurface, t.x->x, t.x->y,
            static_cast<uint16_t>(w), static_cast<uint16_t>(h));
    }
}

void any_set_activated(const AnyView &t, bool active) {
    if (t.v != nullptr) {
        wlr_xdg_toplevel_set_activated(t.v->toplevel, active);
    } else if (t.x != nullptr && t.x->xsurface->surface != nullptr) {
        wlr_xwayland_surface_activate(t.x->xsurface, active);
    }
}

void any_close(const AnyView &t) {
    if (t.v != nullptr) {
        wlr_xdg_toplevel_send_close(t.v->toplevel);
    } else if (t.x != nullptr) {
        wlr_xwayland_surface_close(t.x->xsurface);
    }
}

bool any_matches(const AnyView &t, const AnyView &other) {
    return t.v == other.v && t.x == other.x;
}

void any_remove_tile(Server *server, const AnyView &t) {
    auto &tiles = server->tiles;
    tiles.erase(std::remove_if(tiles.begin(), tiles.end(),
        [&](const AnyView &e) { return any_matches(e, t); }),
        tiles.end());
}

void any_raise_to_top(Server *server, const AnyView &t) {
    struct wlr_scene_tree *tree = any_tree(t);
    if (tree != nullptr) {
        wlr_scene_node_raise_to_top(&tree->node);
    }
    any_remove_tile(server, t);
    server->tiles.insert(server->tiles.begin(), t);
}

void spawn_terminal(const std::string &configured) {
    if (fork() == 0) {
        setsid();
        // Configured terminal first, then well-known fallbacks (deduped).
        // execlp resolves via PATH, so this works for system packages,
        // nix profiles and /usr/local installs alike.
        const char *candidates[] = {
            configured.c_str(), "kitty", "foot", "weston-terminal", nullptr};
        const char *tried[4] = {nullptr, nullptr, nullptr, nullptr};
        for (int i = 0; candidates[i] != nullptr; ++i) {
            bool seen = false;
            for (int j = 0; j < i; ++j) {
                if (tried[j] != nullptr &&
                    std::strcmp(tried[j], candidates[i]) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                tried[i] = candidates[i];
                execlp(candidates[i], candidates[i], nullptr);
            }
        }
        // Reached only when no terminal exists on PATH: loud, with the
        // errno, instead of vanishing silently into _exit.
        std::fprintf(stderr,
            "aquawm: spawn-terminal: no terminal on PATH (tried %s): %s\n",
            configured.c_str(), std::strerror(errno));
        _exit(127);
    }
    wlr_log(WLR_INFO, "spawn-terminal requested (Super+Return)");
}

void any_set_workspace(const AnyView &t, int ws) {
    if (t.v != nullptr) {
        t.v->workspace = ws;
    } else if (t.x != nullptr) {
        t.x->workspace = ws;
    }
}

bool any_valid(const AnyView &t) {
    return t.v != nullptr || t.x != nullptr;
}

// Pixel-accurate hit test (tinywl-style): ask the scene which surface is
// under the cursor instead of doing box math. Handles nonzero window
// geometry, subsurfaces and popups; wallpaper, warnbar and layer bars are
// not tiles and never hit. Returns the tile plus the surface and its
// surface-local coordinates.
bool tile_at(Server *server, double lx, double ly, AnyView &hit,
    struct wlr_surface **surface, double *sx, double *sy) {
    if (server->view_tree == nullptr) {
        return false;
    }
    double nx = 0.0, ny = 0.0;
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene->tree.node, lx, ly, &nx, &ny);
    if (node == nullptr) {
        return false;
    }
    // Ascend to the direct child of view_tree; anything else is not a tile.
    struct wlr_scene_node *child = node;
    while (child->parent != nullptr && child->parent != server->view_tree) {
        child = &child->parent->node;
        if (child == &server->scene->tree.node) {
            return false;
        }
    }
    if (child->parent != server->view_tree) {
        return false;
    }
    for (const AnyView &t : server->tiles) {
        if (!any_mapped(t) || any_workspace(t) != server->active_workspace) {
            continue;
        }
        if (any_tree(t) == nullptr || &any_tree(t)->node != child) {
            continue;
        }
        struct wlr_surface *s = nullptr;
        if (node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_surface *ss =
                wlr_scene_surface_try_from_buffer(
                    wlr_scene_buffer_from_node(node));
            if (ss != nullptr) {
                s = ss->surface;
            }
        }
        if (s == nullptr) {
            s = any_surface(t);
        }
        if (s == nullptr) {
            return false;
        }
        hit = t;
        if (surface != nullptr) {
            *surface = s;
        }
        if (sx != nullptr) {
            *sx = nx;
        }
        if (sy != nullptr) {
            *sy = ny;
        }
        return true;
    }
    return false;
}

// Tile all mapped, non-floating views of the active workspace using the
// master-stack layout. Oldest window becomes master for a stable layout.
// Tiling fills the output's usable area (full box minus layer-shell
// exclusive zones, see arrange_layers), then applies gaps. A mapped
// fullscreen X11 window covers the whole usable area instead.
void arrange(Server *server) {
    if (server->outputs.empty()) {
        return;
    }
    Output *output = server->outputs.front();
    struct wlr_box area{};
    if (output->usable_valid) {
        area = output->usable_area;
    } else {
        wlr_output_layout_get_box(server->output_layout, output->wlr_output, &area);
    }
    // Fallback warning bar reserves its strip at the top when active.
    if (warn_bar_active(server)) {
        area.y += aquawm::WARN_BAR_HEIGHT;
        area.height -= aquawm::WARN_BAR_HEIGHT;
    }
    for (const AnyView &t : server->tiles) {
        if (any_mapped(t) && any_fullscreen(t) &&
            any_workspace(t) == server->active_workspace) {
            any_set_pos(t, area.x, area.y);
            any_commit_size(t, area.width, area.height);
            any_raise_to_top(server, t);
            return;
        }
    }
    const int gaps = server->config.gaps;
    area.x += gaps;
    area.y += gaps;
    area.width -= 2 * gaps;
    area.height -= 2 * gaps;
    if (area.width <= 0 || area.height <= 0) {
        return;
    }
    std::vector<AnyView> tiled;
    for (auto it = server->tiles.rbegin(); it != server->tiles.rend(); ++it) {
        if (any_mapped(*it) && !any_floating(*it) &&
            any_workspace(*it) == server->active_workspace) {
            tiled.push_back(*it);
        }
    }
    std::vector<aquawm::Box> boxes;
    const std::string &layout = server->config.layout;
    aquawm::Box area_box{area.x, area.y, area.width, area.height};
    if (layout == "dwindle") {
        boxes = aquawm::dwindle(static_cast<int>(tiled.size()), area_box,
            server->config.split_ratio);
    } else if (layout == "grid") {
        boxes = aquawm::grid(static_cast<int>(tiled.size()), area_box);
    } else if (layout == "monocle") {
        boxes = aquawm::monocle(static_cast<int>(tiled.size()), area_box);
    } else {
        boxes = aquawm::master_stack(static_cast<int>(tiled.size()), area_box,
            server->config.nmaster, server->config.mfact);
    }
    for (std::size_t i = 0; i < tiled.size(); ++i) {
        const AnyView &t = tiled[i];
        int w = boxes[i].w - 2 * gaps;
        int h = boxes[i].h - 2 * gaps;
        any_set_pos(t, boxes[i].x + gaps, boxes[i].y + gaps);
        any_commit_size(t, w, h);
    }
}

void focus_any(Server *server, const AnyView &t) {
    struct wlr_surface *prev_surface = server->seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = any_surface(t);
    if (prev_surface == surface) {
        return;
    }
    if (prev_surface != nullptr) {
        struct wlr_xdg_toplevel *prev_top =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_top != nullptr) {
            wlr_xdg_toplevel_set_activated(prev_top, false);
        } else if (server->xwayland != nullptr) {
            struct wlr_xwayland_surface *prev_x =
                wlr_xwayland_surface_try_from_wlr_surface(prev_surface);
            if (prev_x != nullptr) {
                wlr_xwayland_surface_activate(prev_x, false);
            }
        }
    }
    if (surface == nullptr) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
        return;
    }
    // Raise to the top both in the scene and in our focus order.
    any_raise_to_top(server, t);
    any_set_activated(t, true);
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (keyboard != nullptr) {
        wlr_seat_keyboard_notify_enter(server->seat, surface, keyboard->keycodes,
            keyboard->num_keycodes, &keyboard->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(server->seat, surface, nullptr, 0, nullptr);
    }
}

void focus_view(Server *server, View *view) {
    if (view == nullptr) {
        focus_any(server, AnyView{});
        return;
    }
    focus_any(server, AnyView{view, nullptr});
}

void focus_xview(Server *server, XView *xview) {
    if (xview == nullptr) {
        focus_any(server, AnyView{});
        return;
    }
    focus_any(server, AnyView{nullptr, xview});
}

// Most recently focused mapped tile on the active workspace, if any.
bool top_visible(Server *server, AnyView &top) {
    for (const AnyView &t : server->tiles) {
        if (any_mapped(t) && any_workspace(t) == server->active_workspace) {
            top = t;
            return true;
        }
    }
    return false;
}

void switch_workspace(Server *server, int ws) {
    if (ws < 0 || ws >= server->config.workspaces ||
        ws == server->active_workspace) {
        return;
    }
    server->active_workspace = ws;
    for (const AnyView &t : server->tiles) {
        struct wlr_scene_tree *tree = any_tree(t);
        if (tree != nullptr) {
            wlr_scene_node_set_enabled(&tree->node,
                any_mapped(t) && any_workspace(t) == ws);
        }
    }
    arrange(server);
    AnyView top{};
    if (top_visible(server, top)) {
        focus_any(server, top);
    }
}

void focus_cycle(Server *server, int dir) {
    std::vector<AnyView> vis;
    for (const AnyView &t : server->tiles) {
        if (any_mapped(t) && any_workspace(t) == server->active_workspace) {
            vis.push_back(t);
        }
    }
    if (vis.empty()) {
        return;
    }
    struct wlr_surface *cur = server->seat->keyboard_state.focused_surface;
    std::size_t idx = 0;
    bool found = false;
    for (std::size_t i = 0; i < vis.size(); ++i) {
        if (any_surface(vis[i]) == cur) {
            idx = i;
            found = true;
            break;
        }
    }
    if (!found) {
        focus_any(server, vis.front());
        return;
    }
    idx = (idx + static_cast<std::size_t>(dir) + vis.size()) % vis.size();
    focus_any(server, vis[idx]);
}

void on_view_map(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, map);
    Server *server = view->server;
    view->mapped = true;
    view->workspace = server->active_workspace;
    {
        const char *app_id =
            view->toplevel->app_id != nullptr ? view->toplevel->app_id : "";
        const char *title =
            view->toplevel->title != nullptr ? view->toplevel->title : "";
        apply_rules(server, app_id, title, false, view->floating,
            view->workspace);
    }
    wlr_scene_node_set_enabled(&view->scene_tree->node, true);
    struct wlr_box geom = view->toplevel->base->geometry;
    wlr_log(WLR_INFO,
        "xdg toplevel mapped: app_id=%s ws=%d tile=(%d,%d) geom=(%d,%d %dx%d)",
        view->toplevel->app_id != nullptr ? view->toplevel->app_id : "?",
        view->workspace, view->x, view->y, geom.x, geom.y, geom.width,
        geom.height);
    arrange(server);
    focus_view(server, view);
}

void on_view_unmap(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, unmap);
    Server *server = view->server;
    wlr_log(WLR_INFO, "xdg toplevel unmapped: app_id=%s",
        view->toplevel->app_id != nullptr ? view->toplevel->app_id : "?");
    view->mapped = false;
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    arrange(server);
    if (server->seat->keyboard_state.focused_surface ==
        view->toplevel->base->surface) {
        AnyView top{};
        if (top_visible(server, top)) {
            focus_any(server, top);
        }
    }
}

void on_view_commit(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, commit);
    if (view->toplevel->base->initial_commit) {
        // Suggest a default size; the tiling phase will set this per layout.
        wlr_xdg_toplevel_set_size(view->toplevel, 640, 480);
    }
    // Clients can move their window geometry at any commit: keep the node
    // aligned so geometry keeps landing on the tile (no-op when unchanged).
    struct wlr_box geom = view->toplevel->base->geometry;
    wlr_scene_node_set_position(&view->scene_tree->node, view->x - geom.x,
        view->y - geom.y);
}

void on_view_destroy(struct wl_listener *listener, void * /*data*/) {
    View *view = wl_container_of(listener, view, destroy);
    Server *server = view->server;
    // Exclude the dying view from layout before arranging: configuring a
    // toplevel from inside its own destroy event would use-after-free.
    view->mapped = false;
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->destroy.link);
    any_remove_tile(server, AnyView{view, nullptr});
    arrange(server);
    // The toplevel is going away: never let it keep keyboard focus, and
    // don't dereference its surface below (it may already be half-torn-down).
    if (server->seat->keyboard_state.focused_surface != nullptr) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
    AnyView top{};
    if (top_visible(server, top)) {
        focus_any(server, top);
    }
    delete view;
}

void on_new_toplevel(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = static_cast<struct wlr_xdg_toplevel *>(data);

    View *view = new View();
    view->server = server;
    view->toplevel = toplevel;
    // Tiling views live in view_tree so focus raise_to_top stays below the
    // top/overlay layer trees (Phase 3b scene order).
    struct wlr_scene_tree *view_parent =
        server->view_tree != nullptr ? server->view_tree : &server->scene->tree;
    view->scene_tree = wlr_scene_xdg_surface_create(view_parent, toplevel->base);
    wlr_scene_node_set_enabled(&view->scene_tree->node, false);

    view->map.notify = on_view_map;
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    view->unmap.notify = on_view_unmap;
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    view->commit.notify = on_view_commit;
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    view->destroy.notify = on_view_destroy;
    wl_signal_add(&toplevel->events.destroy, &view->destroy);

    server->tiles.push_back(AnyView{view, nullptr});
}

// --- X11 windows (Phase 3c) ----------------------------------------------
// An XView joins the tiling stack on associate (when its surface exists)
// and leaves on destroy. Override-redirect windows (menus, tooltips) and
// fullscreen windows float above tiling; everything else tiles.
void xview_update_floating(XView *xview) {
    xview->floating =
        xview->xsurface->override_redirect || xview->fullscreen;
}

void on_xview_associate(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, associate);
    Server *server = xview->server;
    struct wlr_scene_tree *parent =
        server->view_tree != nullptr ? server->view_tree : &server->scene->tree;
    xview->scene_tree =
        wlr_scene_subsurface_tree_create(parent, xview->xsurface->surface);
    if (xview->scene_tree == nullptr) {
        wlr_log(WLR_ERROR, "failed to create scene node for X11 window");
        return;
    }
    xview->mapped = true;
    xview->workspace = server->active_workspace;
    {
        const char *cls = aquawm_xwayland_class(xview->xsurface);
        const char *title = aquawm_xwayland_title(xview->xsurface);
        apply_rules(server, cls != nullptr ? cls : "",
            title != nullptr ? title : "", true, xview->floating,
            xview->workspace);
    }
    xview->fullscreen = xview->xsurface->fullscreen;
    xview_update_floating(xview);
    arrange(server);
    focus_xview(server, xview);
    wlr_log(WLR_INFO, "X11 window associated (title=%s fullscreen=%d)",
        xview->xsurface->title != nullptr ? xview->xsurface->title : "?",
        xview->fullscreen ? 1 : 0);
}

void on_xview_dissociate(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, dissociate);
    Server *server = xview->server;
    xview->mapped = false;
    if (xview->scene_tree != nullptr) {
        wlr_scene_node_destroy(&xview->scene_tree->node);
        xview->scene_tree = nullptr;
    }
    arrange(server);
    AnyView top{};
    if (top_visible(server, top)) {
        focus_any(server, top);
    }
}

void on_xview_destroy(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, destroy);
    Server *server = xview->server;
    xview->mapped = false;
    wl_list_remove(&xview->associate.link);
    wl_list_remove(&xview->dissociate.link);
    wl_list_remove(&xview->destroy.link);
    wl_list_remove(&xview->request_configure.link);
    wl_list_remove(&xview->request_activate.link);
    wl_list_remove(&xview->request_close.link);
    wl_list_remove(&xview->request_move.link);
    wl_list_remove(&xview->request_resize.link);
    wl_list_remove(&xview->request_maximize.link);
    wl_list_remove(&xview->request_fullscreen.link);
    any_remove_tile(server, AnyView{nullptr, xview});
    if (xview->scene_tree != nullptr) {
        wlr_scene_node_destroy(&xview->scene_tree->node);
        xview->scene_tree = nullptr;
    }
    arrange(server);
    if (server->seat->keyboard_state.focused_surface != nullptr) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
    AnyView top{};
    if (top_visible(server, top)) {
        focus_any(server, top);
    }
    delete xview;
}

void on_xview_request_configure(struct wl_listener *listener, void *data) {
    XView *xview = wl_container_of(listener, xview, request_configure);
    auto *event =
        static_cast<struct wlr_xwayland_surface_configure_event *>(data);
    if (xview->floating || !xview->mapped) {
        // Floating or not yet placed: honor the requested geometry.
        xview->x = event->x;
        xview->y = event->y;
        if (xview->scene_tree != nullptr) {
            wlr_scene_node_set_position(&xview->scene_tree->node, event->x,
                event->y);
        }
        xview->applied_w = event->width;
        xview->applied_h = event->height;
    }
    // Tiled windows keep the arranged geometry; either way ack so the
    // client stops waiting. Unmapped surfaces have no size yet.
    if (xview->scene_tree != nullptr) {
        wlr_xwayland_surface_configure(xview->xsurface, xview->x, xview->y,
            static_cast<uint16_t>(xview->applied_w > 0 ? xview->applied_w : 640),
            static_cast<uint16_t>(xview->applied_h > 0 ? xview->applied_h : 480));
    }
}

void on_xview_request_activate(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, request_activate);
    focus_xview(xview->server, xview);
}

void on_xview_request_close(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, request_close);
    wlr_xwayland_surface_close(xview->xsurface);
}

void on_xview_request_move(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, request_move);
    Server *server = xview->server;
    AnyView t{nullptr, xview};
    server->grab_from_tiled = !any_floating(t);
    if (server->grab_from_tiled) {
        any_set_floating(t, true);
        arrange(server);
    }
    any_raise_to_top(server, t);
    server->grabbed_tile = t;
    server->has_grab = true;
    server->cursor_mode = CursorMode::Move;
    server->grab_button = 0;
    server->grab_lx = server->cursor->x;
    server->grab_ly = server->cursor->y;
    struct wlr_box box = any_box(t);
    server->grab_vx = box.x;
    server->grab_vy = box.y;
    server->grab_vw = box.width;
    server->grab_vh = box.height;
}

void on_xview_request_resize(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, request_resize);
    Server *server = xview->server;
    AnyView t{nullptr, xview};
    server->grab_from_tiled = !any_floating(t);
    if (server->grab_from_tiled) {
        any_set_floating(t, true);
        arrange(server);
    }
    any_raise_to_top(server, t);
    server->grabbed_tile = t;
    server->has_grab = true;
    server->cursor_mode = CursorMode::Resize;
    server->grab_button = 0;
    server->grab_lx = server->cursor->x;
    server->grab_ly = server->cursor->y;
    struct wlr_box box = any_box(t);
    server->grab_vx = box.x;
    server->grab_vy = box.y;
    server->grab_vw = box.width;
    server->grab_vh = box.height;
}

void on_xview_request_maximize(struct wl_listener *listener, void * /*data*/) {
    // Tiled windows already fill their tile; accept the requested state so
    // the client stops waiting, then re-tile (a no-op geometrically).
    // (request signals carry no payload; the surface fields are pre-updated.)
    XView *xview = wl_container_of(listener, xview, request_maximize);
    wlr_xwayland_surface_set_maximized(xview->xsurface,
        xview->xsurface->maximized_horz, xview->xsurface->maximized_vert);
    arrange(xview->server);
}

void on_xview_request_fullscreen(struct wl_listener *listener, void * /*data*/) {
    XView *xview = wl_container_of(listener, xview, request_fullscreen);
    Server *server = xview->server;
    const bool fullscreen = xview->xsurface->fullscreen;
    any_set_fullscreen(AnyView{nullptr, xview}, fullscreen);
    xview_update_floating(xview);
    wlr_xwayland_surface_set_fullscreen(xview->xsurface, fullscreen);
    arrange(server);
    if (fullscreen) {
        focus_xview(server, xview);
    }
}

void on_new_xwayland_surface(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_xwayland_surface);
    struct wlr_xwayland_surface *xsurface =
        static_cast<struct wlr_xwayland_surface *>(data);
    XView *xview = new XView();
    xview->server = server;
    xview->xsurface = xsurface;
    xview->associate.notify = on_xview_associate;
    wl_signal_add(&xsurface->events.associate, &xview->associate);
    xview->dissociate.notify = on_xview_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &xview->dissociate);
    xview->destroy.notify = on_xview_destroy;
    wl_signal_add(&xsurface->events.destroy, &xview->destroy);
    xview->request_configure.notify = on_xview_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &xview->request_configure);
    xview->request_activate.notify = on_xview_request_activate;
    wl_signal_add(&xsurface->events.request_activate, &xview->request_activate);
    xview->request_close.notify = on_xview_request_close;
    wl_signal_add(&xsurface->events.request_close, &xview->request_close);
    xview->request_move.notify = on_xview_request_move;
    wl_signal_add(&xsurface->events.request_move, &xview->request_move);
    xview->request_resize.notify = on_xview_request_resize;
    wl_signal_add(&xsurface->events.request_resize, &xview->request_resize);
    xview->request_maximize.notify = on_xview_request_maximize;
    wl_signal_add(&xsurface->events.request_maximize, &xview->request_maximize);
    xview->request_fullscreen.notify = on_xview_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen,
        &xview->request_fullscreen);
    server->tiles.push_back(AnyView{nullptr, xview});
}

// --- Layer-shell bars (Phase 3b) ------------------------------------------
// Configure every layer surface on `output` overlay-first and shrink the
// output's usable area by each mapped surface's positive exclusive zone.
// The scene helper positions each node and folds the zone (+ margin) into
// `usable` for the next surface, so stacked bars compose. Tiling then
// fills what remains (see arrange). Unmapped surfaces never shrink.
void output_arrange_layers(Server *server, Output *output) {
    struct wlr_box full{};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &full);
    struct wlr_box usable = full;
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
         layer >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND; --layer) {
        for (LayerSurface *ls : server->layers) {
            if (ls->layer->output != output->wlr_output) {
                continue;
            }
            if ((int)ls->layer->current.layer != layer) {
                continue;
            }
            wlr_scene_layer_surface_v1_configure(ls->scene, &full, &usable);
        }
    }
    output->usable_area = usable;
    output->usable_valid = true;
}

void arrange_layers(Server *server) {
    for (Output *output : server->outputs) {
        output_arrange_layers(server, output);
    }
    arrange(server);
}

void on_layer_map(struct wl_listener *listener, void * /*data*/) {
    LayerSurface *ls = wl_container_of(listener, ls, map);
    // NOTE: wlr_layer_surface_v1 has a `namespace` field, but `namespace`
    // is a C++ keyword, so log the output name instead.
    wlr_log(WLR_INFO, "layer surface mapped: layer=%d zone=%d output=%s",
        (int)ls->layer->current.layer, ls->layer->current.exclusive_zone,
        ls->layer->output != nullptr && ls->layer->output->name != nullptr
            ? ls->layer->output->name
            : "?");
    arrange_layers(ls->server);
}

void on_layer_unmap(struct wl_listener *listener, void * /*data*/) {
    LayerSurface *ls = wl_container_of(listener, ls, unmap);
    arrange_layers(ls->server);
}

void on_layer_commit(struct wl_listener *listener, void * /*data*/) {
    LayerSurface *ls = wl_container_of(listener, ls, commit);
    Server *server = ls->server;
    struct wlr_layer_surface_v1 *layer = ls->layer;
    if (layer->current.committed == 0) {
        return;
    }
    // A client may move between protocol layers; keep the scene node in
    // the matching layer tree so render order stays correct.
    if (layer->current.committed & WLR_LAYER_SURFACE_V1_STATE_LAYER) {
        int want = (int)layer->current.layer;
        if (want >= ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND &&
            want <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY && ls->scene != nullptr) {
            wlr_scene_node_reparent(&ls->scene->tree->node,
                server->layer_trees[want]);
        }
    }
    arrange_layers(server);
}

void on_layer_destroy(struct wl_listener *listener, void * /*data*/) {
    LayerSurface *ls = wl_container_of(listener, ls, destroy);
    Server *server = ls->server;
    // The scene helper frees its own node via its destroy listener; drop
    // only our wrapper and listeners here.
    wl_list_remove(&ls->map.link);
    wl_list_remove(&ls->unmap.link);
    wl_list_remove(&ls->commit.link);
    wl_list_remove(&ls->destroy.link);
    auto it = std::find(server->layers.begin(), server->layers.end(), ls);
    if (it != server->layers.end()) {
        server->layers.erase(it);
    }
    delete ls;
    arrange_layers(server);
}

void on_new_layer_surface(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *layer =
        static_cast<struct wlr_layer_surface_v1 *>(data);
    // Layer clients may leave the output unassigned; pin to the primary
    // output (single-output layout, same assumption as arrange).
    if (layer->output == nullptr) {
        if (server->outputs.empty()) {
            wlr_log(WLR_ERROR,
                "layer surface with no output and no outputs yet; ignoring");
            return;
        }
        layer->output = server->outputs.front()->wlr_output;
    }
    // current.layer is already set from the creation request (wlroots
    // assigns it before emitting new_surface).
    int want = (int)layer->current.layer;
    if (want < ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND ||
        want > ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
        wlr_log(WLR_ERROR, "layer surface with invalid layer %d; ignoring", want);
        return;
    }
    LayerSurface *ls = new LayerSurface();
    ls->server = server;
    ls->layer = layer;
    ls->scene = wlr_scene_layer_surface_v1_create(server->layer_trees[want], layer);
    if (ls->scene == nullptr) {
        wlr_log(WLR_ERROR, "failed to create scene node for layer surface");
        delete ls;
        return;
    }
    ls->map.notify = on_layer_map;
    wl_signal_add(&layer->surface->events.map, &ls->map);
    ls->unmap.notify = on_layer_unmap;
    wl_signal_add(&layer->surface->events.unmap, &ls->unmap);
    ls->commit.notify = on_layer_commit;
    wl_signal_add(&layer->surface->events.commit, &ls->commit);
    ls->destroy.notify = on_layer_destroy;
    wl_signal_add(&layer->events.destroy, &ls->destroy);
    server->layers.push_back(ls);
    wlr_log(WLR_INFO, "new layer surface: layer=%d output=%s", want,
        layer->output->name != nullptr ? layer->output->name : "?");
}

bool focused_tile(Server *server, AnyView &focused) {
    struct wlr_surface *s = server->seat->keyboard_state.focused_surface;
    if (s == nullptr) {
        return false;
    }
    for (const AnyView &t : server->tiles) {
        if (any_mapped(t) && any_surface(t) == s) {
            focused = t;
            return true;
        }
    }
    return false;
}

uint32_t wlr_to_tile_mods(uint32_t wlr_mods) {
    uint32_t mods = 0;
    if (wlr_mods & WLR_MODIFIER_SHIFT) {
        mods |= aquawm::MOD_SHIFT;
    }
    if (wlr_mods & WLR_MODIFIER_CTRL) {
        mods |= aquawm::MOD_CTRL;
    }
    if (wlr_mods & WLR_MODIFIER_ALT) {
        mods |= aquawm::MOD_ALT;
    }
    if (wlr_mods & WLR_MODIFIER_LOGO) {
        mods |= aquawm::MOD_SUPER;
    }
    return mods;
}

// Re-read the config file and apply it: fix up workspace assignments,
// refresh visibility, re-tile, refocus. Keeps the old config on failure.
bool reload_config(Server *server) {
    aquawm::Config next = server->config;
    std::string error;
    if (!aquawm::load_config_file(server->config_path.c_str(), next, error)) {
        wlr_log(WLR_ERROR, "config reload failed (%s): %s",
            server->config_path.c_str(), error.c_str());
        return false;
    }
    server->config = std::move(next);
    // A successful reload means a valid file: no longer fallback.
    server->config_fallback = false;
    if (server->active_workspace >= server->config.workspaces) {
        server->active_workspace = server->config.workspaces - 1;
    }
    for (const AnyView &t : server->tiles) {
        if (any_workspace(t) >= server->config.workspaces) {
            any_set_workspace(t, 0);
        }
        struct wlr_scene_tree *tree = any_tree(t);
        if (tree != nullptr) {
            wlr_scene_node_set_enabled(&tree->node,
                any_mapped(t) && any_workspace(t) == server->active_workspace);
        }
    }
    arrange(server);
    AnyView top{};
    if (top_visible(server, top)) {
        focus_any(server, top);
    }
    wlr_log(WLR_INFO, "config reloaded: gaps=%d mfact=%.2f nmaster=%d "
            "workspaces=%d binds=%zu",
        server->config.gaps, static_cast<double>(server->config.mfact),
        server->config.nmaster, server->config.workspaces,
        server->config.keys.size());
    // A changed wallpaper path rebuilds lazily on the next frame; a
    // changed fallback state rebuilds the warning bar the same way.
    drop_wallpaper(server);
    drop_warnbars(server);
    return true;
}

void run_action(Server *server, const aquawm::Keybind &bind) {
    const std::string &a = bind.action;
    if (a == "spawn-terminal") {
        spawn_terminal(server->config.terminal);
    } else if (a == "close") {
        AnyView focused{};
        if (focused_tile(server, focused)) {
            any_close(focused);
        }
    } else if (a == "quit") {
        wl_display_terminate(server->display);
    } else if (a == "focus-next") {
        focus_cycle(server, +1);
    } else if (a == "focus-prev") {
        focus_cycle(server, -1);
    } else if (a == "toggle-floating") {
        AnyView focused{};
        if (focused_tile(server, focused)) {
            any_set_floating(focused, !any_floating(focused));
            // Keep the window where it is and on top while floating.
            any_raise_to_top(server, focused);
            arrange(server);
        }
    } else if (a == "cycle-layout") {
        const auto &names = aquawm::layout_names();
        std::size_t idx = 0;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == server->config.layout) {
                idx = i;
                break;
            }
        }
        server->config.layout = names[(idx + 1) % names.size()];
        wlr_log(WLR_INFO, "layout: %s", server->config.layout.c_str());
        arrange(server);
    } else if (a == "workspace") {
        switch_workspace(server, bind.arg - 1);
    } else if (a == "move-to-workspace") {
        AnyView focused{};
        int ws = bind.arg - 1;
        if (focused_tile(server, focused) && ws >= 0 &&
            ws < server->config.workspaces) {
            any_set_workspace(focused, ws);
            struct wlr_scene_tree *tree = any_tree(focused);
            if (tree != nullptr) {
                wlr_scene_node_set_enabled(&tree->node,
                    any_mapped(focused) && ws == server->active_workspace);
            }
            arrange(server);
            AnyView top{};
            if (top_visible(server, top)) {
                focus_any(server, top);
            }
        }
    } else if (a == "reload-config") {
        reload_config(server);
    }
}

bool handle_keybinding(Server *server, xkb_keysym_t sym, uint32_t modifiers) {
    const uint32_t mods = wlr_to_tile_mods(modifiers);
    for (const aquawm::Keybind &bind : server->config.keys) {
        if (bind.mods == mods && bind.keysym == sym) {
            run_action(server, bind);
            return true;
        }
    }
    return false;
}

int on_reload_signal(int /*signal_number*/, void *data) {
    Server *server = static_cast<Server *>(data);
    reload_config(server);
    return 0;
}

// Debounced save-and-reload (Hyprland-style): the inotify dispatch arms a
// one-shot timer; a save burst re-arms it instead of reloading per event.
int on_config_debounce(void *data) {
    Server *server = static_cast<Server *>(data);
    wlr_log(WLR_INFO, "config file saved; reloading %s",
        server->config_path.c_str());
    reload_config(server);
    return 0;
}

int on_config_inotify(int fd, uint32_t /*mask*/, void *data) {
    Server *server = static_cast<Server *>(data);
    alignas(struct inotify_event) char buf[16 * (sizeof(struct inotify_event) + NAME_MAX + 1)];
    const ssize_t len = read(fd, buf, sizeof(buf));
    if (len <= 0) {
        return 0;
    }
    bool save_seen = false;
    for (const char *p = buf; p < buf + len;) {
        const struct inotify_event *ev =
            reinterpret_cast<const struct inotify_event *>(p);
        std::string name = ev->len > 0 ? ev->name : "";
        if (aquawm::configwatch_should_reload(ev->mask, name,
                server->config_watch_name)) {
            save_seen = true;
        }
        p += sizeof(struct inotify_event) + ev->len;
    }
    if (save_seen && server->config_timer != nullptr) {
        wl_event_source_timer_update(server->config_timer,
            aquawm::CONFIG_RELOAD_DEBOUNCE_MS);
    }
    return 0;
}

// Watch the config's parent directory (covers in-place rewrites and atomic
// save-as-rename alike); failures degrade to manual reload only.
void setup_config_watch(Server *server, struct wl_event_loop *loop) {
    namespace fs = std::filesystem;
    fs::path cfg(server->config_path);
    fs::path dir = cfg.parent_path();
    if (dir.empty()) {
        return;
    }
    server->config_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (server->config_inotify_fd < 0) {
        wlr_log(WLR_ERROR, "config watch: inotify unavailable, "
                "reload on save disabled (SIGHUP still works)");
        return;
    }
    if (inotify_add_watch(server->config_inotify_fd, dir.c_str(),
            IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
        wlr_log(WLR_ERROR, "config watch: cannot watch %s, "
                "reload on save disabled (SIGHUP still works)",
            dir.c_str());
        close(server->config_inotify_fd);
        server->config_inotify_fd = -1;
        return;
    }
    server->config_watch_name = cfg.filename().string();
    if (wl_event_loop_add_fd(loop, server->config_inotify_fd, WL_EVENT_READABLE,
            on_config_inotify, server) == nullptr) {
        wlr_log(WLR_ERROR, "config watch: event source failed, "
                "reload on save disabled");
        close(server->config_inotify_fd);
        server->config_inotify_fd = -1;
        return;
    }
    server->config_timer = wl_event_loop_add_timer(loop, on_config_debounce,
        server);
    if (server->config_timer == nullptr) {
        wlr_log(WLR_ERROR, "config watch: timer failed, "
                "reload on save disabled");
        return;
    }
    wlr_log(WLR_INFO, "config watch: reloading %s on save",
        server->config_path.c_str());
}

void on_keyboard_key(struct wl_listener *listener, void *data) {
    Keyboard *kb = wl_container_of(listener, kb, key);
    Server *server = kb->server;
    if (server->backend_gone) {
        return; // tearing down: the seat may be half-destroyed
    }
    struct wlr_keyboard_key_event *event =
        static_cast<struct wlr_keyboard_key_event *>(data);

    const uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms = nullptr;
    int nsyms = 0;
    if (kb->kbd->xkb_state != nullptr) {
        nsyms = xkb_state_key_get_syms(kb->kbd->xkb_state, keycode, &syms);
    } else if (!kb->warned_no_state) {
        kb->warned_no_state = true;
        wlr_log(WLR_ERROR, "keyboard has no xkb state; forwarding raw keycodes only");
    }

    bool handled = false;
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        const uint32_t modifiers = wlr_keyboard_get_modifiers(kb->kbd);
        for (int i = 0; i < nsyms && !handled; ++i) {
            handled = handle_keybinding(server, syms[i], modifiers);
        }
    }
    if (!handled) {
        wlr_seat_set_keyboard(server->seat, kb->kbd);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode,
            event->state);
    }
}

void on_keyboard_modifiers(struct wl_listener *listener, void * /*data*/) {
    Keyboard *kb = wl_container_of(listener, kb, modifiers);
    if (kb->server->backend_gone) {
        return;
    }
    wlr_seat_set_keyboard(kb->server->seat, kb->kbd);
    wlr_seat_keyboard_notify_modifiers(kb->server->seat, &kb->kbd->modifiers);
}

void on_keyboard_destroy(struct wl_listener *listener, void * /*data*/) {
    Keyboard *kb = wl_container_of(listener, kb, destroy);
    wl_list_remove(&kb->key.link);
    wl_list_remove(&kb->modifiers.link);
    wl_list_remove(&kb->destroy.link);
    delete kb;
}

void setup_keyboard(Server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *kbd = wlr_keyboard_from_input_device(device);
    wlr_log(WLR_INFO, "new keyboard: %s", device->name != nullptr ? device->name : "(unnamed)");

    // A keyboard without a working keymap still forwards raw keycodes;
    // only the compositor-side keysym matching degrades. Every failure
    // here is loud because silent dead keys are worse than no keyboard.
    struct xkb_rule_names rules{};
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = nullptr;
    if (context == nullptr) {
        wlr_log(WLR_ERROR, "keyboard: cannot create xkb context; keys will not translate");
    } else {
        keymap = xkb_keymap_new_from_names(context, &rules,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (keymap == nullptr) {
            wlr_log(WLR_ERROR,
                "keyboard: cannot compile keymap (missing xkeyboard-config data? check XKB_CONFIG_ROOT); keys will not translate");
        } else {
            wlr_keyboard_set_keymap(kbd, keymap);
            if (kbd->xkb_state == nullptr) {
                wlr_log(WLR_ERROR,
                    "keyboard: keymap set but xkb state is missing; keys will not translate");
            }
            xkb_keymap_unref(keymap);
        }
        xkb_context_unref(context);
    }
    wlr_keyboard_set_repeat_info(kbd, 25, 600);

    Keyboard *kb = new Keyboard();
    kb->server = server;
    kb->kbd = kbd;
    kb->device = device;
    kb->key.notify = on_keyboard_key;
    wl_signal_add(&kbd->events.key, &kb->key);
    kb->modifiers.notify = on_keyboard_modifiers;
    wl_signal_add(&kbd->events.modifiers, &kb->modifiers);
    kb->destroy.notify = on_keyboard_destroy;
    wl_signal_add(&device->events.destroy, &kb->destroy);

    wlr_seat_set_keyboard(server->seat, kbd);
}

void set_default_cursor(Server *server) {
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
    server->cursor_is_default = true;
}

// Shared tail of both motion handlers: drive an active drag, otherwise
// forward to the seat and restore the default cursor off-client.
void cursor_process_position(Server *server, uint32_t time_msec) {
    if (server->backend_gone) {
        return; // tearing down: the seat may be half-destroyed
    }
    if (server->cursor_mode != CursorMode::Passthrough && server->has_grab) {
        const AnyView t = server->grabbed_tile;
        const int dx =
            static_cast<int>(server->cursor->x - server->grab_lx);
        const int dy =
            static_cast<int>(server->cursor->y - server->grab_ly);
        if (server->cursor_mode == CursorMode::Move) {
            any_set_pos(t, server->grab_vx + dx, server->grab_vy + dy);
        } else {
            int w = server->grab_vw + dx;
            int h = server->grab_vh + dy;
            if (w < 100) {
                w = 100;
            }
            if (h < 100) {
                h = 100;
            }
            any_commit_size(t, w, h);
        }
        return;
    }
    // Focus-follows-mouse: enter the topmost tile under the cursor (with
    // surface-local coordinates) so motion, buttons, axis and client-side
    // decorations all reach the right client; leave when over nothing.
    // Without this, clients never gain pointer focus and clicks, scrolling
    // and CSD buttons silently go nowhere.
    AnyView hit{};
    struct wlr_surface *surface = nullptr;
    double sx = 0.0, sy = 0.0;
    tile_at(server, server->cursor->x, server->cursor->y, hit, &surface, &sx,
        &sy);
    if (surface != nullptr) {
        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
    } else {
        if (server->seat->pointer_state.focused_surface != nullptr) {
            wlr_seat_pointer_notify_clear_focus(server->seat);
        }
        wlr_seat_pointer_notify_motion(server->seat, time_msec,
            server->cursor->x, server->cursor->y);
    }
    if (server->seat->pointer_state.focused_surface == nullptr &&
        !server->cursor_is_default) {
        set_default_cursor(server);
    }
}

void begin_grab(Server *server, const AnyView &t, CursorMode mode, uint32_t button) {
    server->grab_from_tiled = !any_floating(t);
    if (server->grab_from_tiled) {
        // Dragging floats the window first so the tiling layout reflows
        // around the gap it leaves behind.
        any_set_floating(t, true);
        arrange(server);
    }
    any_raise_to_top(server, t);
    server->grabbed_tile = t;
    server->has_grab = true;
    server->grab_button = button;
    server->cursor_mode = mode;
    server->grab_lx = server->cursor->x;
    server->grab_ly = server->cursor->y;
    struct wlr_box box = any_box(t);
    server->grab_vx = box.x;
    server->grab_vy = box.y;
    server->grab_vw = box.width > 0 ? box.width : 640;
    server->grab_vh = box.height > 0 ? box.height : 480;
}

void on_cursor_motion(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, motion);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_motion_event *>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    cursor_process_position(server, event->time_msec);
}

void on_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, motion_absolute);
    Server *server = ce->server;
    auto *event = static_cast<struct wlr_pointer_motion_absolute_event *>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    cursor_process_position(server, event->time_msec);
}

void on_cursor_button(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, button);
    Server *server = ce->server;
    if (server->backend_gone) {
        return; // tearing down: the seat may be half-destroyed
    }
    auto *event = static_cast<struct wlr_pointer_button_event *>(data);
    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button,
        event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        AnyView hit{};
        struct wlr_surface *surface = nullptr;
        double sx = 0.0, sy = 0.0;
        if (tile_at(server, server->cursor->x, server->cursor->y, hit,
                &surface, &sx, &sy)) {
            struct wlr_box box = any_box(hit);
            const char *name = "?";
            if (hit.v != nullptr && hit.v->toplevel->app_id != nullptr) {
                name = hit.v->toplevel->app_id;
            } else if (hit.x != nullptr && hit.x->xsurface->title != nullptr) {
                name = hit.x->xsurface->title;
            }
            wlr_log(WLR_INFO,
                "pointer press: cursor=(%.0f,%.0f) hit=%s box=(%d,%d %dx%d) local=(%.0f,%.0f)",
                server->cursor->x, server->cursor->y, name, box.x, box.y,
                box.width, box.height, sx, sy);
            focus_any(server, hit);
        } else {
            wlr_log(WLR_INFO, "pointer press: cursor=(%.0f,%.0f) hit=none",
                server->cursor->x, server->cursor->y);
            focus_any(server, AnyView{});
        }
        struct wlr_keyboard *kbd = wlr_seat_get_keyboard(server->seat);
        const uint32_t mods =
            kbd != nullptr ? wlr_keyboard_get_modifiers(kbd) : 0;
        if (any_valid(hit) && (mods & WLR_MODIFIER_LOGO) != 0) {
            if (event->button == BTN_LEFT) {
                begin_grab(server, hit, CursorMode::Move, event->button);
            } else if (event->button == BTN_RIGHT) {
                begin_grab(server, hit, CursorMode::Resize, event->button);
            }
        }
    } else if (server->has_grab && (event->button == server->grab_button ||
                   server->grab_button == 0)) {
        // Drop: a move grab that started in tiling docks the window back
        // so the layout reflows around it; anything else stays floating.
        // (grab_button 0 comes from client-initiated X11 move/resize
        // requests, which carry no button to match on release.)
        if (server->cursor_mode == CursorMode::Move && server->grab_from_tiled) {
            any_set_floating(server->grabbed_tile, false);
            arrange(server);
        }
        server->cursor_mode = CursorMode::Passthrough;
        server->grabbed_tile = AnyView{};
        server->has_grab = false;
        server->grab_from_tiled = false;
        server->grab_button = 0;
    }
}

void on_cursor_axis(struct wl_listener *listener, void *data) {
    CursorEvents *ce = wl_container_of(listener, ce, axis);
    Server *server = ce->server;
    if (server->backend_gone) {
        return;
    }
    auto *event = static_cast<struct wlr_pointer_axis_event *>(data);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation,
        event->delta, event->delta_discrete, event->source,
        event->relative_direction);
}

void on_cursor_frame(struct wl_listener *listener, void * /*data*/) {
    CursorEvents *ce = wl_container_of(listener, ce, frame);
    if (ce->server->backend_gone) {
        return;
    }
    wlr_seat_pointer_notify_frame(ce->server->seat);
}

void on_request_cursor(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, request_cursor);
    auto *event = static_cast<struct wlr_seat_pointer_request_set_cursor_event *>(data);
    if (event->seat_client == server->seat->pointer_state.focused_client) {
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x,
            event->hotspot_y);
        server->cursor_is_default = false;
    }
}

void on_new_input(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = static_cast<struct wlr_input_device *>(data);
    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        setup_keyboard(server, device);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(server->cursor, device);
    }
}

// If the backend dies (e.g. the host disconnects our nested window),
// detach every backend listener, then leave the event loop so main() can
// tear down in order. Detaching all three matters: wlr_backend_finish
// asserts the destroy/new_input/new_output listener lists are all empty,
// so leaving new_input/new_output attached aborts (SIGABRT) instead of
// exiting cleanly. main() teardown skips them via backend_gone.
void on_backend_destroy(struct wl_listener *listener, void * /*data*/) {
    Server *server = wl_container_of(listener, server, backend_destroy);
    wl_list_remove(&server->new_input.link);
    wl_list_remove(&server->new_output.link);
    wl_list_remove(&listener->link);
    server->backend_gone = true;
    wlr_log(WLR_ERROR, "backend destroyed; shutting down");
    wl_display_terminate(server->display);
}

// CPU-backed wallpaper buffer (last-resort upload path): a malloc'd XRGB
// image exposed read-only via data_ptr. The GLES2 renderer uploads such
// buffers with a plain CPU copy, which works on every driver — including
// ones whose GBM buffers are neither mappable nor renderable (external-
// only). Same pattern as wlroots' internal readonly_data_buffer.
struct CpuImageBuffer {
    struct wlr_buffer base;
    uint8_t *pixels = nullptr; // XRGB8888, stride = width * 4, owned here
};

void cpu_image_buffer_destroy(struct wlr_buffer *wlr_buffer) {
    struct CpuImageBuffer *self =
        wl_container_of(wlr_buffer, self, base);
    wlr_buffer_finish(wlr_buffer);
    free(self->pixels);
    free(self);
}

bool cpu_image_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
    uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    struct CpuImageBuffer *self =
        wl_container_of(wlr_buffer, self, base);
    if ((flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) != 0) {
        return false; // immutable after creation; renderers only need READ
    }
    *data = self->pixels;
    *format = DRM_FORMAT_XRGB8888;
    *stride = static_cast<size_t>(wlr_buffer->width) * 4;
    return true;
}

void cpu_image_buffer_end_data_ptr_access(struct wlr_buffer * /*wlr_buffer*/) {
    // No-op: nothing to flush for plain malloc'd memory.
}

const struct wlr_buffer_impl cpu_image_buffer_impl = {
    cpu_image_buffer_destroy,
    nullptr, // get_dmabuf: CPU-only, no dmabuf
    nullptr, // get_shm: CPU-only, no shm fd
    cpu_image_buffer_begin_data_ptr_access,
    cpu_image_buffer_end_data_ptr_access,
};

// GPU-side wallpaper upload for buffers that refuse CPU mapping (typical
// for GBM/dmabuf on real hardware): push pixels into a texture, then blit
// it into the destination buffer with a throwaway render pass.
bool blit_wallpaper_gpu(Server *server, const uint8_t *rgba, int iw, int ih,
    struct wlr_buffer *buf) {
    struct wlr_texture *tex = wlr_texture_from_pixels(server->renderer,
        DRM_FORMAT_ABGR8888, static_cast<uint32_t>(iw * 4),
        static_cast<uint32_t>(iw), static_cast<uint32_t>(ih), rgba);
    if (tex == nullptr) {
        return false;
    }
    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(server->renderer, buf, nullptr);
    if (pass == nullptr) {
        wlr_texture_destroy(tex);
        return false;
    }
    struct wlr_render_texture_options opts{};
    opts.texture = tex;
    opts.dst_box = {0, 0, iw, ih};
    wlr_render_pass_add_texture(pass, &opts);
    bool ok = wlr_render_pass_submit(pass);
    wlr_texture_destroy(tex);
    return ok;
}

// Fill an allocator buffer with decoded RGBA pixels: a CPU memcpy when the
// buffer is mappable (shm/pixman/dumb allocators), else a GPU blit through
// the renderer (GBM/dmabuf buffers on real hardware generally refuse CPU
// mapping). Returns false when neither path works for this buffer.
bool fill_wallpaper_buffer(Server *server, struct wlr_buffer *buf,
    const uint8_t *rgba, int iw, int ih) {
    void *data = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    bool mapped = wlr_buffer_begin_data_ptr_access(buf,
        WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride);
    if (mapped && format != DRM_FORMAT_XRGB8888) {
        wlr_buffer_end_data_ptr_access(buf);
        mapped = false;
    }
    if (mapped) {
        auto *px = static_cast<uint8_t *>(data);
        for (int y = 0; y < ih; ++y) {
            uint8_t *row = px + static_cast<std::size_t>(y) * stride;
            const uint8_t *src = rgba + static_cast<std::size_t>(y) * iw * 4;
            for (int x = 0; x < iw; ++x) {
                row[4 * x + 0] = src[4 * x + 2];
                row[4 * x + 1] = src[4 * x + 1];
                row[4 * x + 2] = src[4 * x + 0];
                row[4 * x + 3] = 0xFF;
            }
        }
        wlr_buffer_end_data_ptr_access(buf);
        return true;
    }
    return blit_wallpaper_gpu(server, rgba, iw, ih, buf);
}

// Decode the configured wallpaper and upload it once into a shared XRGB
// allocator buffer. Remembers the last attempted path so a missing file
// costs one open() instead of one per frame.
//
// Modifier candidates, in order: LINEAR first (mappable on shm/dumb/Intel
// GBM, so the previous behavior is unchanged there), then INVALID, which
// makes GBM fall back to driver-default implicit allocation. Some drivers
// (notably Nvidia proprietary) hand out LINEAR buffers that are neither
// CPU-mappable nor renderable (external-only); the implicit buffer is
// renderable, so the GPU blit path succeeds there. If both fail, a
// malloc'd CPU buffer is used (always renderer-uploadable, never
// scanout-able).
// Anything else degrades to no background instead of crashing.
bool upload_wallpaper(Server *server) {
    if (server->wallpaper.buffer != nullptr) {
        return true;
    }
    std::string path = aquawm::resolve_wallpaper_path(server->config.wallpaper);
    if (path == server->wallpaper.tried_path) {
        return false;
    }
    server->wallpaper.tried_path = path;

    std::vector<uint8_t> rgba;
    int iw = 0, ih = 0;
    std::string error;
    if (!aquawm::decode_image(path.c_str(), rgba, iw, ih, error)) {
        wlr_log(WLR_ERROR, "wallpaper: %s", error.c_str());
        return false;
    }
    static uint64_t mods_linear[] = {DRM_FORMAT_MOD_LINEAR};
    static uint64_t mods_implicit[] = {DRM_FORMAT_MOD_INVALID};
    static struct wlr_drm_format fmts[] = {
        {DRM_FORMAT_XRGB8888, 1, 1, mods_linear},
        {DRM_FORMAT_XRGB8888, 1, 1, mods_implicit},
    };
    struct wlr_buffer *buf = nullptr;
    for (std::size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); ++i) {
        struct wlr_buffer *candidate = wlr_allocator_create_buffer(
            server->allocator, iw, ih, &fmts[i]);
        if (candidate == nullptr) {
            continue;
        }
        if (fill_wallpaper_buffer(server, candidate, rgba.data(), iw, ih)) {
            buf = candidate;
            break;
        }
        wlr_buffer_drop(candidate);
    }
    if (buf == nullptr) {
        // Last resort: our own malloc'd buffer (always uploadable, never
        // scanout-able). Converts RGBA decode output to XRGB in place.
        struct CpuImageBuffer *cpu =
            static_cast<struct CpuImageBuffer *>(calloc(1, sizeof(*cpu)));
        const std::size_t stride = static_cast<std::size_t>(iw) * 4;
        uint8_t *pixels = cpu != nullptr
            ? static_cast<uint8_t *>(malloc(stride * static_cast<std::size_t>(ih)))
            : nullptr;
        if (pixels == nullptr) {
            free(cpu);
            wlr_log(WLR_ERROR,
                "wallpaper: no usable %dx%d buffer (tried linear + implicit + cpu); skipping background",
                iw, ih);
            return false;
        }
        for (int y = 0; y < ih; ++y) {
            uint8_t *row = pixels + static_cast<std::size_t>(y) * stride;
            const uint8_t *src =
                rgba.data() + static_cast<std::size_t>(y) * stride;
            for (int x = 0; x < iw; ++x) {
                row[4 * x + 0] = src[4 * x + 2];
                row[4 * x + 1] = src[4 * x + 1];
                row[4 * x + 2] = src[4 * x + 0];
                row[4 * x + 3] = 0xFF;
            }
        }
        cpu->pixels = pixels;
        wlr_buffer_init(&cpu->base, &cpu_image_buffer_impl, iw, ih);
        buf = &cpu->base;
        wlr_log(WLR_INFO, "wallpaper: using CPU fallback buffer (%dx%d)", iw, ih);
    }
    server->wallpaper.buffer = buf;
    server->wallpaper.img_w = iw;
    server->wallpaper.img_h = ih;
    wlr_log(WLR_INFO, "wallpaper: %dx%d from %s", iw, ih, path.c_str());
    return true;
}

void drop_wallpaper(Server *server) {
    for (Output *o : server->outputs) {
        if (o->bg != nullptr) {
            wlr_scene_node_destroy(&o->bg->node);
            o->bg = nullptr;
            o->bg_w = o->bg_h = -1;
        }
    }
    if (server->wallpaper.buffer != nullptr) {
        wlr_buffer_drop(server->wallpaper.buffer);
        server->wallpaper.buffer = nullptr;
    }
    server->wallpaper.img_w = server->wallpaper.img_h = 0;
    server->wallpaper.tried_path.clear();
}

// Keep a cover-fit wallpaper behind everything on this output. Cheap
// per-frame checks; the image uploads once and node creation happens once.
void ensure_wallpaper(Server *server, Output *output) {
    if (!upload_wallpaper(server)) {
        return;
    }
    if (output->bg == nullptr) {
        output->bg = wlr_scene_buffer_create(&server->scene->tree,
            server->wallpaper.buffer);
        if (output->bg == nullptr) {
            return;
        }
    }
    // Always stay behind views, even ones mapped before we existed.
    wlr_scene_node_lower_to_bottom(&output->bg->node);
    int ow = 0, oh = 0;
    wlr_output_effective_resolution(output->wlr_output, &ow, &oh);
    if (ow == output->bg_w && oh == output->bg_h) {
        return;
    }
    const int iw = server->wallpaper.img_w;
    const int ih = server->wallpaper.img_h;
    const float scale =
        std::max(static_cast<float>(ow) / iw, static_cast<float>(oh) / ih);
    const int dw = static_cast<int>(iw * scale + 0.5f);
    const int dh = static_cast<int>(ih * scale + 0.5f);
    struct wlr_box obox{};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &obox);
    wlr_scene_node_set_position(&output->bg->node, obox.x + (ow - dw) / 2,
        obox.y + (oh - dh) / 2);
    wlr_scene_buffer_set_dest_size(output->bg, dw, dh);
    output->bg_w = ow;
    output->bg_h = oh;
}

// --- Fallback warning bar ------------------------------------------------
// Shown while running on built-in defaults (missing/broken config) unless
// autogenerated_warn is false: an amber strip with the config path,
// reserving space so tiling never covers it.

bool warn_bar_active(Server *server) {
    return server->config_fallback && server->config.autogenerated_warn;
}

// Rasterize message centered into an ow x WARN_BAR_HEIGHT XRGB image.
// Amber background, black antialiased text. Falls back to a solid strip
// when no font loads (still visible, still reserves space).
uint8_t *render_warnbar_pixels(Server *server, int ow, const std::string &message) {
    using aquawm::WARN_BAR_HEIGHT;
    const std::size_t stride = static_cast<std::size_t>(ow) * 4;
    uint8_t *px = static_cast<uint8_t *>(calloc(1, stride * WARN_BAR_HEIGHT));
    if (px == nullptr) {
        return nullptr;
    }
    // Amber background, fully opaque.
    for (int y = 0; y < WARN_BAR_HEIGHT; ++y) {
        uint8_t *row = px + static_cast<std::size_t>(y) * stride;
        for (int x = 0; x < ow; ++x) {
            row[4 * x + 0] = 0xFF;
            row[4 * x + 1] = 0xA8;
            row[4 * x + 2] = 0x00;
            row[4 * x + 3] = 0xFF;
        }
    }
    if (!server->ft_ready) {
        server->ft_ready = FT_Init_FreeType(&server->ft_lib) == 0;
        if (!server->ft_ready) {
            wlr_log(WLR_ERROR, "warnbar: freetype init failed; solid strip only");
            return px;
        }
    }
    static const char *const font_dirs[] = {
        nullptr, // $HOME/.fonts
        "/run/current-system/sw/share/fonts",
        "/usr/share/fonts",
    };
    static const char *const font_files[] = {
        "DejaVuSansMono.ttf",
        "DejaVuSans.ttf",
        "NotoSansMono-Regular.ttf",
        "NotoSans-Regular.ttf",
    };
    // Prefer fontconfig (works everywhere, including NixOS per-package
    // font dirs), then fall back to well-known directories.
    std::string fc_path;
    {
        FcConfig *fc = FcInitLoadConfigAndFonts();
        if (fc != nullptr) {
            FcPattern *pat = FcNameParse(
                reinterpret_cast<const FcChar8 *>("monospace"));
            if (pat != nullptr) {
                FcConfigSubstitute(fc, pat, FcMatchPattern);
                FcDefaultSubstitute(pat);
                FcResult res = FcResultNoMatch;
                FcPattern *match =
                    FcFontMatch(fc, pat, &res);
                if (match != nullptr) {
                    FcChar8 *file = nullptr;
                    if (FcPatternGetString(match, FC_FILE, 0, &file) ==
                        FcResultMatch) {
                        fc_path = reinterpret_cast<const char *>(file);
                    }
                    FcPatternDestroy(match);
                }
                FcPatternDestroy(pat);
            }
        }
    }
    FT_Face face = nullptr;
    if (!fc_path.empty() &&
        FT_New_Face(server->ft_lib, fc_path.c_str(), 0, &face) != 0) {
        wlr_log(WLR_DEBUG, "warnbar: fontconfig match %s unusable",
            fc_path.c_str());
        face = nullptr;
    }
    const char *home = std::getenv("HOME");
    // Well-known directories as fallback when fontconfig finds nothing.
    if (face == nullptr) {
    for (const char *dir : font_dirs) {
        for (const char *file : font_files) {
            std::string path = dir != nullptr
                ? std::string(dir) + "/" + file
                : (home != nullptr ? std::string(home) + "/.fonts/" + file : "");
            if (path.empty()) {
                continue;
            }
            if (FT_New_Face(server->ft_lib, path.c_str(), 0, &face) == 0) {
                break;
            }
            face = nullptr;
        }
        if (face != nullptr) {
            break;
        }
    }
    }
    if (face == nullptr) {
        wlr_log(WLR_ERROR, "warnbar: no usable font found; solid strip only");
        return px;
    }
    // DejaVu/Noto ship weights as separate files; accept whatever matched.
    constexpr int font_px = 13;
    if (FT_Set_Pixel_Sizes(face, 0, font_px) != 0) {
        FT_Done_Face(face);
        return px;
    }
    // Measure first so the text centers.
    int text_w = 0;
    for (char ch : message) {
        if (FT_Load_Char(face, static_cast<unsigned char>(ch),
                FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            continue;
        }
        text_w += face->glyph->advance.x >> 6;
    }
    int pen_x = aquawm::warn_text_x(ow, text_w);
    const int baseline =
        (WARN_BAR_HEIGHT + (face->size->metrics.ascender - face->size->metrics.descender) / 64) / 2;
    for (char ch : message) {
        if (FT_Load_Char(face, static_cast<unsigned char>(ch),
                FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
            pen_x += font_px / 2; // missing glyph: skip ahead, keep going
            continue;
        }
        FT_GlyphSlot g = face->glyph;
        const int gx = pen_x + g->bitmap_left;
        const int gy = baseline - g->bitmap_top;
        for (unsigned row = 0; row < g->bitmap.rows; ++row) {
            const int y = gy + static_cast<int>(row);
            if (y < 0 || y >= WARN_BAR_HEIGHT) {
                continue;
            }
            uint8_t *dst = px + static_cast<std::size_t>(y) * stride;
            for (unsigned col = 0; col < g->bitmap.width; ++col) {
                const int x = gx + static_cast<int>(col);
                if (x < 0 || x >= ow) {
                    continue;
                }
                const uint8_t a = g->bitmap.buffer[row * g->bitmap.pitch + col];
                uint8_t *p = dst + static_cast<std::size_t>(x) * 4;
                p[0] = static_cast<uint8_t>((p[0] * (255 - a)) / 255);
                p[1] = static_cast<uint8_t>((p[1] * (255 - a)) / 255);
                p[2] = static_cast<uint8_t>((p[2] * (255 - a)) / 255);
            }
        }
        pen_x += g->advance.x >> 6;
    }
    FT_Done_Face(face);
    return px;
}

void drop_warnbars(Server *server) {
    for (Output *o : server->outputs) {
        if (o->warn != nullptr) {
            wlr_scene_node_destroy(&o->warn->node);
            o->warn = nullptr;
        }
        if (o->warn_buf != nullptr) {
            wlr_buffer_drop(o->warn_buf);
            o->warn_buf = nullptr;
        }
        o->warn_w = -1;
    }
}

// Keep a fallback notice behind top-layer clients but above tiling views.
// Cheap per-frame checks; the strip uploads once per output width.
void ensure_warnbar(Server *server, Output *output) {
    const bool want = warn_bar_active(server);
    if (!want) {
        if (output->warn != nullptr) {
            wlr_scene_node_destroy(&output->warn->node);
            output->warn = nullptr;
        }
        if (output->warn_buf != nullptr) {
            wlr_buffer_drop(output->warn_buf);
            output->warn_buf = nullptr;
        }
        output->warn_w = -1;
        return;
    }
    int ow = 0, oh = 0;
    wlr_output_effective_resolution(output->wlr_output, &ow, &oh);
    (void)oh;
    if (ow <= 0) {
        return;
    }
    if (output->warn_buf == nullptr || output->warn_w != ow) {
        std::string message =
            aquawm::warn_bar_message(server->config_path);
        uint8_t *px = render_warnbar_pixels(server, ow, message);
        if (px == nullptr) {
            return;
        }
        struct CpuImageBuffer *cpu =
            static_cast<struct CpuImageBuffer *>(calloc(1, sizeof(*cpu)));
        if (cpu == nullptr) {
            free(px);
            return;
        }
        cpu->pixels = px;
        wlr_buffer_init(&cpu->base, &cpu_image_buffer_impl, ow,
            aquawm::WARN_BAR_HEIGHT);
        if (output->warn_buf != nullptr) {
            wlr_buffer_drop(output->warn_buf);
        }
        output->warn_buf = &cpu->base;
        output->warn_w = ow;
        if (output->warn == nullptr) {
            output->warn = wlr_scene_buffer_create(
                server->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP],
                output->warn_buf);
        } else {
            wlr_scene_buffer_set_buffer(output->warn, output->warn_buf);
        }
        if (output->warn == nullptr) {
            wlr_buffer_drop(output->warn_buf);
            output->warn_buf = nullptr;
            output->warn_w = -1;
            return;
        }
        wlr_log(WLR_INFO, "warnbar: showing fallback notice (%dx%d)", ow,
            aquawm::WARN_BAR_HEIGHT);
    }
    struct wlr_box obox{};
    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &obox);
    wlr_scene_node_set_position(&output->warn->node, obox.x, obox.y);
    wlr_scene_buffer_set_dest_size(output->warn, ow, aquawm::WARN_BAR_HEIGHT);
}

void on_output_frame(struct wl_listener *listener, void * /*data*/) {
    Output *output = wl_container_of(listener, output, frame);
    Server *server = output->server;

    ensure_wallpaper(server, output);
    ensure_warnbar(server, output);

    struct wlr_scene_output *scene_output =
        wlr_scene_get_scene_output(server->scene, output->wlr_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (!wlr_scene_output_build_state(scene_output, &state, nullptr)) {
        wlr_output_state_finish(&state);
        return;
    }
    if (!wlr_output_commit_state(output->wlr_output, &state)) {
        wlr_log(WLR_ERROR, "output commit failed");
    } else {
        // Frame pacing: without this, clients never receive wl_surface.frame
        // callbacks and stall after their initial burst (~2s: kitty's cursor,
        // Firefox's animations, everything). The scene only builds state;
        // delivery is the compositor's job.
        struct timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        wlr_scene_output_send_frame_done(scene_output, &now);
    }
    wlr_output_state_finish(&state);
}

void on_output_destroy(struct wl_listener *listener, void * /*data*/) {
    Output *output = wl_container_of(listener, output, destroy);
    Server *server = output->server;
    if (output->bg != nullptr) {
        wlr_scene_node_destroy(&output->bg->node);
        output->bg = nullptr;
    }
    if (output->warn != nullptr) {
        wlr_scene_node_destroy(&output->warn->node);
        output->warn = nullptr;
    }
    if (output->warn_buf != nullptr) {
        wlr_buffer_drop(output->warn_buf);
        output->warn_buf = nullptr;
    }
    output->warn_w = -1;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    auto it = std::find(server->outputs.begin(), server->outputs.end(), output);
    if (it != server->outputs.end()) {
        server->outputs.erase(it);
    }
    // Keep layer surfaces pinned to a live output (or none) so arrange
    // never dereferences the dying one.
    struct wlr_output *fallback =
        server->outputs.empty() ? nullptr : server->outputs.front()->wlr_output;
    for (LayerSurface *ls : server->layers) {
        if (ls->layer->output == output->wlr_output) {
            ls->layer->output = fallback;
        }
    }
    delete output;
    arrange_layers(server);
}

void on_new_output(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = static_cast<struct wlr_output *>(data);

    // Since wlroots 0.19 outputs must be explicitly bound to the renderer
    // and allocator before anything (including the software cursor) can
    // submit buffers to them.
    if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
        wlr_log(WLR_ERROR, "failed to init output rendering");
        return;
    }

    // Give the host-side window (the nested output under WSLg/X11) real
    // metadata. Without a title/app_id, WSLg's RAIL shell only gets a bare
    // entry that shows on the taskbar but won't restore or focus properly.
    if (wlr_output_is_wl(wlr_output)) {
        wlr_wl_output_set_title(wlr_output, "aquawm");
        wlr_wl_output_set_app_id(wlr_output, "aquawm");
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode != nullptr) {
        wlr_output_state_set_mode(&state, mode);
    }
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "initial output commit failed");
    }
    wlr_output_state_finish(&state);

    wlr_output_layout_add_auto(server->output_layout, wlr_output);

    Output *output = new Output();
    output->server = server;
    output->wlr_output = wlr_output;
    output->frame.notify = on_output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = on_output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);
    server->outputs.push_back(output);

    wlr_scene_output_create(server->scene, wlr_output);
    set_default_cursor(server);
    // Full box until arrange_layers accounts for bars; adopt layer
    // surfaces that arrived before any output existed.
    wlr_output_layout_get_box(server->output_layout, wlr_output, &output->usable_area);
    output->usable_valid = true;
    for (LayerSurface *ls : server->layers) {
        if (ls->layer->output == nullptr) {
            ls->layer->output = wlr_output;
        }
    }
    arrange_layers(server);
}

} // namespace

int main(int argc, char **argv) {
    wlr_log_init(WLR_DEBUG, nullptr);

    Server server{};
    server.config = aquawm::default_config();
    std::string explicit_path = argc > 1 ? argv[1] : "";
    server.config_path = aquawm::resolve_config_path(explicit_path);
    if (explicit_path.empty() &&
        server.config_path == aquawm::legacy_config_path()) {
        wlr_log(WLR_INFO, "using legacy config %s; move it to %s",
            server.config_path.c_str(),
            aquawm::default_config_path().c_str());
    }
    {
        std::string error;
        if (aquawm::load_config_file(server.config_path.c_str(), server.config,
                error)) {
            wlr_log(WLR_INFO, "loaded config %s", server.config_path.c_str());
            server.config_fallback = false;
        } else {
            wlr_log(WLR_ERROR, "using built-in defaults (%s: %s)",
                server.config_path.c_str(), error.c_str());
            server.config_fallback = true;
        }
        wlr_log(WLR_INFO, "config: gaps=%d mfact=%.2f nmaster=%d workspaces=%d "
                "binds=%zu",
            server.config.gaps, static_cast<double>(server.config.mfact),
            server.config.nmaster, server.config.workspaces,
            server.config.keys.size());
    }
    server.display = wl_display_create();
    assert(server.display && "wl_display_create failed");

    struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
    // SIGHUP reloads the config file without restarting the compositor
    // (Super+Shift+R does the same from the keyboard).
    wl_event_loop_add_signal(loop, SIGHUP, on_reload_signal, &server);
    // Saving aquawm.lua reloads it automatically (Hyprland-style);
    // SIGHUP stays as the manual fallback.
    setup_config_watch(&server, loop);
    server.backend = wlr_backend_autocreate(loop, &server.session);
    if (server.backend == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create backend\n");
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create renderer\n");
        return 1;
    }
    if (!wlr_renderer_init_wl_display(server.renderer, server.display)) {
        std::fprintf(stderr, "aquawm: failed to init renderer display\n");
        return 1;
    }

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create allocator\n");
        return 1;
    }

    server.compositor = wlr_compositor_create(server.display, 6, server.renderer);
    if (server.compositor == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create compositor\n");
        return 1;
    }
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    // XWayland (Phase 3c, lazy): no X server spawns until an X11 client
    // actually connects. The wlr_compositor is required for XWM startup.
    server.xwayland = wlr_xwayland_create(server.display, server.compositor, true);
    if (server.xwayland == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create Xwayland\n");
        return 1;
    }
    server.new_xwayland_surface.notify = on_new_xwayland_surface;
    wl_signal_add(&server.xwayland->events.new_surface,
        &server.new_xwayland_surface);

    server.output_layout = wlr_output_layout_create(server.display);
    server.scene = wlr_scene_create();
    server.scene_layout =
        wlr_scene_attach_output_layout(server.scene, server.output_layout);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
    server.new_toplevel.notify = on_new_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_toplevel);

    // Output descriptions for bar clients (waybar requires xdg-output).
    // Tracks the output layout automatically; no listeners needed.
    server.xdg_output_manager =
        wlr_xdg_output_manager_v1_create(server.display, server.output_layout);
    if (server.xdg_output_manager == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create xdg-output manager\n");
        return 1;
    }

    // Layer-shell bars (Phase 3b, protocol v5 = vendored XML): one scene
    // tree per protocol layer, created bottom-up so render order is
    // background < bottom < views < top < overlay. Tiling views attach to
    // view_tree, created between bottom and top.
    server.layer_shell = wlr_layer_shell_v1_create(server.display, 5);
    if (server.layer_shell == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create layer shell\n");
        return 1;
    }
    for (int layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
         layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; ++layer) {
        // Tiling views render between the bottom and top layers: create
        // view_tree after the bottom tree, before the top tree.
        if (layer == ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
            server.view_tree = wlr_scene_tree_create(&server.scene->tree);
            if (server.view_tree == nullptr) {
                std::fprintf(stderr, "aquawm: failed to create view tree\n");
                return 1;
            }
        }
        server.layer_trees[layer] = wlr_scene_tree_create(&server.scene->tree);
        if (server.layer_trees[layer] == nullptr) {
            std::fprintf(stderr, "aquawm: failed to create layer tree %d\n", layer);
            return 1;
        }
    }
    server.new_layer_surface.notify = on_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_mgr = wlr_xcursor_manager_create(nullptr, 24);
    // A visible pointer from the start: client cursors take over on focus
    // via request_set_cursor, and cursor_process_position restores this
    // whenever the pointer rests on no client surface. Needs an xcursor
    // theme installed (e.g. Adwaita) or nothing shows.
    set_default_cursor(&server);

    server.cursor_events.server = &server;
    server.cursor_events.motion.notify = on_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_events.motion);
    server.cursor_events.motion_absolute.notify = on_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute,
        &server.cursor_events.motion_absolute);
    server.cursor_events.button.notify = on_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_events.button);
    server.cursor_events.axis.notify = on_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_events.axis);
    server.cursor_events.frame.notify = on_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_events.frame);

    server.seat = wlr_seat_create(server.display, "seat0");
    server.request_cursor.notify = on_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    // Advertise input capabilities up front. Without this the seat reports
    // zero caps: clients can neither create keyboard/pointer objects nor
    // receive input (kitty won't even map its window, foot looks frozen).
    // Devices themselves attach/detach via new_input; the caps describe
    // what this seat offers.
    wlr_seat_set_capabilities(server.seat,
        WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
    if (server.xwayland != nullptr) {
        wlr_xwayland_set_seat(server.xwayland, server.seat);
    }

    server.new_input.notify = on_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.new_output.notify = on_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);
    server.backend_destroy.notify = on_backend_destroy;
    wl_signal_add(&server.backend->events.destroy, &server.backend_destroy);

    server.socket = wl_display_add_socket_auto(server.display);
    if (server.socket == nullptr) {
        std::fprintf(stderr, "aquawm: failed to create Wayland socket\n");
        return 1;
    }
    // Spawned clients (Super+Return terminal, XWayland) inherit our
    // environment: point them at our socket, not at whatever display we
    // were started from (matters nested; bare metal is usually wayland-0).
    setenv("WAYLAND_DISPLAY", server.socket, 1);
    std::fprintf(stderr, "aquawm: running on WAYLAND_DISPLAY=%s\n", server.socket);

    if (!wlr_backend_start(server.backend)) {
        std::fprintf(stderr, "aquawm: failed to start backend\n");
        return 1;
    }

    wl_display_run(server.display);

    // Tear down in order: detach our listeners first so backend/display
    // destruction never trips wlroots' listener-list assertions.
    // (On a backend-initiated shutdown on_backend_destroy already detached
    // the backend listeners and set backend_gone; removing them twice
    // would corrupt the list, so skip them here in that case.)
    if (!server.backend_gone) {
        wl_list_remove(&server.new_output.link);
        wl_list_remove(&server.new_input.link);
        wl_list_remove(&server.backend_destroy.link);
    }
    wl_list_remove(&server.new_toplevel.link);
    wl_list_remove(&server.new_layer_surface.link);
    wl_list_remove(&server.new_xwayland_surface.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.cursor_events.motion.link);
    wl_list_remove(&server.cursor_events.motion_absolute.link);
    wl_list_remove(&server.cursor_events.button.link);
    wl_list_remove(&server.cursor_events.axis.link);
    wl_list_remove(&server.cursor_events.frame.link);

    // Event-loop sources die with the loop; close our own inotify fd.
    if (server.config_inotify_fd >= 0) {
        close(server.config_inotify_fd);
        server.config_inotify_fd = -1;
    }

    wl_display_destroy_clients(server.display);
    wl_display_destroy(server.display);
    return 0;
}
