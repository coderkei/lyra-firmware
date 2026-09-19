/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

bool append_debug_text(char *destination, size_t capacity, const char *format, ...)
{
    if (!destination || capacity == 0 || !format) return false;
    const size_t used = std::strlen(destination);
    if (used >= capacity) return false;
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(destination + used, capacity - used, format, arguments);
    va_end(arguments);
    return written >= 0 && static_cast<size_t>(written) < capacity - used;
}

void format_mac_address(const uint8_t mac[6], char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (!mac) {
        copy_ui_text(output, capacity, tr(lyra::i18n::StringId::Unavailable));
        return;
    }
    std::snprintf(output, capacity, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

const char *debug_partition_type(const esp_partition_t *partition)
{
    if (!partition) return tr(lyra::i18n::StringId::Unknown);
    if (partition->type == ESP_PARTITION_TYPE_APP) {
        switch (partition->subtype) {
            case ESP_PARTITION_SUBTYPE_APP_FACTORY: return "app/factory";
            case ESP_PARTITION_SUBTYPE_APP_OTA_0: return "app/ota_0";
            case ESP_PARTITION_SUBTYPE_APP_OTA_1: return "app/ota_1";
            default: return "app";
        }
    }
    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) return "data/nvs";
    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_OTA) return "data/otadata";
    if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS) return "data/littlefs";
    return "data";
}

bool app_partition_image_size(const esp_partition_t *partition, uint32_t *image_size)
{
    if (!partition || !image_size || partition->type != ESP_PARTITION_TYPE_APP) return false;

    // ESP application images use a 24-byte image header and 8-byte segment
    // headers. This is deliberately a small footprint estimate for the
    // diagnostic display; the partition itself remains the authoritative size.
    constexpr uint32_t kImageHeaderSize = 24;
    constexpr uint32_t kSegmentHeaderSize = 8;
    constexpr uint32_t kImageMagic = 0xE9;
    constexpr uint8_t kMaximumSegments = 16;
    uint8_t header[kImageHeaderSize]{};
    if (esp_partition_read(partition, 0, header, sizeof(header)) != ESP_OK ||
        header[0] != kImageMagic || header[1] == 0 || header[1] > kMaximumSegments) {
        return false;
    }

    uint32_t offset = kImageHeaderSize;
    for (uint8_t segment_index = 0; segment_index < header[1]; ++segment_index) {
        uint8_t segment_header[kSegmentHeaderSize]{};
        if (offset > partition->size - kSegmentHeaderSize ||
            esp_partition_read(partition, offset, segment_header, sizeof(segment_header)) != ESP_OK) {
            return false;
        }
        const uint32_t segment_size = static_cast<uint32_t>(segment_header[4]) |
                                      (static_cast<uint32_t>(segment_header[5]) << 8) |
                                      (static_cast<uint32_t>(segment_header[6]) << 16) |
                                      (static_cast<uint32_t>(segment_header[7]) << 24);
        offset += kSegmentHeaderSize;
        if (segment_size == 0 || segment_size % 4u != 0 ||
            segment_size > partition->size - offset) return false;
        offset += segment_size;
    }
    if (offset >= partition->size) return false;
    // The checksum follows the segments; a simple SHA-256 digest follows it
    // when hash_appended is set in the image header.
    const uint32_t appended_hash_size = header[23] != 0 ? 32u : 0u;
    const uint32_t trailer_size = 1u + appended_hash_size;
    if (partition->size < trailer_size || offset > partition->size - trailer_size) return false;
    *image_size = offset + 1u + appended_hash_size;
    return true;
}

void append_debug_partition(char *info, size_t capacity, const esp_partition_t *partition,
                            const esp_partition_t *running)
{
    if (!partition) return;
    char total[24];
    format_file_size(partition->size, total, sizeof(total));
    const bool is_running = partition == running ||
                            (running && partition->address == running->address);
    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugPartitionAddress),
                      partition->label, is_running ? " [BOOTED]" : "",
                      static_cast<unsigned>(partition->address));
    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugPartitionTypeTotal),
                      debug_partition_type(partition), total);

    if (partition->type == ESP_PARTITION_TYPE_APP) {
        uint32_t image_size = 0;
        if (app_partition_image_size(partition, &image_size) && image_size <= partition->size) {
            char used[24];
            char free_space[24];
            format_file_size(image_size, used, sizeof(used));
            format_file_size(partition->size - image_size, free_space, sizeof(free_space));
            append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugImageFree), used, free_space);
        } else {
            append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugImageUnavailable));
        }
    } else if (partition->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS) {
        nvs_stats_t stats{};
        if (nvs_get_stats(partition->label, &stats) == ESP_OK) {
            char free_entries[24];
            format_file_size(static_cast<uint64_t>(stats.free_entries) * 32u,
                             free_entries, sizeof(free_entries));
            append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugFreeNvs),
                              static_cast<unsigned>(stats.free_entries), free_entries);
        } else {
            append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugFreeUnavailable));
        }
    } else {
        append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugFreeRaw));
    }
}

