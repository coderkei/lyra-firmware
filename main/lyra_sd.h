/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
#include <cstddef>
#include <cstdio>
#include "freertos/FreeRTOS.h"
#else
#include <stddef.h>
#include <stdio.h>
#endif

#ifdef __cplusplus
namespace lyra::sd {

enum class Client : unsigned char {
    Audio,
    Artwork,
    Filesystem,
};

// The gate is deliberately held only for one bounded VFS operation. In
// particular, callers must release it before doing JPEG/PNG work.
bool init();
bool audio_waiting();
bool acquire(Client client, TickType_t timeout = portMAX_DELAY);
void release(Client client);

size_t read(FILE *file, void *buffer, size_t size, Client client);
size_t write(FILE *file, const void *buffer, size_t size, Client client);
int seek(FILE *file, long offset, int origin, Client client);
FILE *open(const char *path, const char *mode, Client client);
int close(FILE *file, Client client);
int remove(const char *path, Client client);
int rename(const char *old_path, const char *new_path, Client client);

// Non-realtime transfers are split into bounded transactions so a waiting
// audio read can take priority between chunks.
bool read_exact(FILE *file, void *buffer, size_t size, Client client,
                size_t chunk_size = 8192);
bool write_exact(FILE *file, const void *buffer, size_t size, Client client,
                 size_t chunk_size = 8192);

} // namespace lyra::sd
#endif

// C hooks used by libjpeg's transient virtual-array backing store.
#ifdef __cplusplus
extern "C" {
#endif
size_t lyra_sd_artwork_read(FILE *file, void *buffer, size_t size);
size_t lyra_sd_artwork_write(FILE *file, const void *buffer, size_t size);
int lyra_sd_artwork_seek(FILE *file, long offset, int origin);
FILE *lyra_sd_artwork_open(const char *path, const char *mode);
int lyra_sd_artwork_close(FILE *file);
int lyra_sd_artwork_remove(const char *path);
#ifdef __cplusplus
}
#endif
