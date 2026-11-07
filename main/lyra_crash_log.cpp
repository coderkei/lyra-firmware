/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_crash_log.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "lyra_clock.h"
#include "lyra_media.h"
#include "lyra_sd.h"
#include "sdkconfig.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif

namespace lyra::crash_log {
namespace {

constexpr const char *kTag = "lyra.crash";
constexpr const char *kDataDir = "/sdcard/.lyra";
constexpr const char *kCrashDir = "/sdcard/.lyra/crash";
constexpr size_t kCopyChunk = 4096;
std::atomic<bool> s_notice{false};

const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_PANIC: return "panic (exception)";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout (supply voltage dropped)";
        case ESP_RST_CPU_LOCKUP: return "CPU lockup";
        default: return "other";
    }
}

bool crash_reset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT || reason == ESP_RST_CPU_LOCKUP;
}

bool ensure_directory(const char *path)
{
    struct stat info{};
    if (stat(path, &info) == 0) return S_ISDIR(info.st_mode);
    return mkdir(path, 0775) == 0;
}

// crash-YYYYMMDD-HHMM, with a counter when that name is taken.
bool report_base(char *output, size_t capacity)
{
    const lyra::clock::DateTime now = lyra::clock::now();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (attempt == 0) {
            std::snprintf(output, capacity, "%s/crash-%04d%02d%02d-%02d%02d", kCrashDir,
                          now.year, now.month, now.day, now.hour, now.minute);
        } else {
            std::snprintf(output, capacity, "%s/crash-%04d%02d%02d-%02d%02d-%u", kCrashDir,
                          now.year, now.month, now.day, now.hour, now.minute, attempt);
        }
        char probe[160];
        std::snprintf(probe, sizeof(probe), "%s.txt", output);
        struct stat info{};
        if (stat(probe, &info) != 0) return true;
    }
    return false;
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
// Copies the stored ELF core dump so `idf.py coredump-info` can read it.
bool copy_core_dump(const char *path, size_t address, size_t size)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!partition || address < partition->address ||
        address + size > partition->address + partition->size) return false;
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(kCopyChunk, MALLOC_CAP_8BIT));
    FILE *file = buffer ? lyra::sd::open(path, "wb", lyra::sd::Client::Filesystem) : nullptr;
    bool ok = file != nullptr;
    for (size_t done = 0; ok && done < size; done += kCopyChunk) {
        const size_t length = size - done < kCopyChunk ? size - done : kCopyChunk;
        ok = esp_partition_read(partition, address - partition->address + done, buffer, length) == ESP_OK &&
             lyra::sd::write_exact(file, buffer, length, lyra::sd::Client::Filesystem);
    }
    if (file) ok = lyra::sd::close(file, lyra::sd::Client::Filesystem) == 0 && ok;
    heap_caps_free(buffer);
    if (!ok) lyra::sd::remove(path, lyra::sd::Client::Filesystem);
    return ok;
}
#endif

} // namespace