void build_debug_info(char *info, size_t capacity)
{
    if (!info || capacity == 0) return;
    info[0] = '\0';

    const esp_partition_t *running = esp_ota_get_running_partition();
    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugBootPartition));
    if (running) {
        char size[24];
        format_file_size(running->size, size, sizeof(size));
        append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugRunningPartition),
                          running->label, debug_partition_type(running),
                          static_cast<unsigned>(running->address), size);
    } else {
        append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugUnavailableBlock));
    }

    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    uint8_t mac[6]{};
    char mac_text[24];
    format_mac_address(esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) == ESP_OK ? mac : nullptr,
                       mac_text, sizeof(mac_text));
    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugHardwareIdReport),
                      "ESP32-S3", chip.revision, chip.cores);
    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugFactoryMac),
                      mac_text, esp_get_idf_version(), __DATE__, __TIME__);

    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugFlashPartitions));
    esp_partition_iterator_t iterator = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
    while (iterator) {
        const esp_partition_t *partition = esp_partition_get(iterator);
        append_debug_partition(info, capacity, partition, running);
        iterator = esp_partition_next(iterator);
    }
    esp_partition_iterator_release(iterator);

    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t psram_used = psram_total >= psram_free ? psram_total - psram_free : 0;
    char psram_used_text[24];
    char psram_total_text[24];
    format_file_size(psram_used, psram_used_text, sizeof(psram_used_text));
    format_file_size(psram_total, psram_total_text, sizeof(psram_total_text));
    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugPsram),
                      psram_used_text, psram_total_text);

    const lyra::media::Status media_status = lyra::media::status();
    lyra::media::CardInfo card{};
    const bool card_available = lyra::media::card_info(&card);
    append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugMicroSd));
    if (!media_status.mounted || !card_available) {
        append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugNotMounted));
    } else {
        const uint64_t card_capacity = card.capacity_bytes != 0 ? card.capacity_bytes :
                                       media_status.total_bytes;
        char capacity_text[24];
        char free_text[24];
        char total_text[24];
        format_file_size(card_capacity, capacity_text, sizeof(capacity_text));
        format_file_size(media_status.free_bytes, free_text, sizeof(free_text));
        format_file_size(media_status.total_bytes, total_text, sizeof(total_text));
        char card_name[9]{};
        std::memcpy(card_name, card.name, sizeof(card.name));
        append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugCardCapacity),
                          capacity_text, free_text, total_text);
        append_debug_text(info, capacity, tr(lyra::i18n::StringId::DebugCidFields),
                          static_cast<unsigned>(card.manufacturer_id) & 0xFFu,
                          card.manufacturer_id, static_cast<unsigned>(card.oem_id) & 0xFFFFu,
                          card_name, card.revision, card.serial, card.date);
    }
}

bool dump_debug_info_to_sd()
{
    char info[kDebugInfoCapacity];
    build_debug_info(info, sizeof(info));

    char path[64];
    std::snprintf(path, sizeof(path), "/sdcard/lyra-debug-info-%llu.txt",
                  static_cast<unsigned long long>(esp_timer_get_time()));
    FILE *file = lyra::sd::open(path, "wb", lyra::sd::Client::Filesystem);
    if (!file) {
        copy_ui_text(s_debug_status, sizeof(s_debug_status),
                     tr(lyra::i18n::StringId::CouldNotOpenMicroSdRoot));
        return false;
    }

    const size_t length = std::strlen(info);
    const bool written = lyra::sd::write_exact(file, info, length,
                                               lyra::sd::Client::Filesystem);
    const bool closed = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0;
    if (!written || !closed) {
        lyra::sd::remove(path, lyra::sd::Client::Filesystem);
        copy_ui_text(s_debug_status, sizeof(s_debug_status),
                     tr(lyra::i18n::StringId::CouldNotWriteMicroSdRoot));
        return false;
    }

    format_text(lyra::i18n::StringId::DebugInfoSaved, path,
                s_debug_status, sizeof(s_debug_status));
    return true;
}

void debug_tab_cb(lv_event_t *event)
{
    s_debug_tab = static_cast<DebugTab>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    render(View::DebugMenu);
}

