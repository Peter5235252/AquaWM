#pragma once

// Pure tiling geometry for aquawm: no wlroots types here on purpose,
// so this header stays unit-testable on any machine.
//
// master-stack layout: the first `nmaster` windows share the left
// "master" column (stacked vertically), the rest share the right
// "stack" column (also stacked vertically). `mfact` controls the
// width fraction of the master column when both columns are occupied.
//
// dwindle layout (Hyprland-inspired): every window splits the remaining
// area in two, side-by-side when the region is wider than tall and
// stacked otherwise, at `ratio`. The oldest window keeps the biggest
// piece; newer windows nest smaller. Deterministic and stateless: the
// same window list always yields the same boxes.
//
// grid layout: rows x columns of equal cells covering the area.
//
// monocle layout: every window takes the whole area (stacked; the topmost
// in focus order shows).

#include <vector>

namespace aquawm {

struct Box {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

inline std::vector<Box> master_stack(int count, Box area, int nmaster = 1, float mfact = 0.55f) {
    std::vector<Box> out;
    if (count <= 0 || area.w <= 0 || area.h <= 0) {
        return out;
    }
    if (nmaster < 1) {
        nmaster = 1;
    }
    if (mfact <= 0.05f) {
        mfact = 0.05f;
    }
    if (mfact >= 0.95f) {
        mfact = 0.95f;
    }

    const int nmaster_clamped = count < nmaster ? count : nmaster;
    const int nstack = count - nmaster_clamped;

    int master_w = area.w;
    int stack_x = area.x;
    if (nstack > 0) {
        master_w = static_cast<int>(area.w * mfact);
        stack_x = area.x + master_w;
    }
    const int stack_w = area.w - master_w;

    out.reserve(static_cast<std::size_t>(count));

    // Master column: split vertically among nmaster_clamped windows,
    // handing remainder pixels to the first windows so nothing is lost.
    for (int i = 0; i < nmaster_clamped; ++i) {
        const int h = area.h / nmaster_clamped;
        const int extra = (i < area.h % nmaster_clamped) ? 1 : 0;
        const int y = area.y + i * h + (i < area.h % nmaster_clamped ? i : area.h % nmaster_clamped);
        out.push_back(Box{area.x, y, master_w, h + extra});
    }

    // Stack column: same vertical split for the remaining windows.
    for (int i = 0; i < nstack; ++i) {
        const int h = area.h / nstack;
        const int y = area.y + i * h + (i < area.h % nstack ? i : area.h % nstack);
        const int extra = (i < area.h % nstack) ? 1 : 0;
        out.push_back(Box{stack_x, y, stack_w, h + extra});
    }

    return out;
}

inline float clamp_ratio(float ratio) {
    if (ratio < 0.1f) {
        return 0.1f;
    }
    if (ratio > 0.9f) {
        return 0.9f;
    }
    return ratio;
}

inline std::vector<Box> dwindle(int count, Box area, float ratio = 0.5f) {
    std::vector<Box> out;
    if (count <= 0 || area.w <= 0 || area.h <= 0) {
        return out;
    }
    const float r = clamp_ratio(ratio);
    out.reserve(static_cast<std::size_t>(count));
    Box rest = area;
    for (int i = 0; i < count; ++i) {
        if (i == count - 1) {
            out.push_back(rest);
            break;
        }
        // Split along the long axis (Hyprland-style dynamic split).
        // The cut is clamped so both halves keep at least one pixel;
        // when the region is down to a line, everything left overlaps
        // instead of looping forever.
        if (rest.w >= rest.h && rest.w > 1) {
            int cut = static_cast<int>(rest.w * r);
            if (cut < 1) {
                cut = 1;
            }
            if (cut > rest.w - 1) {
                cut = rest.w - 1;
            }
            out.push_back(Box{rest.x, rest.y, cut, rest.h});
            rest.x += cut;
            rest.w -= cut;
        } else if (rest.h > 1) {
            int cut = static_cast<int>(rest.h * r);
            if (cut < 1) {
                cut = 1;
            }
            if (cut > rest.h - 1) {
                cut = rest.h - 1;
            }
            out.push_back(Box{rest.x, rest.y, rest.w, cut});
            rest.y += cut;
            rest.h -= cut;
        } else {
            out.push_back(rest);
        }
    }
    return out;
}

inline std::vector<Box> grid(int count, Box area) {
    std::vector<Box> out;
    if (count <= 0 || area.w <= 0 || area.h <= 0) {
        return out;
    }
    int cols = 1;
    while (cols * cols * area.h < count * area.w) {
        ++cols;
    }
    const int rows = (count + cols - 1) / cols;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int cx = i % cols;
        const int cy = i / cols;
        // Last row may be partial: stretch its cells across the width.
        const int row_count =
            (cy == rows - 1) ? (count - cy * cols) : cols;
        const int cw = area.w / row_count;
        const int extra_w = (cx < area.w % row_count) ? 1 : 0;
        const int rh = area.h / rows;
        const int extra_h = (cy < area.h % rows) ? 1 : 0;
        const int x = area.x + cx * cw + (cx < area.w % row_count ? cx : area.w % row_count);
        const int y = area.y + cy * rh + (cy < area.h % rows ? cy : area.h % rows);
        out.push_back(Box{x, y, cw + extra_w, rh + extra_h});
    }
    return out;
}

inline std::vector<Box> monocle(int count, Box area) {
    std::vector<Box> out;
    if (count <= 0 || area.w <= 0 || area.h <= 0) {
        return out;
    }
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out.push_back(area);
    }
    return out;
}

} // namespace aquawm
