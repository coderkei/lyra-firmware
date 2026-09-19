/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

void format_file_date(uint64_t modified_time, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    if (modified_time == 0) {
        copy_ui_text(output, capacity, tr(lyra::i18n::StringId::Unavailable));
        return;
    }
    const std::time_t timestamp = static_cast<std::time_t>(modified_time);
    std::tm local{};
    const auto language = lyra::i18n::current_language();
    const char *date_format = "%Y-%m-%d %H:%M";
    if (language == lyra::i18n::Language::German) {
        date_format = "%d.%m.%Y %H:%M";
    } else if (language == lyra::i18n::Language::French ||
               language == lyra::i18n::Language::Spanish ||
               language == lyra::i18n::Language::Italian ||
               language == lyra::i18n::Language::Russian) {
        date_format = "%d/%m/%Y %H:%M";
    } else if (language == lyra::i18n::Language::Japanese ||
               language == lyra::i18n::Language::Korean ||
               language == lyra::i18n::Language::SimplifiedChinese ||
               language == lyra::i18n::Language::TraditionalChinese) {
        date_format = "%Y/%m/%d %H:%M";
    }
    if (localtime_r(&timestamp, &local) == nullptr ||
        std::strftime(output, capacity, date_format, &local) == 0) {
        copy_ui_text(output, capacity, tr(lyra::i18n::StringId::Unavailable));
    }
}

void format_track_format(const lyra::media::Track &track, char *output, size_t capacity)
{
    copy_ui_text(output, capacity, track.format);
    // File-format identifiers are user/media data. Preserve their spelling
    // instead of applying an English-only casing transformation.
    if (!output[0]) copy_ui_text(output, capacity, tr(lyra::i18n::StringId::Unknown));
}

const char *track_encoding(const lyra::media::Track &track)
{
    if (text_equals_ci(track.format, "mp3")) return tr(lyra::i18n::StringId::EncodingMpegLayer3);
    if (text_equals_ci(track.format, "flac")) return tr(lyra::i18n::StringId::EncodingFlac);
    if (text_equals_ci(track.format, "aac")) return tr(lyra::i18n::StringId::EncodingAac);
    if (text_equals_ci(track.format, "m4a") || text_equals_ci(track.format, "mp4")) {
        return tr(lyra::i18n::StringId::EncodingMpeg4);
    }
    if (text_equals_ci(track.format, "ogg")) return tr(lyra::i18n::StringId::EncodingVorbisOpus);
    if (text_equals_ci(track.format, "opus")) return tr(lyra::i18n::StringId::EncodingOpus);
    if (text_equals_ci(track.format, "wav") || text_equals_ci(track.format, "aiff") ||
        text_equals_ci(track.format, "aif") || text_equals_ci(track.format, "aifc")) {
        return tr(lyra::i18n::StringId::EncodingPcm);
    }
    return tr(lyra::i18n::StringId::Unknown);
}