void maximum_volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    const uint8_t value = static_cast<uint8_t>(lv_slider_get_value(slider));
    const esp_err_t result = lyra::audio::set_maximum_volume_percent(value);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not set maximum volume: %s", esp_err_to_name(result));
        return;
    }
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(value));
        lv_label_set_text(value_label, text);
    }
    update_status_volume_label(lyra::audio::status().volume_percent);
}

void maximum_volume_slider_released_cb(lv_event_t *)
{
    const esp_err_t result = lyra::audio::save_maximum_volume();
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not save maximum volume: %s", esp_err_to_name(result));
    }
    lyra::audio::save_volume();
}

void make_maximum_volume_control(lv_obj_t *parent, int y)
{
    const uint8_t maximum = lyra::audio::maximum_volume_percent();
    lv_obj_t *card = make_box(parent, 7, y, 306, 84, kSurface, 6);
    lv_obj_t *title = make_label(card, tr(lyra::i18n::StringId::MaxVolumeOverride), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
    char value_text[16];
    std::snprintf(value_text, sizeof(value_text), "%u%%", static_cast<unsigned>(maximum));
    lv_obj_t *value_label = make_label(card, value_text, kAccent);
    lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, -12, 10);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 12, 54);
    lv_obj_set_size(slider, 282, 14);
    lv_slider_set_range(slider, 1, lyra::audio::kMaximumVolumePercent);
    lv_slider_set_value(slider, maximum, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value_label);
    lv_obj_add_event_cb(slider, maximum_volume_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, maximum_volume_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, maximum_volume_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

void write_bmp_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

void write_bmp_u32(uint8_t *destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

bool save_screenshot_bmp(const uint8_t *pixels, uint32_t stride, uint32_t width, uint32_t height,
                         lv_color_format_t color_format)
{
    if (!pixels || width == 0 || height == 0 || stride < width * 2u ||
        (color_format != LV_COLOR_FORMAT_RGB565 &&
         color_format != LV_COLOR_FORMAT_RGB565_SWAPPED)) {
        return false;
    }

    const uint32_t row_size = (width * 3u + 3u) & ~3u;
    const uint32_t image_size = row_size * height;
    uint8_t header[54]{};
    header[0] = 'B';
    header[1] = 'M';
    write_bmp_u32(header + 2, 54u + image_size);
    write_bmp_u32(header + 10, 54);
    write_bmp_u32(header + 14, 40);
    write_bmp_u32(header + 18, width);
    write_bmp_u32(header + 22, height);
    write_bmp_u16(header + 26, 1);
    write_bmp_u16(header + 28, 24);
    write_bmp_u32(header + 34, image_size);

    char path[64];
    std::snprintf(path, sizeof(path), "/sdcard/screenshot-%llu-%u.bmp",
                  static_cast<unsigned long long>(esp_timer_get_time()),
                  static_cast<unsigned>(s_screenshot_sequence++));
    FILE *file = lyra::sd::open(path, "wb", lyra::sd::Client::Filesystem);
    if (!file) return false;
    bool written = lyra::sd::write_exact(file, header, sizeof(header),
                                         lyra::sd::Client::Filesystem);
    uint8_t row[static_cast<size_t>(kScreenWidth) * 3u + 3u]{};
    for (uint32_t output_y = 0; written && output_y < height; ++output_y) {
        const uint32_t source_y = height - 1u - output_y;
        const uint8_t *source = pixels + static_cast<size_t>(source_y) * stride;
        std::memset(row, 0, row_size);
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t rgb565 = color_format == LV_COLOR_FORMAT_RGB565_SWAPPED ?
                                    static_cast<uint16_t>((source[x * 2u] << 8) | source[x * 2u + 1u]) :
                                    static_cast<uint16_t>(source[x * 2u] | (source[x * 2u + 1u] << 8));
            row[x * 3u] = static_cast<uint8_t>(((rgb565 >> 11) & 0x1F) * 255 / 31);
            row[x * 3u + 1u] = static_cast<uint8_t>(((rgb565 >> 5) & 0x3F) * 255 / 63);
            row[x * 3u + 2u] = static_cast<uint8_t>((rgb565 & 0x1F) * 255 / 31);
            const uint8_t blue = row[x * 3u];
            row[x * 3u] = row[x * 3u + 2u];
            row[x * 3u + 2u] = blue;
        }
        written = lyra::sd::write_exact(file, row, row_size, lyra::sd::Client::Filesystem);
    }
    const bool closed = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0;
    if (!written || !closed) lyra::sd::remove(path, lyra::sd::Client::Filesystem);
    return written && closed;
}

void screenshot_button_pressed_cb(lv_event_t *event);
void screenshot_button_pressing_cb(lv_event_t *event);
void screenshot_button_clicked_cb(lv_event_t *event);

void create_screenshot_button(int32_t x, int32_t y)
{
    constexpr int32_t kButtonSize = 44;
    x = std::clamp<int32_t>(x, 0, kScreenWidth - kButtonSize);
    y = std::clamp<int32_t>(y, 0, kScreenHeight - kButtonSize);
    s_screenshot_button = make_button(lv_layer_top(), x, y, kButtonSize, kButtonSize,
                                      kAccentDark, 22);
    lv_obj_t *icon = make_label(s_screenshot_button, LV_SYMBOL_IMAGE, kTextOnAccent);
    lv_obj_center(icon);
    lv_obj_add_event_cb(s_screenshot_button, screenshot_button_pressed_cb,
                        LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_screenshot_button, screenshot_button_pressing_cb,
                        LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_screenshot_button, screenshot_button_clicked_cb,
                        LV_EVENT_CLICKED, nullptr);
}

void take_screenshot_cb(lv_event_t *)
{
    if (!s_screenshot_button) return;
    lv_display_t *display = lv_obj_get_display(s_screen);
    if (!display) return;

    // Removing the object avoids copying stale pixels from a direct/double
    // buffered display where hiding alone may not repaint the old button
    // before the active buffer is sampled.
    const int32_t button_x = lv_obj_get_x(s_screenshot_button);
    const int32_t button_y = lv_obj_get_y(s_screenshot_button);
    lv_obj_delete(s_screenshot_button);
    s_screenshot_button = nullptr;
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(s_screen);
    lv_refr_now(display);

    // In direct double-buffer mode LVGL swaps buffers after the flush. The
    // buffer returned as active after the first refresh is therefore the
    // previously displayed buffer, which can still contain the button. A
    // second full refresh updates that buffer too before it is sampled.
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(s_screen);
    lv_refr_now(display);

    lv_draw_buf_t *active = lv_display_get_buf_active(display);
    const bool valid_buffer = active && active->data &&
                              active->header.w == kScreenWidth &&
                              active->header.h == kScreenHeight &&
                              active->header.stride >= kScreenWidth * 2u &&
                              (active->header.cf == LV_COLOR_FORMAT_RGB565 ||
                               active->header.cf == LV_COLOR_FORMAT_RGB565_SWAPPED);
    const uint32_t captured_stride = valid_buffer ? active->header.stride : 0;
    const uint32_t captured_width = valid_buffer ? active->header.w : 0;
    const uint32_t captured_height = valid_buffer ? active->header.h : 0;
    const lv_color_format_t captured_format = valid_buffer ?
        static_cast<lv_color_format_t>(active->header.cf) : LV_COLOR_FORMAT_RGB565;
    uint8_t *copy = nullptr;
    if (valid_buffer) {
        const size_t bytes = static_cast<size_t>(active->header.stride) * active->header.h;
        copy = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!copy) copy = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
        if (copy) std::memcpy(copy, active->data, bytes);
    }

    create_screenshot_button(button_x, button_y);
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(display);
    const bool saved = copy && save_screenshot_bmp(copy, captured_stride, captured_width,
                                                   captured_height, captured_format);
    heap_caps_free(copy);
    show_notice(saved ? tr(lyra::i18n::StringId::ScreenshotSaved) :
                       tr(lyra::i18n::StringId::ScreenshotFailed),
                 saved ? tr(lyra::i18n::StringId::ScreenshotSavedMessage) :
                         tr(lyra::i18n::StringId::ScreenshotFailedMessage));
}

