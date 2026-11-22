/* SPDX-License-Identifier: Apache-2.0 */
// Photos and Ebook: every picture or text file on the card, found by a
// background scan, listed a page at a time and opened in the viewers.
#include "../lyra_gui_internal.h"

#include <atomic>
#include <new>

namespace lyra::gui::internal {
namespace {
constexpr int kAppFilePageSize = 5;

struct FileScan {
    std::atomic<bool> running{false};
    std::atomic<bool> complete{false};
    std::atomic<size_t> found{};
    lyra::media::FolderFile *files{};
    size_t count{};
    bool succeeded{};
    bool refresh_cache{};
};

struct FileScanJob {
    FileScan *scan;
    lyra::media::FileKind kind;
    bool refresh_cache;
    bool psram_stack;
};

FileScan s_photo_scan;
FileScan s_ebook_scan;
lv_timer_t *s_file_scan_timer{};
lv_obj_t *s_file_scan_count{};

void file_scan_progress(void *context, size_t found)
{
    auto *scan = static_cast<FileScan *>(context);
    if (scan) scan->found.store(found, std::memory_order_release);
}

void file_scan_task(void *argument)
{
    auto *job = static_cast<FileScanJob *>(argument);
    lyra::media::FolderFile *files = nullptr;
    size_t count = 0;
    bool succeeded = lyra::media::folder_tree_files(
        "/sdcard", job->kind, &files, &count, file_scan_progress,
        job->scan, job->refresh_cache);
    job->scan->files = files;
    job->scan->count = count;
    job->scan->succeeded = succeeded;
    job->scan->found.store(count, std::memory_order_release);
    job->scan->complete.store(true, std::memory_order_release);
    job->scan->running.store(false, std::memory_order_release);
    const bool psram_stack = job->psram_stack;
    delete job;
    if (psram_stack) vTaskDeleteWithCaps(nullptr);
    else vTaskDelete(nullptr);
}

bool start_file_scan(FileScan *scan, lyra::media::FileKind kind)
{
    if (scan->running.load(std::memory_order_acquire)) return true;
    if (scan->complete.load(std::memory_order_acquire)) return true;
    auto *job = new (std::nothrow) FileScanJob{scan, kind, scan->refresh_cache, false};
    if (!job) {
        scan->succeeded = false;
        scan->complete.store(true, std::memory_order_release);
        return false;
    }
    scan->refresh_cache = false;
    scan->found.store(0, std::memory_order_release);
    scan->running.store(true, std::memory_order_release);
    // The scan only reads the SD card, so a PSRAM stack is safe when the
    // internal heap is too fragmented for 16 KiB; scanning must not fail.
    bool started = xTaskCreate(file_scan_task, "lyra.app.files", 16 * 1024, job, 2, nullptr) == pdPASS;
    if (!started) {
        job->psram_stack = true;
        started = xTaskCreatePinnedToCoreWithCaps(file_scan_task, "lyra.app.files", 16 * 1024, job, 2,
                                                  nullptr, tskNO_AFFINITY,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    if (!started) {
        scan->running.store(false, std::memory_order_release);
        delete job;
        scan->succeeded = false;
        scan->complete.store(true, std::memory_order_release);
        return false;
    }
    return true;
}

void file_scan_poll_cb(lv_timer_t *timer)
{
    if (s_view != View::Photos && s_view != View::Ebook) {
        lv_timer_delete(timer);
        s_file_scan_timer = nullptr;
        return;
    }
    FileScan &scan = s_view == View::Photos ? s_photo_scan : s_ebook_scan;
    if (scan.complete.load(std::memory_order_acquire) &&
        !scan.running.load(std::memory_order_acquire)) {
        lv_timer_delete(timer);
        s_file_scan_timer = nullptr;
        s_file_scan_count = nullptr;
        render(s_view);
        return;
    }
    if (s_file_scan_count) {
        char count_text[32];
        format_u32(lyra::i18n::StringId::AppFilesFound,
            static_cast<uint32_t>(std::min<size_t>(
                scan.found.load(std::memory_order_acquire), UINT32_MAX)),
            count_text, sizeof(count_text));
        set_label_text_if_changed(s_file_scan_count, count_text);
    }
}

void ensure_file_scan_poll()
{
    if (!s_file_scan_timer) s_file_scan_timer = lv_timer_create(file_scan_poll_cb, 150, nullptr);
}

void file_reload_cb(lv_event_t *)
{
    if (s_view != View::Photos && s_view != View::Ebook) return;
    FileScan &scan = s_view == View::Photos ? s_photo_scan : s_ebook_scan;
    if (scan.running.load(std::memory_order_acquire)) return;
    release_app_file_scan(s_view);
    scan.refresh_cache = true;
    s_list_page = 0;
    render(s_view);
}

} // namespace

void release_app_file_scan(View view)
{
    if (view != View::Photos && view != View::Ebook) return;
    FileScan &scan = view == View::Photos ? s_photo_scan : s_ebook_scan;
    if (scan.running.load(std::memory_order_acquire)) return;
    lyra::media::free_folder_tree_files(scan.files);
    scan.files = nullptr;
    scan.count = 0;
    scan.succeeded = false;
    scan.found.store(0, std::memory_order_release);
    scan.refresh_cache = false;
    scan.complete.store(false, std::memory_order_release);
}

void stop_apps_runtime_timers(View next_view)
{
    // The scan poll belongs to the page that started it; a new page that
    // still needs it starts its own.
    (void)next_view;
    if (s_file_scan_timer) lv_timer_delete(s_file_scan_timer);
    s_file_scan_timer = nullptr;
    s_file_scan_count = nullptr;
}

void render_app_files()
{
    const bool photos = s_view == View::Photos;
    FileScan &scan = photos ? s_photo_scan : s_ebook_scan;
    const lyra::media::FileKind kind = photos ? lyra::media::FileKind::Image :
                                                lyra::media::FileKind::Text;
    lv_obj_t *header = make_header(tr(photos ? lyra::i18n::StringId::Photos :
                                               lyra::i18n::StringId::Ebook),
                                   View::Apps, true, nullptr, nullptr, 44);
    lv_obj_t *reload = make_symbol_button(header, 274, 2, 40, LV_SYMBOL_REFRESH, kTextPrimary);
    lv_obj_add_event_cb(reload, file_reload_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *list = make_scroll_body(72);
    if (!scan.complete.load(std::memory_order_acquire)) {
        lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        start_file_scan(&scan, kind);
        ensure_file_scan_poll();
        lv_obj_t *spinner = lv_spinner_create(list);
        lv_obj_set_size(spinner, 48, 48);
        lv_spinner_set_anim_params(spinner, 900, 250);
        lv_obj_set_style_arc_color(spinner, kDivider, LV_PART_MAIN);
        lv_obj_set_style_arc_color(spinner, kAccent, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
        lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 14);
        lv_obj_t *reading = make_label(list,
            tr(lyra::i18n::StringId::ReadingMicroSdFolders), kTextMuted);
        lv_obj_set_width(reading, 300);
        lv_obj_set_style_text_align(reading, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(reading, 10, 76);
        char count_text[32];
        format_u32(lyra::i18n::StringId::AppFilesFound,
            static_cast<uint32_t>(std::min<size_t>(
                scan.found.load(std::memory_order_acquire), UINT32_MAX)),
            count_text, sizeof(count_text));
        s_file_scan_count = make_label(list, count_text, kTextSecondary);
        lv_obj_set_width(s_file_scan_count, 300);
        lv_obj_set_style_text_align(s_file_scan_count, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_file_scan_count, 10, 108);
        return;
    }
    s_list_page_count = 0;
    if (!scan.succeeded) {
        const lyra::i18n::StringId error = lyra::media::status().mounted ?
            lyra::i18n::StringId::AppFileScanFailed : lyra::i18n::StringId::NoMicroSdCard;
        lv_obj_t *message = make_label(list, tr(error), kTextMuted);
        lv_obj_set_pos(message, 12, 18);
        return;
    }
    if (!scan.count) {
        lv_obj_t *empty = make_label(list,
            tr(lyra::i18n::StringId::AppNoSupportedFiles), kTextMuted);
        lv_obj_set_pos(empty, 12, 18);
        return;
    }

    const size_t pages = (scan.count + kAppFilePageSize - 1) / kAppFilePageSize;
    s_list_page_count = pages;
    if (s_list_page >= pages) s_list_page = pages - 1;
    const size_t first = s_list_page * kAppFilePageSize;
    const size_t last = std::min(first + kAppFilePageSize, scan.count);
    size_t row = 0;
    for (size_t index = first; index < last; ++index) {
        make_document_row(list, 6 + static_cast<int>(row++) * 58,
                          scan.files[index], s_view);
    }
    make_page_controls(list, 6 + static_cast<int>(row) * 58 + 3,
                       scan.count, kAppFilePageSize);
}

} // namespace lyra::gui::internal
