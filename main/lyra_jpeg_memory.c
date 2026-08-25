/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "jerror.h"
#include "jpeglib.h"
#include "lyra_sd.h"

#define LYRA_JPEG_TEMP_NAME_LENGTH 64
#define LYRA_JPEG_PSRAM_RESERVE (512u * 1024u)

typedef struct lyra_jpeg_backing_store *lyra_jpeg_backing_store_ptr;
typedef struct lyra_jpeg_backing_store {
    void (*read_backing_store)(j_common_ptr, lyra_jpeg_backing_store_ptr,
                               void *, long, long);
    void (*write_backing_store)(j_common_ptr, lyra_jpeg_backing_store_ptr,
                                void *, long, long);
    void (*close_backing_store)(j_common_ptr, lyra_jpeg_backing_store_ptr);
    FILE *temp_file;
    char temp_name[LYRA_JPEG_TEMP_NAME_LENGTH];
} lyra_jpeg_backing_store;

static uint32_t s_temp_sequence;
static j_common_ptr s_tracked_decoder;
static uint32_t *s_tracked_sd_flag;
static uint32_t s_tracked_sd_allowed;

void lyra_jpeg_begin_backing_store_tracking(j_common_ptr cinfo, uint32_t *flag,
                                             uint32_t allow_sd);
void lyra_jpeg_end_backing_store_tracking(j_common_ptr cinfo);

void lyra_jpeg_begin_backing_store_tracking(j_common_ptr cinfo, uint32_t *flag,
                                             uint32_t allow_sd)
{
    if (flag) *flag = 0;
    __atomic_store_n(&s_tracked_sd_flag, flag, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tracked_sd_allowed, allow_sd, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tracked_decoder, cinfo, __ATOMIC_RELEASE);
}

void lyra_jpeg_end_backing_store_tracking(j_common_ptr cinfo)
{
    if (__atomic_load_n(&s_tracked_decoder, __ATOMIC_ACQUIRE) != cinfo) return;
    __atomic_store_n(&s_tracked_decoder, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s_tracked_sd_flag, NULL, __ATOMIC_RELAXED);
    __atomic_store_n(&s_tracked_sd_allowed, 0, __ATOMIC_RELAXED);
}

void *__wrap_jpeg_get_large(j_common_ptr cinfo, size_t size)
{
    (void)cinfo;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void __wrap_jpeg_free_large(j_common_ptr cinfo, void *object, size_t size)
{
    (void)cinfo;
    (void)size;
    heap_caps_free(object);
}

size_t __wrap_jpeg_mem_available(j_common_ptr cinfo, size_t minimum,
                                 size_t maximum, size_t allocated)
{
    (void)minimum;
    const size_t largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t safe = largest > LYRA_JPEG_PSRAM_RESERVE
        ? largest - LYRA_JPEG_PSRAM_RESERVE : 0;

    /* Keep the whole virtual array in PSRAM whenever it fits safely. */
    if (maximum <= safe) return maximum;

    /* Otherwise retain a bounded active window and spill only the oversized
     * virtual array to transient SD working storage. */
    const size_t configured = cinfo->mem->max_memory_to_use > 0
        ? (size_t)cinfo->mem->max_memory_to_use : 512u * 1024u;
    const size_t budget = configured < safe ? configured : safe;
    const size_t available = budget > allocated ? budget - allocated : 0;
    return available < maximum ? available : maximum;
}

static void backing_read(j_common_ptr cinfo, lyra_jpeg_backing_store_ptr info,
                         void *buffer, long offset, long count)
{
    if (!info || !info->temp_file || offset < 0 || count < 0 ||
        lyra_sd_artwork_seek(info->temp_file, offset, SEEK_SET) != 0)
        ERREXIT(cinfo, JERR_TFILE_SEEK);
    if (lyra_sd_artwork_read(info->temp_file, buffer, (size_t)count) != (size_t)count)
        ERREXIT(cinfo, JERR_TFILE_READ);
}

static void backing_write(j_common_ptr cinfo, lyra_jpeg_backing_store_ptr info,
                          void *buffer, long offset, long count)
{
    if (!info || !info->temp_file || offset < 0 || count < 0 ||
        lyra_sd_artwork_seek(info->temp_file, offset, SEEK_SET) != 0)
        ERREXIT(cinfo, JERR_TFILE_SEEK);
    if (lyra_sd_artwork_write(info->temp_file, buffer, (size_t)count) != (size_t)count)
        ERREXIT(cinfo, JERR_TFILE_WRITE);
}

static void backing_close(j_common_ptr cinfo, lyra_jpeg_backing_store_ptr info)
{
    (void)cinfo;
    if (!info) return;
    if (info->temp_file) lyra_sd_artwork_close(info->temp_file);
    info->temp_file = NULL;
    if (info->temp_name[0]) lyra_sd_artwork_remove(info->temp_name);
    info->temp_name[0] = '\0';
}

void __wrap_jpeg_open_backing_store(j_common_ptr cinfo, void *opaque_info,
                                    long total_bytes)
{
    lyra_jpeg_backing_store_ptr info = (lyra_jpeg_backing_store_ptr)opaque_info;
    if (!info || total_bytes <= 0 || total_bytes > 64L * 1024L * 1024L)
        ERREXIT(cinfo, JERR_NO_BACKING_STORE);

    /* Mark only the tracked artwork decoder. LVGL may use the same wrapped
     * libjpeg allocator, so client_data cannot safely be repurposed globally. */
    if (__atomic_load_n(&s_tracked_decoder, __ATOMIC_ACQUIRE) == cinfo) {
        if (!__atomic_load_n(&s_tracked_sd_allowed, __ATOMIC_RELAXED))
            ERREXIT(cinfo, JERR_NO_BACKING_STORE);
        uint32_t *flag = __atomic_load_n(&s_tracked_sd_flag, __ATOMIC_RELAXED);
        if (flag) *flag = 1;
    }

    const uint32_t sequence = __atomic_fetch_add(&s_temp_sequence, 1, __ATOMIC_RELAXED);
    snprintf(info->temp_name, sizeof(info->temp_name),
             "/sdcard/.lyra/jpeg-%08lx.tmp", (unsigned long)sequence);
    info->temp_file = lyra_sd_artwork_open(info->temp_name, "w+b");
    if (!info->temp_file) ERREXITS(cinfo, JERR_TFILE_CREATE, info->temp_name);
    if (setvbuf(info->temp_file, NULL, _IONBF, 0) != 0) {
        lyra_sd_artwork_close(info->temp_file);
        info->temp_file = NULL;
        lyra_sd_artwork_remove(info->temp_name);
        ERREXITS(cinfo, JERR_TFILE_CREATE, info->temp_name);
    }
    info->read_backing_store = backing_read;
    info->write_backing_store = backing_write;
    info->close_backing_store = backing_close;
}