void screenshot_button_pressed_cb(lv_event_t *)
{
    if (!lv_indev_active()) return;
    lv_indev_get_point(lv_indev_active(), &s_screenshot_press_point);
    s_screenshot_button_start.x = lv_obj_get_x(s_screenshot_button);
    s_screenshot_button_start.y = lv_obj_get_y(s_screenshot_button);
    s_screenshot_dragged = false;
}

void screenshot_button_pressing_cb(lv_event_t *)
{
    if (!s_screenshot_button || !lv_indev_active()) return;
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    const int32_t dx = point.x - s_screenshot_press_point.x;
    const int32_t dy = point.y - s_screenshot_press_point.y;
    if (std::abs(dx) > 3 || std::abs(dy) > 3) s_screenshot_dragged = true;
    const int32_t width = lv_obj_get_width(s_screenshot_button);
    const int32_t height = lv_obj_get_height(s_screenshot_button);
    const int32_t x = std::clamp<int32_t>(s_screenshot_button_start.x + dx, 0,
                                          kScreenWidth - width);
    const int32_t y = std::clamp<int32_t>(s_screenshot_button_start.y + dy, 0,
                                          kScreenHeight - height);
    lv_obj_set_pos(s_screenshot_button, x, y);
}

void screenshot_button_clicked_cb(lv_event_t *)
{
    if (s_screenshot_dragged) {
        s_screenshot_dragged = false;
        return;
    }
    take_screenshot_cb(nullptr);
}

