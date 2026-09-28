// C bridge for wlr_xwayland_surface fields unusable from C++: `class`
// is a reserved keyword there, so main.cpp cannot name the member.
// Compiled as C (project() enables both languages).

#include <wlr/xwayland.h>

const char *aquawm_xwayland_class(const struct wlr_xwayland_surface *surface) {
    if (surface == NULL) {
        return NULL;
    }
    return surface->class;
}

const char *aquawm_xwayland_title(const struct wlr_xwayland_surface *surface) {
    if (surface == NULL) {
        return NULL;
    }
    return surface->title;
}
