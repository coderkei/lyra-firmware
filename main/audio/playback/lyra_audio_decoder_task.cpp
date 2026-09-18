/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_audio_internal.h"

namespace lyra::audio::internal {

esp_err_t play_file(const char *path, uint32_t generation, uint32_t start_position_ms,
                    bool start_paused, bool pause_after_seek)
{
    AudioFormat format{};
    if (!get_audio_format(path, &format)) return ESP_ERR_NOT_SUPPORTED;

    FILE *file = lyra::sd::open(path, "rb", lyra::sd::Client::Audio);
    if (file == nullptr) {
        ESP_LOGE(kTag, "failed to open %s: %s", audio_format_name(format), path);
        set_error(ESP_ERR_NOT_FOUND, path);
        return ESP_ERR_NOT_FOUND;
    }

    OggOpusReader ogg_opus_reader(file);
    if (format == AudioFormat::kOgg && ogg_opus_reader.is_opus_stream()) {
        format = AudioFormat::kOpus;
    } else if (format == AudioFormat::kOgg) {
        const char *extension = std::strrchr(path, '.');
        if (extension && extension_is(extension + 1, "opus")) {
            // A .opus file must be an Ogg Opus stream here. Raw Opus has no
            // packet boundaries or stream configuration, so passing it through
            // the OGG parser makes the codec receive the whole file as one
            // packet (and produces misleading decoder-length errors).
            ESP_LOGE(kTag, "unsupported raw or corrupt OPUS stream: %s", path);
            lyra::sd::close(file, lyra::sd::Client::Audio);
            set_error(ESP_ERR_NOT_SUPPORTED, path);
            return ESP_ERR_NOT_SUPPORTED;
        }
    }

    long file_end = -1;
    if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
    audio_seek(file, 0, SEEK_SET);

    AiffStreamInfo aiff_info{};
    if (format == AudioFormat::kAiff && !read_aiff_stream_info(file, &aiff_info)) {
        ESP_LOGE(kTag, "unsupported or corrupt AIFF PCM stream: %s", path);
        lyra::sd::close(file, lyra::sd::Client::Audio);
        set_error(ESP_ERR_NOT_SUPPORTED, path);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t duration_ms = 0;
    switch (format) {
    case AudioFormat::kMp3: duration_ms = read_mp3_duration_ms(file); break;
    case AudioFormat::kFlac: duration_ms = read_flac_duration_ms(file); break;
    case AudioFormat::kAac: duration_ms = read_aac_duration_ms(file); break;
    case AudioFormat::kM4a: duration_ms = read_m4a_duration_ms(file); break;
    case AudioFormat::kWav: duration_ms = read_wav_duration_ms(file); break;
    case AudioFormat::kOgg: duration_ms = read_ogg_duration_ms(file); break;
    case AudioFormat::kOpus: duration_ms = read_ogg_tail_duration_ms(
        file, ogg_opus_reader.sample_rate, ogg_opus_reader.pre_skip); break;
    case AudioFormat::kAiff: duration_ms = read_aiff_duration_ms(aiff_info); break;
    }
    audio_seek(file, 0, SEEK_SET);

    if (duration_ms > 0 && start_position_ms > duration_ms) start_position_ms = duration_ms;
    M4aAacReader m4a_aac_reader(file);
    bool m4a_frame_decoder = false;
    uint64_t m4a_seek_base_samples = 0;
    if (format == AudioFormat::kM4a && start_position_ms > 0) {
        m4a_frame_decoder = m4a_aac_reader.prepare_seek(
            start_position_ms, &m4a_seek_base_samples);
        if (!m4a_frame_decoder) audio_seek(file, 0, SEEK_SET);
    }

    esp_audio_simple_dec_handle_t decoder = nullptr;
    esp_audio_dec_handle_t opus_decoder = nullptr;
    esp_audio_dec_handle_t aac_decoder = nullptr;
    union SimpleDecoderConfig {
        esp_aac_dec_cfg_t aac;
        esp_m4a_dec_cfg_t m4a;
        esp_opus_dec_cfg_t opus;
    } simple_decoder_config{};
    if (format != AudioFormat::kAiff) {
        esp_audio_err_t decoder_ret = ESP_AUDIO_ERR_OK;
        if (m4a_frame_decoder) {
            simple_decoder_config.aac.sample_rate =
                static_cast<int32_t>(m4a_aac_reader.sample_rate);
            simple_decoder_config.aac.channel = m4a_aac_reader.channels;
            simple_decoder_config.aac.bits_per_sample = 16;
            simple_decoder_config.aac.no_adts_header = true;
            simple_decoder_config.aac.aac_plus_enable = true;
            decoder_ret = esp_aac_dec_open(&simple_decoder_config.aac,
                                           sizeof(simple_decoder_config.aac),
                                           &aac_decoder);
        } else if (format == AudioFormat::kOpus) {
            // Ogg pages have already been packetized by OggOpusReader.  The
            // simple decoder has no RAW_OPUS parser, so call the Opus frame
            // decoder directly with exactly one complete packet.
            simple_decoder_config.opus.sample_rate = ogg_opus_reader.sample_rate;
            simple_decoder_config.opus.channel = ogg_opus_reader.channels;
            simple_decoder_config.opus.frame_duration =
                ESP_OPUS_DEC_FRAME_DURATION_20_MS;
            simple_decoder_config.opus.self_delimited = false;
            decoder_ret = esp_opus_dec_open(&simple_decoder_config.opus,
                                            sizeof(simple_decoder_config.opus),
                                            &opus_decoder);
        } else {
            esp_audio_simple_dec_cfg_t decoder_config{};
            decoder_config.dec_type = decoder_type_for_format(format);
            decoder_config.use_frame_dec = false;
            if (format == AudioFormat::kAac) {
                simple_decoder_config.aac.aac_plus_enable = true;
                decoder_config.dec_cfg = &simple_decoder_config.aac;
                decoder_config.cfg_size = sizeof(simple_decoder_config.aac);
            } else if (format == AudioFormat::kM4a) {
                simple_decoder_config.m4a.aac_plus_enable = true;
                decoder_config.dec_cfg = &simple_decoder_config.m4a;
                decoder_config.cfg_size = sizeof(simple_decoder_config.m4a);
            }
            decoder_ret = esp_audio_simple_dec_open(&decoder_config, &decoder);
        }
        if (decoder_ret != ESP_AUDIO_ERR_OK ||
            (format == AudioFormat::kOpus ? opus_decoder == nullptr :
             m4a_frame_decoder ? aac_decoder == nullptr : decoder == nullptr)) {
            ESP_LOGE(kTag, "failed to open %s decoder: %d", audio_format_name(format),
                     static_cast<int>(decoder_ret));
            if (decoder) esp_audio_simple_dec_close(decoder);
            if (opus_decoder) esp_opus_dec_close(opus_decoder);
            if (aac_decoder) esp_aac_dec_close(aac_decoder);
            lyra::sd::close(file, lyra::sd::Client::Audio);
            set_error(ESP_FAIL, path);
            return ESP_FAIL;
        }
    }

    // The read-ahead ring is CPU-only; keep the smaller internal/DMA heap
    // available for the I2S staging buffer and the LCD transport allocator.
    auto *read_ahead_buffer = static_cast<uint8_t *>(heap_caps_malloc(
        kReadAheadBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (read_ahead_buffer == nullptr) {
        read_ahead_buffer = static_cast<uint8_t *>(heap_caps_malloc(
            kReadAheadBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    auto *pcm = format == AudioFormat::kAiff ? nullptr :
        static_cast<uint8_t *>(alloc_audio_buffer(kInitialPcmBufferBytes));
    auto *stereo = static_cast<int16_t *>(alloc_audio_dma_buffer(kStereoBufferBytes));
    size_t pcm_capacity = kInitialPcmBufferBytes;
    if (read_ahead_buffer == nullptr || (format != AudioFormat::kAiff && pcm == nullptr) ||
        stereo == nullptr) {
        ESP_LOGE(kTag, "not enough memory for %s buffers", audio_format_name(format));
        heap_caps_free(read_ahead_buffer);
        heap_caps_free(pcm);
        heap_caps_free(stereo);
        if (decoder) esp_audio_simple_dec_close(decoder);
        if (opus_decoder) esp_opus_dec_close(opus_decoder);
        if (aac_decoder) esp_aac_dec_close(aac_decoder);
        lyra::sd::close(file, lyra::sd::Client::Audio);
        set_error(ESP_ERR_NO_MEM, path);
        return ESP_ERR_NO_MEM;
    }

    const size_t read_ahead_chunk_bytes = format == AudioFormat::kOpus || m4a_frame_decoder ? 1 :
        format == AudioFormat::kM4a ? kM4aReadAheadChunkBytes :
        format == AudioFormat::kFlac ? kFlacReadAheadChunkBytes : kReadAheadChunkBytes;
    AudioReadAhead input{};
    input.file = file;
    input.buffer = read_ahead_buffer;
    input.capacity = kReadAheadBufferBytes;
    input.chunk_bytes = read_ahead_chunk_bytes;
    if (format == AudioFormat::kOpus) input.ogg_opus = &ogg_opus_reader;
    if (m4a_frame_decoder) input.m4a_aac = &m4a_aac_reader;
    if (format == AudioFormat::kFlac) {
        FlacStreamInfo stream_info{};
        if (read_flac_stream_info(file, &stream_info) &&
            prime_flac_decoder_input(&input, stream_info)) {
            // read_flac_stream_info leaves the file immediately before the
            // first audio frame, so the normal read-ahead loop now appends
            // frames to the synthetic, minimal FLAC header above.
            ESP_LOGD(kTag, "FLAC metadata skipped (%ld bytes before first frame)",
                     stream_info.first_frame_offset);
        } else {
            // Preserve the decoder's existing error path for malformed FLAC
            // files rather than attempting playback from an unknown offset.
            audio_seek(file, 0, SEEK_SET);
        }
    }
    if (format == AudioFormat::kAiff) {
        const size_t bytes_per_sample = aiff_info.bits_per_sample / 8u;
        const size_t bytes_per_frame = bytes_per_sample * aiff_info.channels;
        if (bytes_per_frame == 0 || audio_seek(file, aiff_info.data_start, SEEK_SET) != 0) {
            heap_caps_free(read_ahead_buffer);
            heap_caps_free(stereo);
            lyra::sd::close(file, lyra::sd::Client::Audio);
            set_error(ESP_ERR_INVALID_SIZE, path);
            return ESP_ERR_INVALID_SIZE;
        }
        input.bounded_source = true;
        input.source_little_endian = aiff_info.little_endian;
        input.source_bytes_per_sample = static_cast<uint8_t>(bytes_per_sample);
        input.source_channels = aiff_info.channels;
        input.source_remaining = aiff_info.data_bytes;
    }
    PcmOutput pcm_sink{};

    uint64_t discard_pcm_bytes = 0;
    bool replay_seek_pending = false;
    uint32_t replay_seek_position_ms = 0;
    uint32_t pcm_position_base_ms = start_position_ms;
    const uint64_t opus_pre_skip_bytes = format == AudioFormat::kOpus ?
        static_cast<uint64_t>(ogg_opus_reader.pre_skip) * ogg_opus_reader.channels *
        sizeof(int16_t) : 0;
    discard_pcm_bytes = opus_pre_skip_bytes;
    if (m4a_frame_decoder) {
        const uint64_t target_samples =
            (static_cast<uint64_t>(start_position_ms) * m4a_aac_reader.sample_rate) / 1000u;
        const uint64_t discard_samples = target_samples > m4a_seek_base_samples ?
            target_samples - m4a_seek_base_samples : 0;
        discard_pcm_bytes = discard_samples * m4a_aac_reader.channels * sizeof(int16_t);
        const uint64_t base_ms = (m4a_seek_base_samples * 1000u +
                                  m4a_aac_reader.sample_rate / 2u) /
                                 m4a_aac_reader.sample_rate;
        pcm_position_base_ms = base_ms > 0xFFFFFFFFu ? 0xFFFFFFFFu :
            static_cast<uint32_t>(base_ms);
        ESP_LOGI(kTag, "M4A seek %u ms using AAC sample table (base=%u ms discard=%llu bytes)",
                 static_cast<unsigned>(start_position_ms),
                 static_cast<unsigned>(pcm_position_base_ms),
                 static_cast<unsigned long long>(discard_pcm_bytes));
    }
    if (start_position_ms > 0) {
        if (format == AudioFormat::kMp3) {
            if (!seek_mp3_file(file, start_position_ms, duration_ms)) {
                audio_seek(file, 0, SEEK_SET);
                replay_seek_pending = true;
                replay_seek_position_ms = start_position_ms;
            }
        } else if (format == AudioFormat::kFlac) {
            // The simple FLAC decoder is a non-frame decoder, so provide it
            // with a minimal STREAMINFO header followed by a real frame near
            // the target. This avoids replaying the entire file for every seek.
            FlacStreamInfo stream_info{};
            if (read_flac_stream_info(file, &stream_info) && stream_info.sample_rate != 0 &&
                stream_info.channels != 0 &&
                (stream_info.bits_per_sample == 16 || stream_info.bits_per_sample == 24 ||
                 stream_info.bits_per_sample == 32)) {
                const uint64_t target_samples = std::min<uint64_t>(
                    stream_info.total_samples,
                    (static_cast<uint64_t>(start_position_ms) * stream_info.sample_rate) / 1000u);
                const uint8_t bytes_per_sample = stream_info.bits_per_sample / 8;
                long file_end = -1;
                long seek_frame = -1;
                uint64_t seek_base_samples = 0;
                bool have_fast_seek = false;

                if (audio_seek(file, 0, SEEK_END) == 0) file_end = std::ftell(file);
                if (stream_info.seek_sample != kNoFlacSeekSample && file_end > 0 &&
                    stream_info.seek_offset <= static_cast<uint64_t>(
                        file_end - stream_info.first_frame_offset)) {
                    seek_frame = stream_info.first_frame_offset +
                                 static_cast<long>(stream_info.seek_offset);
                    seek_base_samples = stream_info.seek_sample;
                    have_fast_seek = true;
                } else if (file_end > stream_info.first_frame_offset &&
                           stream_info.total_samples > 0) {
                    const uint64_t encoded_bytes = static_cast<uint64_t>(
                        file_end - stream_info.first_frame_offset);
                    const uint64_t estimated_delta = static_cast<uint64_t>(
                        (static_cast<double>(encoded_bytes) * target_samples) /
                        stream_info.total_samples);
                    const long estimated_frame = stream_info.first_frame_offset +
                        static_cast<long>(std::min<uint64_t>(estimated_delta, encoded_bytes));
                    have_fast_seek = find_flac_frame_offset(
                        file, estimated_frame, stream_info.first_frame_offset, file_end,
                        read_ahead_buffer, kReadAheadBufferBytes, &seek_frame);
                    // Without a seek table the frame scanner gives us a
                    // close encoded position, but not its exact sample number.
                    // Starting at that frame is preferable to replaying minutes
                    // of audio; the result is within one FLAC frame of target.
                    seek_base_samples = target_samples;
                }

                if (have_fast_seek && seek_frame >= stream_info.first_frame_offset &&
                    file_end > seek_frame && audio_seek(file, seek_frame, SEEK_SET) == 0 &&
                    prime_flac_decoder_input(&input, stream_info)) {
                    if (seek_base_samples < target_samples) {
                        discard_pcm_bytes = (target_samples - seek_base_samples) *
                                            stream_info.channels * bytes_per_sample;
                    }
                    ESP_LOGI(kTag, "FLAC seek %u ms using frame at %ld (discard=%llu bytes)",
                             static_cast<unsigned>(start_position_ms), seek_frame,
                             static_cast<unsigned long long>(discard_pcm_bytes));
                } else {
                    // Fallback for files with no usable seek point. This
                    // replays encoded frames from the beginning. Keep the
                    // synthetic STREAMINFO header so large metadata blocks do
                    // not re-enter the parser during that replay.
                    discard_pcm_bytes = target_samples * stream_info.channels *
                                        bytes_per_sample;
                    if (prime_flac_decoder_input(&input, stream_info) &&
                        audio_seek(file, stream_info.first_frame_offset, SEEK_SET) == 0) {
                        ESP_LOGI(kTag, "FLAC seek %u ms replaying from first frame",
                                 static_cast<unsigned>(start_position_ms));
                    } else {
                        start_position_ms = 0;
                        audio_seek(file, 0, SEEK_SET);
                    }
                }
            } else {
                start_position_ms = 0;
            }
        } else if (format == AudioFormat::kAiff) {
            const uint64_t target_frames = std::min<uint64_t>(
                aiff_info.data_bytes / (static_cast<size_t>(aiff_info.channels) *
                                        (aiff_info.bits_per_sample / 8u)),
                (static_cast<uint64_t>(start_position_ms) * aiff_info.sample_rate) / 1000u);
            const uint64_t target_bytes = target_frames * aiff_info.channels *
                                          (aiff_info.bits_per_sample / 8u);
            if (audio_seek(file, aiff_info.data_start + static_cast<long>(target_bytes),
                           SEEK_SET) == 0) {
                input.read_index = 0;
                input.write_index = 0;
                input.available = 0;
                input.eof = false;
                input.error = false;
                input.source_remaining = aiff_info.data_bytes - target_bytes;
                input.eof = input.source_remaining == 0;
            } else {
                start_position_ms = 0;
                audio_seek(file, aiff_info.data_start, SEEK_SET);
                input.source_remaining = aiff_info.data_bytes;
                input.eof = false;
            }
        } else if (format == AudioFormat::kAac) {
            if (!seek_aac_file(file, start_position_ms, duration_ms)) {
                audio_seek(file, 0, SEEK_SET);
                replay_seek_pending = true;
                replay_seek_position_ms = start_position_ms;
            }
        } else if (format == AudioFormat::kOpus) {
            const uint64_t target_samples =
                (static_cast<uint64_t>(start_position_ms) * ogg_opus_reader.sample_rate) /
                1000u + ogg_opus_reader.pre_skip;
            uint64_t base_samples = 0;
            if (ogg_opus_reader.seek_to_position(start_position_ms, duration_ms, &base_samples) &&
                target_samples >= base_samples) {
                const uint64_t base_playable_samples = base_samples > ogg_opus_reader.pre_skip ?
                    base_samples - ogg_opus_reader.pre_skip : 0;
                const uint64_t base_ms = (base_playable_samples * 1000u +
                                          ogg_opus_reader.sample_rate / 2u) /
                                         ogg_opus_reader.sample_rate;
                pcm_position_base_ms = base_ms > 0xFFFFFFFFu ? 0xFFFFFFFFu :
                    static_cast<uint32_t>(base_ms);
                discard_pcm_bytes = (target_samples - base_samples) *
                                    ogg_opus_reader.channels * sizeof(int16_t);
                ESP_LOGI(kTag, "OPUS seek %u ms using OGG page at %llu samples "
                         "(base=%u ms discard=%llu bytes)",
                         static_cast<unsigned>(start_position_ms),
                         static_cast<unsigned long long>(base_samples),
                         static_cast<unsigned>(pcm_position_base_ms),
                         static_cast<unsigned long long>(discard_pcm_bytes));
            } else {
                ogg_opus_reader.reset();
                audio_seek(file, 0, SEEK_SET);
                replay_seek_pending = true;
                replay_seek_position_ms = start_position_ms;
            }
        } else if ((format == AudioFormat::kM4a && !m4a_frame_decoder) ||
                   format == AudioFormat::kWav ||
                   format == AudioFormat::kOgg) {
            // Espressif's simple decoders are streaming parsers and do not
            // expose a seek API. Restarting the parser and discarding decoded
            // PCM keeps seeking correct for containers whose headers must be
            // replayed from byte zero.
            audio_seek(file, 0, SEEK_SET);
            replay_seek_pending = true;
            replay_seek_position_ms = start_position_ms;
        }
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_diagnostics = {};
    s_diagnostics.audio_buffer_low_watermark = input.capacity;
    s_diagnostics.pcm_buffer_low_watermark = kPcmOutputBufferBytes;
    s_diagnostics.read_ahead_internal = buffer_is_internal(input.buffer);
    s_diagnostics.pcm_internal = buffer_is_internal(pcm);
    s_diagnostics.stereo_internal = buffer_is_internal(stereo);
    s_status.playing = true;
    s_status.paused = start_paused;
    s_status.eof = false;
    s_status.last_error = ESP_OK;
    s_status.sample_rate = 0;
    s_status.channels = 0;
    s_status.bits_per_sample = 0;
    s_status.decoded_bytes = 0;
    s_status.position_ms = start_position_ms;
    s_status.duration_ms = duration_ms;
    std::strncpy(s_status.path, path, sizeof(s_status.path) - 1);
    s_status.path[sizeof(s_status.path) - 1] = '\0';
    xSemaphoreGive(s_state_mutex);

    bool have_audio_info = false;
    esp_err_t result = ESP_OK;
    uint32_t no_progress_count = 0;
    uint32_t replay_yield_count = 0;
    bool pause_after_seek_pending = pause_after_seek;

    while (generation_is_current(generation)) {
        if (!read_pause_state(generation)) break;

        // Keep the ring mostly full while the decoder/I2S path is active.
        // Every iteration is still one bounded SD transaction, so artwork
        // gets opportunities between audio refills but cannot run ahead of it.
        while (!input.eof &&
               input.available < input.capacity - input.chunk_bytes) {
            const size_t before = input.available;
            if (!input.fill()) {
                if (input.error) {
                    result = ESP_FAIL;
                    ESP_LOGE(kTag, "read error while playing %s", path);
                    break;
                }
                break;
            }
            if (input.available == before) break;
        }
        if (result != ESP_OK) break;
        if (input.available == 0 && input.eof) break;
        if (input.available == 0) {
            ++s_diagnostics.underrun_count;
            vTaskDelay(1);
            continue;
        }

        esp_audio_simple_dec_raw_t raw{};
        raw.buffer = input.buffer + input.read_index;
        raw.len = static_cast<uint32_t>(format == AudioFormat::kOpus || m4a_frame_decoder ?
            input.contiguous() : std::min(input.contiguous(), kSimpleDecoderInputChunkBytes));
        raw.eos = input.eof && input.available == input.contiguous() &&
                  input.available <= kSimpleDecoderInputChunkBytes;

        while (raw.len > 0 && generation_is_current(generation)) {
            if (format == AudioFormat::kAiff) {
                const size_t bytes_per_frame = static_cast<size_t>(aiff_info.channels) *
                                               (aiff_info.bits_per_sample / 8u);
                size_t pcm_bytes = raw.len;
                pcm_bytes -= pcm_bytes % bytes_per_frame;
                if (pcm_bytes == 0) break;

                if (!have_audio_info) {
                    result = configure_i2s(aiff_info.sample_rate);
                    pcm_sink.sample_rate = aiff_info.sample_rate;
                    if (result != ESP_OK || !start_pcm_output(&pcm_sink)) {
                        ESP_LOGE(kTag, "AIFF I2S output setup failed for %s", path);
                        result = result == ESP_OK ? ESP_ERR_NO_MEM : result;
                        break;
                    }
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.sample_rate = aiff_info.sample_rate;
                    s_status.channels = aiff_info.channels;
                    s_status.bits_per_sample = aiff_info.bits_per_sample;
                    xSemaphoreGive(s_state_mutex);
                    ESP_LOGI(kTag, "playing %s (%u Hz, %u channel, %u-bit)", path,
                             static_cast<unsigned>(aiff_info.sample_rate),
                             static_cast<unsigned>(aiff_info.channels),
                             static_cast<unsigned>(aiff_info.bits_per_sample));
                    have_audio_info = true;
                }

                result = write_pcm(raw.buffer, pcm_bytes, aiff_info.channels,
                                   aiff_info.bits_per_sample, stereo, kStereoBufferBytes,
                                   &pcm_sink, generation);
                if (result != ESP_OK) break;
                input.consume(pcm_bytes);
                raw.buffer += pcm_bytes;
                raw.len -= static_cast<uint32_t>(pcm_bytes);
                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_status.decoded_bytes += pcm_bytes;
                const uint32_t decoded_position_ms = pcm_bytes_to_milliseconds(
                    s_status.decoded_bytes, s_status.sample_rate, s_status.channels,
                    s_status.bits_per_sample);
                const uint64_t absolute_position_ms =
                    static_cast<uint64_t>(pcm_position_base_ms) + decoded_position_ms;
                s_status.position_ms = absolute_position_ms > 0xFFFFFFFFu ?
                    0xFFFFFFFFu : static_cast<uint32_t>(absolute_position_ms);
                if (s_status.duration_ms > 0 && s_status.position_ms > s_status.duration_ms) {
                    s_status.position_ms = s_status.duration_ms;
                }
                xSemaphoreGive(s_state_mutex);
                if (pause_after_seek_pending) {
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.paused = true;
                    xSemaphoreGive(s_state_mutex);
                    pause_after_seek_pending = false;
                    break;
                }
                no_progress_count = 0;
                continue;
            }

            esp_audio_simple_dec_out_t frame{};
            frame.buffer = pcm;
            frame.len = static_cast<uint32_t>(pcm_capacity);
            esp_audio_dec_info_t direct_info{};
            esp_audio_err_t decode_ret = ESP_AUDIO_ERR_OK;
            if (format == AudioFormat::kOpus) {
                esp_audio_dec_in_raw_t opus_raw{};
                opus_raw.buffer = raw.buffer;
                opus_raw.len = raw.len;
                esp_audio_dec_out_frame_t opus_frame{};
                opus_frame.buffer = pcm;
                opus_frame.len = static_cast<uint32_t>(pcm_capacity);
                decode_ret = esp_opus_dec_decode(opus_decoder, &opus_raw,
                                                 &opus_frame, &direct_info);
                raw.consumed = opus_raw.consumed;
                frame.needed_size = opus_frame.needed_size;
                frame.decoded_size = opus_frame.decoded_size;
            } else if (m4a_frame_decoder) {
                esp_audio_dec_in_raw_t aac_raw{};
                aac_raw.buffer = raw.buffer;
                aac_raw.len = raw.len;
                esp_audio_dec_out_frame_t aac_frame{};
                aac_frame.buffer = pcm;
                aac_frame.len = static_cast<uint32_t>(pcm_capacity);
                decode_ret = esp_aac_dec_decode(aac_decoder, &aac_raw,
                                                &aac_frame, &direct_info);
                raw.consumed = aac_raw.consumed;
                frame.needed_size = aac_frame.needed_size;
                frame.decoded_size = aac_frame.decoded_size;
            } else {
                decode_ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            }
            if (decode_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > pcm_capacity &&
                frame.needed_size <= kMaximumPcmBufferBytes) {
                auto *larger = static_cast<uint8_t *>(alloc_audio_buffer(frame.needed_size));
                if (larger == nullptr) {
                    result = ESP_ERR_NO_MEM;
                    break;
                }
                heap_caps_free(pcm);
                pcm = larger;
                pcm_capacity = frame.needed_size;
                s_diagnostics.pcm_internal = buffer_is_internal(pcm);
                continue;
            }
            if (decode_ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(kTag, "%s decode error %d in %s", audio_format_name(format),
                         static_cast<int>(decode_ret), path);
                result = ESP_FAIL;
                break;
            }

            if (raw.consumed > raw.len) {
                result = ESP_FAIL;
                ESP_LOGE(kTag, "%s decoder consumed beyond input buffer",
                         audio_format_name(format));
                break;
            }

            if (raw.consumed > 0) input.consume(raw.consumed);

            if (frame.decoded_size > 0) {
                esp_audio_simple_dec_info_t info{};
                if (!have_audio_info) {
                    esp_audio_err_t info_ret = ESP_AUDIO_ERR_OK;
                    if (format == AudioFormat::kOpus || m4a_frame_decoder) {
                        info.sample_rate = direct_info.sample_rate;
                        info.bits_per_sample = direct_info.bits_per_sample;
                        info.channel = direct_info.channel;
                        info.bitrate = direct_info.bitrate;
                        info.frame_size = direct_info.frame_size;
                        if (info.sample_rate == 0 || info.channel == 0) {
                            info_ret = ESP_AUDIO_ERR_NOT_FOUND;
                        }
                    } else {
                        info_ret = esp_audio_simple_dec_get_info(decoder, &info);
                    }
                    const bool supported_bits = info.bits_per_sample == 8 ||
                                                 info.bits_per_sample == 16 ||
                                                 info.bits_per_sample == 24 ||
                                                 info.bits_per_sample == 32;
                    if (info_ret != ESP_AUDIO_ERR_OK || !supported_bits ||
                        info.channel == 0 || info.channel > 8) {
                        ESP_LOGE(kTag, "unsupported %s PCM format in %s: ret=%d rate=%u "
                                 "channels=%u bits=%u frame=%u",
                                 audio_format_name(format), path, static_cast<int>(info_ret),
                                 static_cast<unsigned>(info.sample_rate),
                                 static_cast<unsigned>(info.channel),
                                 static_cast<unsigned>(info.bits_per_sample),
                                 static_cast<unsigned>(info.frame_size));
                        result = ESP_ERR_NOT_SUPPORTED;
                        break;
                    }
                    result = configure_i2s(info.sample_rate);
                    if (result != ESP_OK) {
                        ESP_LOGE(kTag, "I2S setup failed: %s", esp_err_to_name(result));
                        break;
                    }
                    pcm_sink.sample_rate = info.sample_rate;
                    if (!start_pcm_output(&pcm_sink)) {
                        ESP_LOGE(kTag, "I2S PCM output buffer allocation failed");
                        result = ESP_ERR_NO_MEM;
                        break;
                    }
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.sample_rate = info.sample_rate;
                    s_status.channels = info.channel;
                    s_status.bits_per_sample = info.bits_per_sample;
                    if (duration_ms == 0 && file_end > 0 && info.bitrate > 0) {
                        const uint64_t estimated_duration =
                            (static_cast<uint64_t>(file_end) * 8u * 1000u) / info.bitrate;
                        duration_ms = estimated_duration > 0xFFFFFFFFu ? 0xFFFFFFFFu :
                            static_cast<uint32_t>(estimated_duration);
                        s_status.duration_ms = duration_ms;
                    }
                    xSemaphoreGive(s_state_mutex);
                    if (replay_seek_pending) {
                        uint64_t target_samples =
                            (static_cast<uint64_t>(replay_seek_position_ms) * info.sample_rate) / 1000u;
                        if (format == AudioFormat::kOpus) target_samples += ogg_opus_reader.pre_skip;
                        discard_pcm_bytes = target_samples * info.channel *
                                            (info.bits_per_sample / 8u);
                        replay_seek_pending = false;
                        ESP_LOGI(kTag, "%s seek %u ms replaying and discarding %llu bytes",
                                 audio_format_name(format),
                                 static_cast<unsigned>(replay_seek_position_ms),
                                 static_cast<unsigned long long>(discard_pcm_bytes));
                    }
                    ESP_LOGI(kTag, "playing %s (%u Hz, %u channel, %u-bit)", path,
                             static_cast<unsigned>(info.sample_rate),
                             static_cast<unsigned>(info.channel),
                             static_cast<unsigned>(info.bits_per_sample));
                    have_audio_info = true;
                } else {
                    if (format == AudioFormat::kOpus || m4a_frame_decoder) {
                        info.sample_rate = direct_info.sample_rate;
                        info.bits_per_sample = direct_info.bits_per_sample;
                        info.channel = direct_info.channel;
                        info.bitrate = direct_info.bitrate;
                        info.frame_size = direct_info.frame_size;
                    } else {
                        if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK) {
                            result = ESP_FAIL;
                            break;
                        }
                    }
                }

                const uint8_t *pcm_output = frame.buffer;
                size_t pcm_output_bytes = frame.decoded_size;
                if (discard_pcm_bytes > 0) {
                    const size_t discard = static_cast<size_t>(std::min<uint64_t>(
                        discard_pcm_bytes, pcm_output_bytes));
                    discard_pcm_bytes -= discard;
                    pcm_output += discard;
                    pcm_output_bytes -= discard;
                }
                if (pcm_output_bytes > 0) {
                    result = write_pcm(pcm_output, pcm_output_bytes, info.channel,
                                       info.bits_per_sample,
                                       stereo, kStereoBufferBytes,
                                       &pcm_sink, generation, format == AudioFormat::kWav);
                    if (result != ESP_OK) break;
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.decoded_bytes += pcm_output_bytes;
                    const uint32_t decoded_position_ms = pcm_bytes_to_milliseconds(
                        s_status.decoded_bytes, s_status.sample_rate, s_status.channels,
                        s_status.bits_per_sample);
                    const uint64_t absolute_position_ms =
                        static_cast<uint64_t>(pcm_position_base_ms) + decoded_position_ms;
                    s_status.position_ms = absolute_position_ms > 0xFFFFFFFFu ?
                        0xFFFFFFFFu : static_cast<uint32_t>(absolute_position_ms);
                    if (s_status.duration_ms > 0 && s_status.position_ms > s_status.duration_ms) {
                        s_status.position_ms = s_status.duration_ms;
                    }
                    xSemaphoreGive(s_state_mutex);
                }
                if (pause_after_seek_pending && discard_pcm_bytes == 0) {
                    // A seek issued while paused must decode through the
                    // requested position before pausing again. Otherwise the
                    // normal pause gate stops the FLAC replay at byte zero.
                    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                    s_status.paused = true;
                    xSemaphoreGive(s_state_mutex);
                    pause_after_seek_pending = false;
                    break;
                }
                if (replay_seek_pending || discard_pcm_bytes > 0) {
                    // AAC replay can require millions of bytes to be decoded
                    // before the requested position.  Yield often enough for
                    // LVGL, but not once per compressed frame.
                    if ((++replay_yield_count & 0x0Fu) == 0) vTaskDelay(1);
                }
                no_progress_count = 0;
            } else if (raw.consumed == 0) {
                ++no_progress_count;
                // A decoder may need the next read-ahead segment to finish a
                // frame. Leave the inner loop and let the outer loop refill
                // the ring instead of spinning on a short SD read.
                if (!input.eof && input.available < input.capacity) {
                    if (!input.fill()) {
                        if (input.error) {
                            result = ESP_FAIL;
                            ESP_LOGE(kTag, "read error while playing %s", path);
                            break;
                        }
                    }
                    break;
                }
                if (no_progress_count > 3) {
                    ESP_LOGE(kTag, "%s decoder made no progress in %s",
                             audio_format_name(format), path);
                    result = ESP_FAIL;
                    break;
                }
                vTaskDelay(1);
            } else {
                no_progress_count = 0;
                if (replay_seek_pending) vTaskDelay(1);
            }

            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
        }

        if (result != ESP_OK) break;
    }

    const bool request_still_current = generation_is_current(generation);
    stop_pcm_output(&pcm_sink, request_still_current && result == ESP_OK, generation);
    if (pcm_sink.error && result == ESP_OK) result = pcm_sink.error_code;

    if (!generation_is_current(generation)) {
        if (s_i2s_started) {
            i2s_channel_disable(s_i2s_tx);
            s_i2s_started = false;
        }
        disable_speaker_i2s();
        result = ESP_OK;
    } else if (result == ESP_OK) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_status.playing = false;
        s_status.paused = false;
        s_status.eof = true;
        if (s_status.duration_ms > 0) s_status.position_ms = s_status.duration_ms;
        xSemaphoreGive(s_state_mutex);
        ESP_LOGI(kTag, "finished %s", path);
    } else {
        if (s_i2s_started) {
            i2s_channel_disable(s_i2s_tx);
            s_i2s_started = false;
        }
        disable_speaker_i2s();
        set_error(result, path);
    }

    s_diagnostics.average_sd_read_us = s_diagnostics.sd_read_count == 0 ? 0 :
        static_cast<uint32_t>(s_diagnostics.total_sd_read_us /
                              s_diagnostics.sd_read_count);
    s_diagnostics.average_sd_lock_wait_us = s_diagnostics.sd_read_count == 0 ? 0 :
        static_cast<uint32_t>(s_diagnostics.total_sd_lock_wait_us /
                              s_diagnostics.sd_read_count);
    ESP_LOGI(kTag, "read-ahead: max=%u us avg=%u us lock-max=%u us lock-avg=%u us "
             "low=%u bytes underruns=%u; pcm-low=%u bytes pcm-underruns=%u; "
             "buffers read-ahead=%s pcm=%s stereo=%s",
             static_cast<unsigned>(s_diagnostics.max_sd_read_us),
             static_cast<unsigned>(s_diagnostics.average_sd_read_us),
             static_cast<unsigned>(s_diagnostics.max_sd_lock_wait_us),
             static_cast<unsigned>(s_diagnostics.average_sd_lock_wait_us),
             static_cast<unsigned>(s_diagnostics.audio_buffer_low_watermark),
             static_cast<unsigned>(s_diagnostics.underrun_count),
             static_cast<unsigned>(s_diagnostics.pcm_buffer_low_watermark),
             static_cast<unsigned>(s_diagnostics.pcm_underrun_count),
             s_diagnostics.read_ahead_internal ? "internal" : "psram",
             s_diagnostics.pcm_internal ? "internal" : "psram",
             s_diagnostics.stereo_internal ? "internal" : "psram");

    heap_caps_free(read_ahead_buffer);
    heap_caps_free(pcm);
    heap_caps_free(stereo);
    if (decoder) esp_audio_simple_dec_close(decoder);
    if (opus_decoder) esp_opus_dec_close(opus_decoder);
    if (aac_decoder) esp_aac_dec_close(aac_decoder);
    lyra::sd::close(file, lyra::sd::Client::Audio);
    return result;
}

void audio_task(void *)
{
    uint32_t handled_generation = 0;
    while (true) {
        char path[lyra::audio::kMaxPath];
        uint32_t generation;
        uint32_t seek_position_ms;
        bool paused;
        bool pause_after_seek;
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        generation = s_request_generation;
        seek_position_ms = s_requested_seek_ms;
        paused = s_status.paused;
        pause_after_seek = s_requested_pause_after_seek;
        std::strncpy(path, s_requested_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        xSemaphoreGive(s_state_mutex);

        if (generation == handled_generation) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        handled_generation = generation;
        if (!path[0]) {
            if (s_i2s_started) {
                i2s_channel_disable(s_i2s_tx);
                s_i2s_started = false;
            }
            disable_speaker_i2s();
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            s_status.playing = false;
            s_status.paused = false;
            s_status.eof = false;
            s_status.position_ms = 0;
            s_status.duration_ms = 0;
            s_status.path[0] = '\0';
            xSemaphoreGive(s_state_mutex);
            continue;
        }
        play_file(path, generation, seek_position_ms, paused, pause_after_seek);
    }
}

} // namespace lyra::audio::internal