void toggle_screenshot_button_cb(lv_event_t *)
{
    if (s_screenshot_button) {
        lv_obj_delete(s_screenshot_button);
        s_screenshot_button = nullptr;
    } else {
        create_screenshot_button(268, 220);
    }
    render(View::DebugMenu);
}

void confirm_clear_nvs_cb(lv_event_t *event)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    esp_err_t result = nvs_flash_erase();
    if (result == ESP_OK) result = nvs_flash_init();
    if (result == ESP_ERR_INVALID_STATE) result = ESP_OK;
    if (result == ESP_OK) {
        copy_ui_text(s_debug_status, sizeof(s_debug_status),
                     tr(lyra::i18n::StringId::NvsCleared));
    } else {
        format_text(lyra::i18n::StringId::NvsClearFailed, esp_err_to_name(result),
                    s_debug_status, sizeof(s_debug_status));
    }
    if (overlay) lv_obj_delete(overlay);
    render(View::DebugMenu);
}

void show_clear_nvs_confirmation()
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 126, 280, 228, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::ClearNvsQuestion), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog, tr(lyra::i18n::StringId::ClearNvsMessage),
                                   kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *cancel = make_button(dialog, 14, 164, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 164, 116, 48, lv_color_hex(0x991B1B), 7);
    lv_obj_t *confirm_label = make_label(confirm, tr(lyra::i18n::StringId::Clear), kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_clear_nvs_cb, LV_EVENT_CLICKED, overlay);
}

struct FirmwareUpdateFailure {
    char message[176];
};

void firmware_update_failure_async_cb(void *context)
{
    auto *failure = static_cast<FirmwareUpdateFailure *>(context);
    if (failure) {
        render(View::About);
        show_notice(tr(lyra::i18n::StringId::FirmwareUpdateFailed), failure->message);
        heap_caps_free(failure);
    }
    s_firmware_update_in_progress = false;
}

void post_firmware_update_failure(const char *message)
{
    auto *failure = static_cast<FirmwareUpdateFailure *>(
        heap_caps_malloc(sizeof(FirmwareUpdateFailure), MALLOC_CAP_8BIT));
    if (!failure) {
        ESP_LOGE(kTag, "could not allocate firmware update error message");
        s_firmware_update_in_progress = false;
        return;
    }
    std::strncpy(failure->message, message ? message :
                 tr(lyra::i18n::StringId::FirmwareUpdateDefaultError),
                 sizeof(failure->message) - 1);
    failure->message[sizeof(failure->message) - 1] = '\0';
    lv_async_call(firmware_update_failure_async_cb, failure);
}

bool firmware_update_file_size(FILE *file, size_t *size)
{
    if (!file || !size || !lyra::sd::acquire(lyra::sd::Client::Filesystem)) return false;
    const bool seek_succeeded = std::fseek(file, 0, SEEK_END) == 0;
    const long end = seek_succeeded ? std::ftell(file) : -1;
    lyra::sd::release(lyra::sd::Client::Filesystem);
    if (!seek_succeeded || end <= 0) return false;
    *size = static_cast<size_t>(end);
    return true;
}

