/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

// Row thumbnails for scrolling lists. Each row owns a thumbnail-sized RGB565
// buffer. The media service loads covers off the GUI thread, so binding a row
// only copies a ready image, or leaves the placeholder showing until
// thumbnail_generation() changes and the list refreshes its rows.

#include "../lyra_gui_internal.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace lyra::gui::internal {
namespace {

constexpr int kThumbRadius = 4;
constexpr size_t kMaxRequests = 32;

struct RowThumbView {
    lv_image_dsc_t descriptor;
    lyra::media::ThumbnailSource source;
    size_t index;
    lyra::media::ThumbnailState state;
    uint16_t pixels[lyra::media::kThumbnailPixels];
};

void make_unclickable_tree(lv_obj_t *object)
{
    // Touches must reach the virtual-list slot, which forwards them to the row.
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    const uint32_t children = lv_obj_get_child_count(object);
    for (uint32_t i = 0; i < children; ++i) make_unclickable_tree(lv_obj_get_child(object, i));
}

void release_view_cb(lv_event_t *event)
{
    auto *view = static_cast<RowThumbView *>(lv_event_get_user_data(event));
    lv_image_cache_drop(&view->descriptor);
    heap_caps_free(view);
}

} // namespace

lv_obj_t *make_row_thumbnail(lv_obj_t *row, int x, int y)
{
    constexpr int kSize = kRowThumbnailSize;
    lv_obj_t *art = make_box(row, x, y, kSize, kSize, kArtworkSurface, kThumbRadius);
    make_artwork_placeholder(art, kSize, kSize);
    auto *view = static_cast<RowThumbView *>(heap_caps_calloc(
        1, sizeof(RowThumbView), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    lv_obj_t *image = nullptr;
    if (view) {
        view->index = SIZE_MAX;
        view->state = lyra::media::ThumbnailState::Pending;
        view->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        view->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        view->descriptor.header.w = kSize;
        view->descriptor.header.h = kSize;
        view->descriptor.header.stride = kSize * sizeof(uint16_t);
        view->descriptor.data_size = sizeof(view->pixels);
        view->descriptor.data = reinterpret_cast<const uint8_t *>(view->pixels);
        image = lv_image_create(art);
        lv_obj_set_pos(image, 0, 0);
        lv_obj_set_size(image, kSize, kSize);
        lv_obj_set_style_radius(image, s_neon_hud ? 0 : kThumbRadius, 0);
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(image, view);
        lv_obj_add_event_cb(image, release_view_cb, LV_EVENT_DELETE, view);
    }
    make_unclickable_tree(art);
    return image;
}

void bind_row_thumbnail(lv_obj_t *image, lyra::media::ThumbnailSource source, size_t index)
{
    if (!image) return;
    auto *view = static_cast<RowThumbView *>(lv_obj_get_user_data(image));
    const bool same = view->source == source && view->index == index;
    if (same && view->state != lyra::media::ThumbnailState::Pending) return;
    const lyra::media::ThumbnailState state = lyra::media::thumbnail(source, index, view->pixels);
    if (same && state == view->state) return;
    view->source = source;
    view->index = index;
    view->state = state;
    if (state == lyra::media::ThumbnailState::Ready) {
        // Same descriptor, new pixels: drop any cached decode before redrawing.
        lv_image_cache_drop(&view->descriptor);
        lv_image_set_src(image, &view->descriptor);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
}

void request_row_thumbnails(lyra::media::ThumbnailSource source, const size_t *indices,
                            size_t count)
{
    lyra::media::ThumbnailRequest requests[kMaxRequests];
    count = std::min(count, kMaxRequests);
    for (size_t i = 0; i < count; ++i) requests[i] = {source, indices[i]};
    lyra::media::request_thumbnails(requests, count);
}

void enable_row_thumbnails(VirtualListSpec &spec, VirtualWindow window, VirtualRefresh refresh)
{
    if (!s_list_artwork) return;
    spec.window = window;
    spec.refresh_stamp = lyra::media::thumbnail_generation;
    spec.refresh = refresh;
    spec.release = lyra::media::release_thumbnails;
}

} // namespace lyra::gui::internal