void save_after_boot()
{
    const esp_reset_reason_t reason = esp_reset_reason();
    bool have_dump = false;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t dump_address = 0;
    size_t dump_size = 0;
    have_dump = esp_core_dump_image_check() == ESP_OK &&
                esp_core_dump_image_get(&dump_address, &dump_size) == ESP_OK && dump_size > 0;
#endif
    if (!have_dump && !crash_reset(reason)) return;
    if (!lyra::media::begin_file_access()) {
        ESP_LOGW(kTag, "crash report not saved: MicroSD unavailable");
        return;
    }
    char base[128];
    char path[160];
    bool ready = false;
    if (lyra::sd::acquire(lyra::sd::Client::Filesystem)) {
        ready = ensure_directory(kDataDir) && ensure_directory(kCrashDir) &&
                report_base(base, sizeof(base));
        lyra::sd::release(lyra::sd::Client::Filesystem);
    }
    FILE *report = nullptr;
    if (ready) {
        std::snprintf(path, sizeof(path), "%s.txt", base);
        report = lyra::sd::open(path, "w", lyra::sd::Client::Filesystem);
    }
    if (!report) {
        lyra::media::end_file_access();
        ESP_LOGW(kTag, "crash report not saved: cannot create a file in %s", kCrashDir);
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    char elf_sha[17]{};
    esp_app_get_elf_sha256(elf_sha, sizeof(elf_sha));
    const lyra::clock::DateTime now = lyra::clock::now();
    char text[1536];
    int length = std::snprintf(text, sizeof(text),
        "Lyra crash report\n"
        "Firmware: %s %s (ELF %s, built %s %s)\n"
        "Saved: %04d-%02d-%02d %02d:%02d (clock may be unset after a crash)\n"
        "Reset reason: %s (%d)\n",
        app->project_name, app->version, elf_sha, app->date, app->time,
        now.year, now.month, now.day, now.hour, now.minute,
        reset_reason_name(reason), static_cast<int>(reason));
    bool saved_dump = false;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    if (have_dump) {
        char panic[96]{};
        if (esp_core_dump_get_panic_reason(panic, sizeof(panic)) == ESP_OK && length > 0 &&
            length < int(sizeof(text))) {
            length += std::snprintf(text + length, sizeof(text) - length, "Panic: %s\n", panic);
        }
        auto *summary = static_cast<esp_core_dump_summary_t *>(
            heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_8BIT));
        if (summary && esp_core_dump_get_summary(summary) == ESP_OK && length > 0 &&
            length < int(sizeof(text))) {
            length += std::snprintf(text + length, sizeof(text) - length,
                "Task: %.16s\nPC: 0x%08" PRIx32 "\nException cause: %" PRIu32
                ", address 0x%08" PRIx32 "\nBacktrace%s:",
                summary->exc_task, summary->exc_pc, summary->ex_info.exc_cause,
                summary->ex_info.exc_vaddr, summary->exc_bt_info.corrupted ? " (corrupted)" : "");
            const uint32_t depth = summary->exc_bt_info.depth < 16 ? summary->exc_bt_info.depth : 16;
            for (uint32_t i = 0; i < depth && length > 0 && length < int(sizeof(text)); ++i) {
                length += std::snprintf(text + length, sizeof(text) - length, " 0x%08" PRIx32,
                                        summary->exc_bt_info.bt[i]);
            }
            if (length > 0 && length < int(sizeof(text))) {
                length += std::snprintf(text + length, sizeof(text) - length,
                    "\n\nDecode the backtrace with the matching build:\n"
                    "  xtensa-esp32s3-elf-addr2line -pfiaC -e build/lyra_firmware.elf <addresses>\n");
            }
        }
        heap_caps_free(summary);
        char dump_path[168];
        std::snprintf(dump_path, sizeof(dump_path), "%s-core.elf", base);
        saved_dump = copy_core_dump(dump_path, dump_address, dump_size);
        if (saved_dump && length > 0 && length < int(sizeof(text))) {
            length += std::snprintf(text + length, sizeof(text) - length,
                "Full core dump: %s\n  idf.py coredump-info --core %s --core-format elf\n",
                dump_path, dump_path);
        }
    } else if (length > 0 && length < int(sizeof(text))) {
        length += std::snprintf(text + length, sizeof(text) - length,
                                "No core dump was stored for this reset.\n");
    }
#else
    if (length > 0 && length < int(sizeof(text))) {
        length += std::snprintf(text + length, sizeof(text) - length,
            "Core dumps are disabled in this build (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH).\n");
    }
#endif
    if (length < 0) length = 0;
    if (length >= int(sizeof(text))) length = int(sizeof(text)) - 1;
    const bool written = lyra::sd::write_exact(report, text, size_t(length),
                                               lyra::sd::Client::Filesystem);
    const bool closed = lyra::sd::close(report, lyra::sd::Client::Filesystem) == 0;
    lyra::media::end_file_access();
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    // Keep the flash copy until it is safely on the card.
    if (have_dump && written && closed && saved_dump) esp_core_dump_image_erase();
#endif
    if (written && closed) {
        ESP_LOGW(kTag, "crash report saved to %s", path);
        s_notice.store(true);
    }
    (void)saved_dump;
}

bool take_report_notice()
{
    return s_notice.exchange(false);
}

} // namespace lyra::crash_log
