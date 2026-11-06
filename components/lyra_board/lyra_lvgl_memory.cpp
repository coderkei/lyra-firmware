/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * LVGL heap for the GUI (CONFIG_LV_USE_CUSTOM_MALLOC).
 *
 * With the C library allocator, ESP-IDF serves every request up to
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL from internal RAM, so each object,
 * style, label string and per-frame draw task competed with audio, Wi-Fi and
 * the display's DMA bands for internal memory. A lyrics page alone holds
 * hundreds of labels. Keep LVGL's heap in PSRAM, where the 64 KiB data cache
 * holds the live widget tree, and fall back to internal RAM only when PSRAM
 * is exhausted. Nothing LVGL allocates here is used for DMA.
 */

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "esp_heap_caps.h"

namespace {

constexpr uint32_t kPreferredCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t kFallbackCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

} // namespace

extern "C" {

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return nullptr;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *memory = heap_caps_malloc(size, kPreferredCaps);
    return memory ? memory : heap_caps_malloc(size, kFallbackCaps);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    // A failed realloc leaves the original block untouched, so the internal
    // retry still owns valid data.
    void *memory = heap_caps_realloc(p, new_size, kPreferredCaps);
    return memory ? memory : heap_caps_realloc(p, new_size, kFallbackCaps);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info{};
    heap_caps_get_info(&info, kPreferredCaps);
    const size_t total = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->total_size = total;
    mon_p->free_cnt = info.free_blocks;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->max_used = total - info.minimum_free_bytes;
    mon_p->used_pct = total ? static_cast<uint8_t>(100 - (100 * info.total_free_bytes) / total) : 0;
    mon_p->frag_pct = info.total_free_bytes ?
        static_cast<uint8_t>(100 - (100 * info.largest_free_block) / info.total_free_bytes) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(kPreferredCaps, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

} // extern "C"

#endif