bool firmware_update_media_idle()
{
    constexpr int kWaitCount = 600;
    for (int wait_count = 0; wait_count < kWaitCount; ++wait_count) {
        const lyra::media::Status media_status = lyra::media::status();
        const lyra::media::SearchStatus search_status = lyra::media::search_status();
        if (!media_status.scanning && !media_status.duration_indexing &&
            !media_status.sorting_indexing && !media_status.artwork_busy &&
            !search_status.running) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

void firmware_update_task(void *)
{
    constexpr const char *kUpdatePath = "/sdcard/update.bin";
    constexpr size_t kTransferBufferSize = 8192;
    FILE *file = lyra::sd::open(kUpdatePath, "rb", lyra::sd::Client::Filesystem);
    if (!file) {
        post_firmware_update_failure(tr(lyra::i18n::StringId::UpdateFileMissing));
        vTaskDelete(nullptr);
        return;
    }

    // Stop playback before taking the filesystem gate for the long transfer.
    // The audio task closes its current file asynchronously, so wait for it
    // to finish before touching the update image.
    const esp_err_t stop_result = lyra::audio::stop();
    if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        post_firmware_update_failure(tr(lyra::i18n::StringId::CouldNotStopAudio));
        vTaskDelete(nullptr);
        return;
    }
    for (int wait_count = 0; wait_count < 200; ++wait_count) {
        if (!lyra::audio::status().playing) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (lyra::audio::status().playing || !firmware_update_media_idle()) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        post_firmware_update_failure(tr(lyra::i18n::StringId::SdCardBusy));
        vTaskDelete(nullptr);
        return;
    }

    size_t file_size = 0;
    if (!firmware_update_file_size(file, &file_size)) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        post_firmware_update_failure(tr(lyra::i18n::StringId::CouldNotReadUpdate));
        vTaskDelete(nullptr);
        return;
    }

    // Reject obviously non-ESP images before erasing the inactive slot. Full
    // image validation is performed by esp_ota_end after the transfer.
    uint8_t image_header[2]{};
    const bool header_read = lyra::sd::seek(file, 0, SEEK_SET,
                                             lyra::sd::Client::Filesystem) == 0 &&
                             lyra::sd::read(file, image_header, sizeof(image_header),
                                            lyra::sd::Client::Filesystem) == sizeof(image_header);
    if (!header_read || image_header[0] != 0xE9 || image_header[1] == 0 || image_header[1] > 16) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        post_firmware_update_failure(tr(lyra::i18n::StringId::InvalidFirmwareUpdate));
        vTaskDelete(nullptr);
        return;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(running);
    if (!target || target == running || file_size > target->size) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        post_firmware_update_failure(tr(lyra::i18n::StringId::UpdateTooLarge));
        vTaskDelete(nullptr);
        return;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t result = esp_ota_begin(target, file_size, &ota_handle);
    if (result != ESP_OK) {
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        char message[176];
        format_text(lyra::i18n::StringId::CouldNotPrepareOta, esp_err_to_name(result),
                    message, sizeof(message));
        post_firmware_update_failure(message);
        vTaskDelete(nullptr);
        return;
    }

    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(kTransferBufferSize, MALLOC_CAP_8BIT));
    if (!buffer) {
        esp_ota_abort(ota_handle);
        lyra::sd::close(file, lyra::sd::Client::Filesystem);
        post_firmware_update_failure(tr(lyra::i18n::StringId::NotEnoughMemoryUpdate));
        vTaskDelete(nullptr);
        return;
    }

    bool transfer_succeeded = lyra::sd::seek(file, 0, SEEK_SET,
                                             lyra::sd::Client::Filesystem) == 0;
    size_t remaining = file_size;
    while (transfer_succeeded && remaining > 0) {
        const size_t wanted = remaining < kTransferBufferSize ? remaining : kTransferBufferSize;
        const size_t read = lyra::sd::read(file, buffer, wanted, lyra::sd::Client::Filesystem);
        if (read != wanted) {
            transfer_succeeded = false;
            break;
        }
        result = esp_ota_write(ota_handle, buffer, read);
        if (result != ESP_OK) {
            transfer_succeeded = false;
            break;
        }
        remaining -= read;
        vTaskDelay(1);
    }
    heap_caps_free(buffer);
    const int close_result = lyra::sd::close(file, lyra::sd::Client::Filesystem);
    if (close_result != 0) transfer_succeeded = false;

    if (!transfer_succeeded) {
        esp_ota_abort(ota_handle);
        post_firmware_update_failure(tr(lyra::i18n::StringId::CouldNotReadWriteUpdate));
        vTaskDelete(nullptr);
        return;
    }

    // esp_ota_end verifies the complete image, including its checksum and
    // image structure. Do not change the persistent boot target until this
    // validation succeeds.
    result = esp_ota_end(ota_handle);
    if (result != ESP_OK) {
        char message[176];
        format_text(lyra::i18n::StringId::InvalidFirmwareUpdateDetail,
                    esp_err_to_name(result), message, sizeof(message));
        post_firmware_update_failure(message);
        vTaskDelete(nullptr);
        return;
    }

    result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        char message[176];
        format_text(lyra::i18n::StringId::CouldNotSelectFirmware,
                    esp_err_to_name(result), message, sizeof(message));
        post_firmware_update_failure(message);
        vTaskDelete(nullptr);
        return;
    }

    // The source file is closed and the image is selected before unmounting;
    // this keeps the card safe and leaves the new OTA slot as the persistent
    // boot target across the restart.
    const esp_err_t shutdown_result = lyra::media::shutdown();
    if (shutdown_result != ESP_OK) {
        post_firmware_update_failure(tr(lyra::i18n::StringId::SdUnmountFailed));
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "firmware update installed in %s; restarting", target->label);
    lyra_board_display_set_backlight(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    vTaskDelete(nullptr);
}

