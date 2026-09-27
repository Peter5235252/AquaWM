#include "layers.hpp"

#include <cstdio>
#include <cstdlib>

using aquawm::Box;

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static bool same(Box a, Box b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

int main() {
    const Box full{0, 0, 1920, 1080};

    // No zone, or overlay zone (-1): usable area untouched.
    CHECK(same(aquawm::apply_exclusive_zone(full, aquawm::ANCHOR_TOP, 0), full));
    CHECK(same(aquawm::apply_exclusive_zone(full, aquawm::ANCHOR_TOP, -1), full));

    // Top bar (the waybar case): anchored top, spans full width.
    {
        Box u = aquawm::apply_exclusive_zone(full,
            aquawm::ANCHOR_TOP | aquawm::ANCHOR_LEFT | aquawm::ANCHOR_RIGHT, 30);
        CHECK(u.x == 0 && u.y == 30 && u.w == 1920 && u.h == 1050);
    }

    // Bottom bar, single-edge anchor.
    {
        Box u = aquawm::apply_exclusive_zone(full, aquawm::ANCHOR_BOTTOM, 40);
        CHECK(u.x == 0 && u.y == 0 && u.w == 1920 && u.h == 1040);
    }

    // Left panel spanning full height.
    {
        Box u = aquawm::apply_exclusive_zone(full,
            aquawm::ANCHOR_LEFT | aquawm::ANCHOR_TOP | aquawm::ANCHOR_BOTTOM, 64);
        CHECK(u.x == 64 && u.y == 0 && u.w == 1856 && u.h == 1080);
    }

    // Right panel, single-edge anchor.
    {
        Box u = aquawm::apply_exclusive_zone(full, aquawm::ANCHOR_RIGHT, 200);
        CHECK(u.x == 0 && u.y == 0 && u.w == 1720 && u.h == 1080);
    }

    // Stacked bars compose: top 30 then bottom 40.
    {
        Box u = aquawm::apply_exclusive_zone(full,
            aquawm::ANCHOR_TOP | aquawm::ANCHOR_LEFT | aquawm::ANCHOR_RIGHT, 30);
        u = aquawm::apply_exclusive_zone(u, aquawm::ANCHOR_BOTTOM, 40);
        CHECK(u.x == 0 && u.y == 30 && u.w == 1920 && u.h == 1010);
    }

    // Offset outputs are respected.
    {
        Box u = aquawm::apply_exclusive_zone(Box{1920, 0, 1920, 1080},
            aquawm::ANCHOR_TOP | aquawm::ANCHOR_LEFT | aquawm::ANCHOR_RIGHT, 30);
        CHECK(u.x == 1920 && u.y == 30 && u.w == 1920 && u.h == 1050);
    }

    // Centered dialog (no anchor): reserves nothing.
    {
        Box u = aquawm::apply_exclusive_zone(full, 0, 100);
        CHECK(same(u, full));
    }

    // Full-screen overlay (all edges): reserves nothing.
    {
        Box u = aquawm::apply_exclusive_zone(full,
            aquawm::ANCHOR_TOP | aquawm::ANCHOR_BOTTOM | aquawm::ANCHOR_LEFT |
                aquawm::ANCHOR_RIGHT,
            50);
        CHECK(same(u, full));
    }

    // Opposing edges (stretched, not attached): reserves nothing.
    {
        Box u = aquawm::apply_exclusive_zone(full,
            aquawm::ANCHOR_TOP | aquawm::ANCHOR_BOTTOM, 50);
        CHECK(same(u, full));
    }

    // Corner-anchored widget (top-left, not full span): reserves nothing,
    // matching wlroots (only exact edge combos own an edge).
    {
        Box u = aquawm::apply_exclusive_zone(full,
            aquawm::ANCHOR_TOP | aquawm::ANCHOR_LEFT, 50);
        CHECK(same(u, full));
    }

    // The edge's margin folds into the reserve, like wlroots.
    {
        Box u = aquawm::apply_exclusive_zone(full,
            aquawm::ANCHOR_TOP | aquawm::ANCHOR_LEFT | aquawm::ANCHOR_RIGHT, 30,
            aquawm::Margins{.top = 4});
        CHECK(u.x == 0 && u.y == 34 && u.w == 1920 && u.h == 1046);
    }

    // Absurd zone never inverts the area.
    {
        Box u = aquawm::apply_exclusive_zone(full, aquawm::ANCHOR_TOP, 5000);
        CHECK(u.y == 5000 && u.h == 0 && u.w == 1920);
    }

    if (failures == 0) {
        std::puts("test_layers: all checks passed");
        return 0;
    }
    std::printf("test_layers: %d check(s) failed\n", failures);
    return 1;
}
