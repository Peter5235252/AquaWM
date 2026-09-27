#pragma once

// Pure exclusive-zone geometry for layer-shell bars (Phase 3b): no wlroots
// types here on purpose, so this header stays unit-testable on any machine.
//
// A layer surface with exclusive_zone > 0 reserves a strip on the edge it
// is anchored to; tiling must use what remains (the "usable area"). This
// mirrors wlr_layer_surface_v1_get_exclusive_edge +
// layer_surface_exclusive_zone from wlroots 0.20 (exact anchor combos,
// edge margin folded into the reserve), minus the exclusive_edge override,
// which only the compositor side tracks.

#include <cstdint>

#include "tiling.hpp"

namespace aquawm {

// Anchor bits (identical values to WLR_EDGE_* and the layer-shell anchor).
constexpr uint32_t ANCHOR_TOP = 1u << 0;
constexpr uint32_t ANCHOR_BOTTOM = 1u << 1;
constexpr uint32_t ANCHOR_LEFT = 1u << 2;
constexpr uint32_t ANCHOR_RIGHT = 1u << 3;

struct Margins {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;
};

// Shrink `usable` by `exclusive_zone` (+ the edge's margin) on the anchored
// edge. zone <= 0 reserves nothing (0 = none, -1 = overlay). Only exact
// edge combos reserve: a single edge, or an edge spanning the full
// perpendicular (e.g. TOP|LEFT|RIGHT for a top bar). Anything else (corner
// anchored, opposing edges, unanchored) spans or floats, so it owns no
// edge and the area is unchanged. Width/height clamp at zero.
inline Box apply_exclusive_zone(Box usable, uint32_t anchor, int exclusive_zone,
    Margins margins = {}) {
    if (exclusive_zone <= 0) {
        return usable;
    }
    switch (anchor) {
    case ANCHOR_TOP:
    case ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_TOP:
        usable.y += exclusive_zone + margins.top;
        usable.h -= exclusive_zone + margins.top;
        break;
    case ANCHOR_BOTTOM:
    case ANCHOR_LEFT | ANCHOR_RIGHT | ANCHOR_BOTTOM:
        usable.h -= exclusive_zone + margins.bottom;
        break;
    case ANCHOR_LEFT:
    case ANCHOR_TOP | ANCHOR_BOTTOM | ANCHOR_LEFT:
        usable.x += exclusive_zone + margins.left;
        usable.w -= exclusive_zone + margins.left;
        break;
    case ANCHOR_RIGHT:
    case ANCHOR_TOP | ANCHOR_BOTTOM | ANCHOR_RIGHT:
        usable.w -= exclusive_zone + margins.right;
        break;
    default:
        break;
    }
    if (usable.w < 0) {
        usable.w = 0;
    }
    if (usable.h < 0) {
        usable.h = 0;
    }
    return usable;
}

} // namespace aquawm