void start_firmware_update()
{
    if (s_firmware_update_in_progress) return;
    s_firmware_update_in_progress = true;
    style_root();
    lv_obj_t *label = make_label(s_screen, tr(lyra::i18n::StringId::UpdatingFirmware),
                                 kTextPrimary);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    lv_refr_now(lv_obj_get_display(s_screen));
    if (xTaskCreatePinnedToCore(firmware_update_task, "lyra_ota", 8192, nullptr,
                                3, nullptr, 0) != pdPASS) {
        s_firmware_update_in_progress = false;
        render(View::About);
        show_notice(tr(lyra::i18n::StringId::FirmwareUpdateFailed),
                    tr(lyra::i18n::StringId::CouldNotStartUpdateTask));
    }
}

void confirm_firmware_update_cb(lv_event_t *event)
{
    lv_obj_t *overlay = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    if (overlay) lv_obj_delete(overlay);
    start_firmware_update();
}

void show_firmware_update_confirmation()
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 124, 280, 232, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::UpdateFirmwareQuestion), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog, tr(lyra::i18n::StringId::InstallUpdateMessage),
                                   kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *cancel = make_button(dialog, 14, 164, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 164, 116, 48, kAccentDark, 7);
    lv_obj_t *confirm_label = make_label(confirm, tr(lyra::i18n::StringId::Update), kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_firmware_update_cb, LV_EVENT_CLICKED, overlay);
}

void firmware_update_cb(lv_event_t *)
{
    show_firmware_update_confirmation();
}

void run_force_boot_test(uint8_t ota_slot)
{
    const esp_err_t volume_result = lyra::audio::save_volume();
    if (volume_result != ESP_OK) {
        ESP_LOGW(kTag, "could not save volume before boot test: %s",
                 esp_err_to_name(volume_result));
    }
    lyra::audio::save_maximum_volume();
    lyra::audio::stop();
    for (int wait_count = 0; wait_count < 200; ++wait_count) {
        if (!lyra::audio::status().playing) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool was_mounted = lyra::media::status().mounted;
    esp_err_t result = was_mounted ? lyra::media::shutdown() : ESP_OK;
    if (result == ESP_OK) result = lyra::boot_test::request_once(ota_slot);
    if (result != ESP_OK) {
        if (was_mounted) lyra::media::init();
        char message[96];
        format_u32_text(lyra::i18n::StringId::BootTestFailed,
                        static_cast<uint32_t>(ota_slot), esp_err_to_name(result),
                        message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::BootTestFailedTitle), message);
    }
}

void confirm_force_boot_test_cb(lv_event_t *event)
{
    const uint8_t ota_slot = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    lv_obj_t *confirm = lv_event_get_current_target_obj(event);
    lv_obj_t *dialog = confirm ? lv_obj_get_parent(confirm) : nullptr;
    lv_obj_t *overlay = dialog ? lv_obj_get_parent(dialog) : nullptr;
    if (overlay) lv_obj_delete(overlay);

    style_root();
    lv_obj_t *label = make_label(s_screen, tr(lyra::i18n::StringId::RebootingOta),
                                 kTextPrimary);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -10);
    lv_refr_now(lv_obj_get_display(s_screen));
    run_force_boot_test(ota_slot);
}

void show_force_boot_confirmation(uint8_t ota_slot)
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_90, 0);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 20, 116, 280, 252, kSurfaceRaised, 12);
    char title_text[32];
    format_u32(lyra::i18n::StringId::BootOtaQuestion,
               static_cast<uint32_t>(ota_slot), title_text, sizeof(title_text));
    lv_obj_t *title = make_label(dialog, title_text, kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *message = make_label(dialog, tr(lyra::i18n::StringId::ValidFirmwareFound),
                                   kTextSecondary);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *cancel = make_button(dialog, 14, 188, 116, 48, kSurface, 7);
    lv_obj_t *cancel_label = make_label(cancel, tr(lyra::i18n::StringId::Cancel), kTextSecondary);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(cancel, cancel_power_action_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *confirm = make_button(dialog, 150, 188, 116, 48, kAccentDark, 7);
    lv_obj_t *confirm_label = make_label(confirm, tr(lyra::i18n::StringId::Reboot), kTextOnAccent);
    lv_obj_center(confirm_label);
    lv_obj_add_event_cb(confirm, confirm_force_boot_test_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(ota_slot)));
}

