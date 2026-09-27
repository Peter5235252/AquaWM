#include "tiling.hpp"

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

static int area_of(Box b) {
    return b.w * b.h;
}

static int total_area(const std::vector<Box> &v) {
    int sum = 0;
    for (Box b : v) {
        sum += area_of(b);
    }
    return sum;
}

static bool inside(Box inner, Box outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

int main() {
    const Box area{0, 0, 1000, 800};

    // Empty / degenerate inputs produce no boxes, everywhere.
    CHECK(aquawm::dwindle(0, area).empty());
    CHECK(aquawm::dwindle(3, Box{0, 0, 0, 600}).empty());
    CHECK(aquawm::grid(0, area).empty());
    CHECK(aquawm::monocle(0, area).empty());

    // Dwindle: wide area splits vertically first at ratio.
    {
        auto v = aquawm::dwindle(2, area, 0.5f);
        CHECK(v.size() == 2);
        CHECK(v[0].x == 0 && v[0].y == 0 && v[0].w == 500 && v[0].h == 800);
        CHECK(v[1].x == 500 && v[1].y == 0 && v[1].w == 500 && v[1].h == 800);
        CHECK(total_area(v) == 1000 * 800);
    }

    // Dwindle: tall area splits horizontally first; oldest stays biggest.
    {
        auto v = aquawm::dwindle(3, Box{0, 0, 600, 900}, 0.5f);
        CHECK(v.size() == 3);
        CHECK(v[0].x == 0 && v[0].y == 0 && v[0].w == 600 && v[0].h == 450);
        // Remainder (600x450) is wide: side-by-side split.
        CHECK(v[1].x == 0 && v[1].y == 450 && v[1].w == 300 && v[1].h == 450);
        CHECK(v[2].x == 300 && v[2].y == 450 && v[2].w == 300 && v[2].h == 450);
        CHECK(total_area(v) == 600 * 900);
        for (Box b : v) {
            CHECK(inside(b, Box{0, 0, 600, 900}));
        }
    }

    // Dwindle: ratio clamps, single window takes all, area conserved.
    {
        auto v = aquawm::dwindle(1, area, 0.3f);
        CHECK(v.size() == 1);
        CHECK(v[0].x == 0 && v[0].y == 0 && v[0].w == 1000 && v[0].h == 800);
        auto w = aquawm::dwindle(4, area, 0.25f);
        CHECK(w.size() == 4);
        CHECK(total_area(w) == 1000 * 800);
        auto c = aquawm::dwindle(4, area, 99.0f); // clamped to 0.9
        CHECK(total_area(c) == 1000 * 800);
    }

    // Dwindle: absurd window counts terminate with no degenerate boxes.
    // (Total area is NOT conserved here by design: once the region is
    // down to a line, leftovers overlap instead of vanishing.)
    {
        auto v = aquawm::dwindle(40, Box{0, 0, 100, 60}, 0.5f);
        CHECK(v.size() == 40);
        for (Box b : v) {
            CHECK(b.w >= 1 && b.h >= 1);
            CHECK(inside(b, Box{0, 0, 100, 60}));
        }
        auto tiny = aquawm::dwindle(10, Box{5, 5, 3, 2}, 0.5f);
        CHECK(tiny.size() == 10);
        for (Box b : tiny) {
            CHECK(b.w >= 1 && b.h >= 1);
        }
    }

    // Grid: 4 windows on square-ish area -> 2x2, no pixel loss.
    {
        auto v = aquawm::grid(4, Box{0, 0, 1000, 800});
        CHECK(v.size() == 4);
        CHECK(total_area(v) == 1000 * 800);
        for (Box b : v) {
            CHECK(inside(b, Box{0, 0, 1000, 800}));
        }
    }

    // Grid: 3 windows in 900x600 -> 3 columns, one row.
    {
        auto v = aquawm::grid(3, Box{0, 0, 900, 600});
        CHECK(v.size() == 3);
        CHECK(total_area(v) == 900 * 600);
        CHECK(v[0].w == 300 && v[0].h == 600);
        CHECK(v[2].x == 600 && v[2].w == 300);
    }

    // Grid: partial last row stretches across the width.
    {
        auto v = aquawm::grid(5, Box{0, 0, 900, 600});
        CHECK(v.size() == 5);
        CHECK(total_area(v) == 900 * 600);
        // Row 0: three 300x300 cells; row 1: two stretched 450x300 cells.
        CHECK(v[0].w == 300 && v[0].h == 300);
        CHECK(v[3].x == 0 && v[3].y == 300 && v[3].w == 450 && v[3].h == 300);
        CHECK(v[4].x == 450 && v[4].y == 300 && v[4].w == 450 && v[4].h == 300);
    }

    // Grid: single window takes all; offset areas respected.
    {
        auto v = aquawm::grid(1, Box{100, 50, 800, 600});
        CHECK(v.size() == 1);
        CHECK(v[0].x == 100 && v[0].y == 50 && v[0].w == 800 && v[0].h == 600);
    }

    // Monocle: every window takes the whole area.
    {
        auto v = aquawm::monocle(3, area);
        CHECK(v.size() == 3);
        for (Box b : v) {
            CHECK(b.x == 0 && b.y == 0 && b.w == 1000 && b.h == 800);
        }
    }

    if (failures == 0) {
        std::puts("test_layouts: all checks passed");
        return 0;
    }
    std::printf("test_layouts: %d check(s) failed\n", failures);
    return 1;
}