void render_track_info()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) {
        make_header(tr(lyra::i18n::StringId::TrackInfo), View::Player, true);
        lv_obj_t *empty = make_label(s_screen, tr(lyra::i18n::StringId::NoTrackSelectedPeriod), kTextMuted);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);
        return;
    }

    make_header(tr(lyra::i18n::StringId::TrackInfo), View::Player, true);
    make_track_info_tab_button(s_screen, 8, tr(lyra::i18n::StringId::SongInfo), TrackInfoTab::Song);
    make_track_info_tab_button(s_screen, 162, tr(lyra::i18n::StringId::MediaInfo), TrackInfoTab::Media);
    lv_obj_t *body = make_scroll_body(118);

    if (s_track_info_tab == TrackInfoTab::Song) {
        constexpr int kArtSize = 116;
        const int art_x = (kScreenWidth - kArtSize) / 2;
        make_album_art(body, art_x, 8, kArtSize, kArtSize, track);
        lv_obj_t *art_hit = make_button(body, art_x, 8, kArtSize, kArtSize, kBackground, 13, false);
        lv_obj_set_style_bg_opa(art_hit, LV_OPA_TRANSP, 0);
        add_route(art_hit, View::FullscreenInfoArt);
        lv_obj_t *hint = make_label(body, tr(lyra::i18n::StringId::TapAlbumArt), kTextMuted);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 128);

        int y = 150;
        make_track_info_row(body, y, tr(lyra::i18n::StringId::Title), track.title); y += 56;
        lv_obj_t *album = make_track_info_row(body, y, tr(lyra::i18n::StringId::Album), track.album, true);
        lv_obj_add_event_cb(album, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Album)));
        y += 56;
        lv_obj_t *artist = make_track_info_row(body, y, tr(lyra::i18n::StringId::Artist), track.artist, true);
        lv_obj_add_event_cb(artist, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Artist)));
        y += 56;
        make_track_info_row(body, y, tr(lyra::i18n::StringId::AlbumArtist), track.album_artist); y += 56;
        make_track_info_row(body, y, tr(lyra::i18n::StringId::Composer), track.composer); y += 56;
        lv_obj_t *genre = make_track_info_row(body, y, tr(lyra::i18n::StringId::Genre), track.genre, true);
        lv_obj_add_event_cb(genre, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Genre)));
        y += 56;
        char number[16];
        format_track_number(track.track_number, number, sizeof(number));
        make_track_info_row(body, y, tr(lyra::i18n::StringId::TrackNumber), number); y += 56;
        format_track_number(track.disc_number, number, sizeof(number));
        make_track_info_row(body, y, tr(lyra::i18n::StringId::DiscNumber), number); y += 56;
        lv_obj_t *year = make_track_info_row(body, y, tr(lyra::i18n::StringId::Year), track.year, true);
        lv_obj_add_event_cb(year, open_current_track_group_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::media::GroupKind::Year)));
        return;
    }

    const lyra::audio::Status audio_status = lyra::audio::status();
    const bool current_audio = std::strcmp(audio_status.path, track.path) == 0;
    const uint32_t duration_ms = current_audio && audio_status.duration_ms > 0 ?
                                 audio_status.duration_ms : track.duration_ms;
    char filename[lyra::media::kMaxPath];
    const char *slash = std::strrchr(track.path, '/');
    copy_ui_text(filename, sizeof(filename), slash ? slash + 1 : track.path);
    char duration[16];
    format_playback_time(duration_ms, duration_ms == 0, duration, sizeof(duration));
    char bitrate[24];
    if (duration_ms == 0 || track.size_bytes == 0) {
        copy_ui_text(bitrate, sizeof(bitrate), tr(lyra::i18n::StringId::Unavailable));
    } else {
        format_u64(lyra::i18n::StringId::BitrateAverage,
                   (track.size_bytes * 8u + duration_ms / 2u) / duration_ms,
                   bitrate, sizeof(bitrate));
    }
    char sample_rate[24];
    if (current_audio && audio_status.sample_rate > 0) {
        format_u32(lyra::i18n::StringId::SampleRateValue,
                   static_cast<uint32_t>(audio_status.sample_rate), sample_rate, sizeof(sample_rate));
    } else copy_ui_text(sample_rate, sizeof(sample_rate), tr(lyra::i18n::StringId::Unavailable));
    char bits_per_sample[16];
    if (current_audio && audio_status.bits_per_sample > 0) {
        format_u32(lyra::i18n::StringId::BitsPerSampleValue,
                   static_cast<uint32_t>(audio_status.bits_per_sample),
                   bits_per_sample, sizeof(bits_per_sample));
    } else copy_ui_text(bits_per_sample, sizeof(bits_per_sample), tr(lyra::i18n::StringId::Unavailable));
    char channels[24];
    if (current_audio && audio_status.channels > 0) {
        format_u32_text(lyra::i18n::StringId::ChannelValue,
                        static_cast<uint32_t>(audio_status.channels),
                        audio_status.channels == 1 ? tr(lyra::i18n::StringId::Mono) :
                        audio_status.channels == 2 ? tr(lyra::i18n::StringId::Stereo) :
                        tr(lyra::i18n::StringId::Multichannel), channels, sizeof(channels));
    } else copy_ui_text(channels, sizeof(channels), tr(lyra::i18n::StringId::Unavailable));
    char size[24];
    format_file_size(track.size_bytes, size, sizeof(size));
    char file_date[32];
    format_file_date(track.modified_time, file_date, sizeof(file_date));
    char format[16];
    format_track_format(track, format, sizeof(format));

    int y = 8;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::Filename), filename); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::FilePath), track.path); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::Duration), duration); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::Bitrate), bitrate); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::SampleRate), sample_rate); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::BitsPerSample), bits_per_sample); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::Format), format); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::Encoding), track_encoding(track)); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::Channels), channels); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::FileSize), size); y += 56;
    make_track_info_row(body, y, tr(lyra::i18n::StringId::FileDateInfo), file_date);
}

void render_fullscreen_info_art()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) {
        render(View::TrackInfo);
        return;
    }
    make_artwork(s_screen, 0, 0, kScreenWidth, kScreenHeight, track, 0, true);
    lv_obj_t *exit_hit = make_button(s_screen, 0, 0, kScreenWidth, kScreenHeight, kBackground, 0, false);
    lv_obj_set_style_bg_opa(exit_hit, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(exit_hit, [](lv_event_t *) { navigate_back(View::TrackInfo); },
                        LV_EVENT_CLICKED, nullptr);
}

} // namespace lyra::gui::internal
