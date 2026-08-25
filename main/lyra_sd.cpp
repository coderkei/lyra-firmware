/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_sd.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

namespace lyra::sd {
namespace {

SemaphoreHandle_t s_gate;
portMUX_TYPE s_waiter_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t s_audio_waiters;

void add_audio_waiter()
{
    portENTER_CRITICAL(&s_waiter_mux);
    ++s_audio_waiters;
    portEXIT_CRITICAL(&s_waiter_mux);
}

void remove_audio_waiter()
{
    portENTER_CRITICAL(&s_waiter_mux);
    if (s_audio_waiters > 0) --s_audio_waiters;
    portEXIT_CRITICAL(&s_waiter_mux);
}

bool timed_out(TickType_t started, TickType_t timeout)
{
    return timeout != portMAX_DELAY &&
           (xTaskGetTickCount() - started) >= timeout;
}

} // namespace

bool init()
{
    if (s_gate) return true;
    s_gate = xSemaphoreCreateMutex();
    return s_gate != nullptr;
}

bool audio_waiting()
{
    portENTER_CRITICAL(&s_waiter_mux);
    const bool waiting = s_audio_waiters != 0;
    portEXIT_CRITICAL(&s_waiter_mux);
    return waiting;
}

bool acquire(Client client, TickType_t timeout)
{
    if (!s_gate && !init()) return false;
    if (client == Client::Audio) {
        add_audio_waiter();
        const BaseType_t taken = xSemaphoreTake(s_gate, timeout);
        remove_audio_waiter();
        return taken == pdTRUE;
    }

    // FreeRTOS mutexes provide inheritance but not strict FIFO priority for
    // a mixed-priority queue. Polling here lets an audio waiter win the next
    // transaction even if artwork reached the gate first.
    const TickType_t started = xTaskGetTickCount();
    while (true) {
        if (audio_waiting()) {
            if (timeout != portMAX_DELAY && timed_out(started, timeout)) return false;
            vTaskDelay(1);
            continue;
        }
        if (xSemaphoreTake(s_gate, 0) == pdTRUE) return true;
        if (timeout != portMAX_DELAY && timed_out(started, timeout)) return false;
        vTaskDelay(1);
    }
}

void release(Client)
{
    if (s_gate) xSemaphoreGive(s_gate);
}

size_t read(FILE *file, void *buffer, size_t size, Client client)
{
    if (!file || !buffer || size == 0 || !acquire(client)) return 0;
    const size_t result = std::fread(buffer, 1, size, file);
    release(client);
    return result;
}

size_t write(FILE *file, const void *buffer, size_t size, Client client)
{
    if (!file || !buffer || size == 0 || !acquire(client)) return 0;
    const size_t result = std::fwrite(buffer, 1, size, file);
    release(client);
    return result;
}

int seek(FILE *file, long offset, int origin, Client client)
{
    if (!file || !acquire(client)) return -1;
    const int result = std::fseek(file, offset, origin);
    release(client);
    return result;
}

FILE *open(const char *path, const char *mode, Client client)
{
    if (!path || !mode || !acquire(client)) return nullptr;
    FILE *file = std::fopen(path, mode);
    release(client);
    return file;
}

int close(FILE *file, Client client)
{
    if (!file || !acquire(client)) return EOF;
    const int result = std::fclose(file);
    release(client);
    return result;
}

int remove(const char *path, Client client)
{
    if (!path || !acquire(client)) return -1;
    const int result = std::remove(path);
    release(client);
    return result;
}

int rename(const char *old_path, const char *new_path, Client client)
{
    if (!old_path || !new_path || !acquire(client)) return -1;
    const int result = std::rename(old_path, new_path);
    release(client);
    return result;
}

bool read_exact(FILE *file, void *buffer, size_t size, Client client,
                size_t chunk_size)
{
    if (!file || !buffer || chunk_size == 0) return false;
    auto *destination = static_cast<unsigned char *>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
        const size_t wanted = remaining < chunk_size ? remaining : chunk_size;
        const size_t count = read(file, destination, wanted, client);
        if (count != wanted) return false;
        destination += count;
        remaining -= count;
        if (remaining > 0 && client != Client::Audio) vTaskDelay(1);
    }
    return true;
}

bool write_exact(FILE *file, const void *buffer, size_t size, Client client,
                 size_t chunk_size)
{
    if (!file || !buffer || chunk_size == 0) return false;
    const auto *source = static_cast<const unsigned char *>(buffer);
    size_t remaining = size;
    while (remaining > 0) {
        const size_t wanted = remaining < chunk_size ? remaining : chunk_size;
        const size_t count = write(file, source, wanted, client);
        if (count != wanted) return false;
        source += count;
        remaining -= count;
        if (remaining > 0 && client != Client::Audio) vTaskDelay(1);
    }
    return true;
}

} // namespace lyra::sd

extern "C" size_t lyra_sd_artwork_read(FILE *file, void *buffer, size_t size)
{
    return lyra::sd::read_exact(file, buffer, size, lyra::sd::Client::Artwork)
        ? size : 0;
}

extern "C" size_t lyra_sd_artwork_write(FILE *file, const void *buffer, size_t size)
{
    return lyra::sd::write_exact(file, buffer, size, lyra::sd::Client::Artwork)
        ? size : 0;
}

extern "C" int lyra_sd_artwork_seek(FILE *file, long offset, int origin)
{
    return lyra::sd::seek(file, offset, origin, lyra::sd::Client::Artwork);
}

extern "C" FILE *lyra_sd_artwork_open(const char *path, const char *mode)
{
    return lyra::sd::open(path, mode, lyra::sd::Client::Artwork);
}

extern "C" int lyra_sd_artwork_close(FILE *file)
{
    return lyra::sd::close(file, lyra::sd::Client::Artwork);
}

extern "C" int lyra_sd_artwork_remove(const char *path)
{
    return lyra::sd::remove(path, lyra::sd::Client::Artwork);
}