void force_boot_test_cb(lv_event_t *event)
{
    const uint8_t ota_slot = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    const esp_err_t result = lyra::boot_test::validate(ota_slot);
    if (result != ESP_OK) {
        char message[112];
        if (result == ESP_ERR_NOT_FOUND) {
            format_u32(lyra::i18n::StringId::OtaNotInPartitionTable,
                       static_cast<uint32_t>(ota_slot), message, sizeof(message));
        } else {
            format_u32(lyra::i18n::StringId::OtaInvalidImage,
                       static_cast<uint32_t>(ota_slot), message, sizeof(message));
        }
        show_notice(tr(lyra::i18n::StringId::BootTestUnavailable), message);
        return;
    }
    show_force_boot_confirmation(ota_slot);
}

void dump_debug_info_cb(lv_event_t *)
{
    dump_debug_info_to_sd();
    render(View::DebugMenu);
}

void render_debug_menu()
{
    make_header(tr(lyra::i18n::StringId::ServiceMenu), View::About, true);
    lv_obj_t *body = make_scroll_body(72);
    for (size_t index = 0; index < 2; ++index) {
        const DebugTab tab = static_cast<DebugTab>(index);
        lv_obj_t *button = make_button(body, 7 + static_cast<int>(index) * 153, 4, 150, 36,
                                       s_debug_tab == tab ? kAccentDark : kSurfaceRaised, 6);
        lv_obj_t *label = make_label(button, tab == DebugTab::Info ?
                                     tr(lyra::i18n::StringId::Info) :
                                     tr(lyra::i18n::StringId::Debug),
                                     s_debug_tab == tab ? kTextOnAccent : kTextSecondary);
        lv_obj_center(label);
        lv_obj_add_event_cb(button, debug_tab_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(tab)));
    }

    if (s_debug_tab == DebugTab::Info) {
        char info[kDebugInfoCapacity];
        build_debug_info(info, sizeof(info));
        lv_obj_t *label = make_label(body, info, kTextSecondary);
        lv_obj_set_pos(label, 14, 52);
        lv_obj_set_width(label, 292);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_line_space(label, 4, 0);
        return;
    }

    lv_obj_t *dump = make_row(body, 48, LV_SYMBOL_SAVE,
                              tr(lyra::i18n::StringId::DumpInfoToSd),
                              tr(lyra::i18n::StringId::SaveInfoToSdRoot),
                              View::DebugMenu, 62);
    lv_obj_remove_event_cb(dump, route_cb);
    lv_obj_add_event_cb(dump, dump_debug_info_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *screenshot = make_row(body, 114, LV_SYMBOL_IMAGE,
                                    tr(lyra::i18n::StringId::ScreenshotButton),
                                    s_screenshot_button ? tr(lyra::i18n::StringId::TapToHideFloating) :
                                                          tr(lyra::i18n::StringId::ShowMovableFloating),
                                    View::DebugMenu, 62);
    lv_obj_remove_event_cb(screenshot, route_cb);
    lv_obj_add_event_cb(screenshot, toggle_screenshot_button_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *ota0 = make_row(body, 180, LV_SYMBOL_REFRESH,
                              tr(lyra::i18n::StringId::ForceBootOta0),
                              tr(lyra::i18n::StringId::ValidateConfirmTest),
                              View::DebugMenu, 62);
    lv_obj_remove_event_cb(ota0, route_cb);
    lv_obj_add_event_cb(ota0, force_boot_test_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(0)));
    lv_obj_t *ota1 = make_row(body, 246, LV_SYMBOL_REFRESH,
                              tr(lyra::i18n::StringId::ForceBootOta1),
                              tr(lyra::i18n::StringId::ValidateConfirmTest),
                              View::DebugMenu, 62);
    lv_obj_remove_event_cb(ota1, route_cb);
    lv_obj_add_event_cb(ota1, force_boot_test_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(1)));

    lv_obj_t *clear = make_row(body, 312, LV_SYMBOL_CLOSE,
                               tr(lyra::i18n::StringId::ClearNvs),
                               tr(lyra::i18n::StringId::RemoveSavedSettings),
                               View::DebugMenu, 62);
    lv_obj_remove_event_cb(clear, route_cb);
    lv_obj_add_event_cb(clear, [](lv_event_t *) { show_clear_nvs_confirmation(); },
                        LV_EVENT_CLICKED, nullptr);
    make_maximum_volume_control(body, 378);
    if (s_debug_status[0]) {
        lv_obj_t *status = make_label(body, s_debug_status, kTextMuted);
        lv_obj_set_pos(status, 14, 470);
        lv_obj_set_width(status, 292);
        lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_WRAP);
    }
}

} // namespace lyra::gui::internal
