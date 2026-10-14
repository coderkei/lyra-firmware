/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

namespace lyra::gui::internal {

// Reject only tasks whose complete LVGL bounds miss the current redraw clip.
// This runs at task creation, before LVGL allocates/copies the descriptor.
// Partially visible primitives retain their original geometry and rasterizer.
inline bool gui_draw_area_visible(const lv_layer_t *layer, const lv_area_t &area)
{
#if LV_DRAW_TRANSFORM_USE_MATRIX
    // Layer matrices can move local geometry into the clip. Let LVGL handle it.
    return true;
#else
    const lv_area_t &clip = layer->_clip_area;
    return area.x1 <= clip.x2 && area.x2 >= clip.x1 &&
           area.y1 <= clip.y2 && area.y2 >= clip.y1;
#endif
}

inline void gui_draw_line(lv_layer_t *layer, const lv_draw_line_dsc_t *line)
{
    // Match lv_draw_line(), including its conservative stroke/cap margin.
    const lv_area_t area{
        static_cast<int32_t>(LV_MIN(line->p1.x, line->p2.x)) - line->width,
        static_cast<int32_t>(LV_MIN(line->p1.y, line->p2.y)) - line->width,
        static_cast<int32_t>(LV_MAX(line->p1.x, line->p2.x)) + line->width,
        static_cast<int32_t>(LV_MAX(line->p1.y, line->p2.y)) + line->width};
    if (gui_draw_area_visible(layer, area)) lv_draw_line(layer, line);
}

inline void gui_draw_arc(lv_layer_t *layer, const lv_draw_arc_dsc_t *arc)
{
    // Match the full-circle task bounds in lv_draw_arc(), even for partial arcs.
    const lv_area_t area{arc->center.x - arc->radius, arc->center.y - arc->radius,
        arc->center.x + arc->radius - 1, arc->center.y + arc->radius - 1};
    if (gui_draw_area_visible(layer, area)) lv_draw_arc(layer, arc);
}

inline void gui_draw_border(lv_layer_t *layer, const lv_draw_border_dsc_t *border,
                            const lv_area_t *area)
{
    if (gui_draw_area_visible(layer, *area)) lv_draw_border(layer, border, area);
}

} // namespace lyra::gui::internal
