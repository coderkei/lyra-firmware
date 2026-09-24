/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {

namespace {

constexpr int64_t kNavLongPressThresholdUs = 2'000'000;
constexpr int64_t kNavSeekRateTierDurationUs = 5'000'000;
constexpr uint32_t kPreviousRestartThresholdMs = 5'000;
constexpr uint32_t kNavSeekRateTier1 = 10;
constexpr uint32_t kNavSeekRateTier2 = 30;
constexpr uint32_t kNavSeekRateTier3 = 50;
constexpr uint32_t kNavSeekRateTier4 = 100;

struct NavHoldState {
    bool pressed;
    bool long_press_handled;
    bool seek_active;
    bool was_paused;
    bool temporary_pause_succeeded;
    int64_t pressed_at_us;
    int64_t seek_started_at_us;
    int64_t last_preview_update_us;
    uint32_t base_position_ms;
    uint32_t target_position_ms;
    uint32_t duration_ms;
    char track_path[lyra::audio::kMaxPath];
};

NavHoldState s_previous_hold;
NavHoldState s_next_hold;

uint64_t nav_seek_distance_ms(int64_t from_us, int64_t to_us,
                              int64_t seek_started_at_us)
{
    const int64_t from_elapsed_us = std::max<int64_t>(0, from_us - seek_started_at_us);
    const int64_t to_elapsed_us = std::max<int64_t>(0, to_us - seek_started_at_us);
    if (to_elapsed_us <= from_elapsed_us) return 0;

    const auto distance_in_segment = [from_elapsed_us, to_elapsed_us](
        int64_t segment_start_us, int64_t segment_end_us, uint32_t rate) {
        const int64_t overlap_start_us = std::max(from_elapsed_us, segment_start_us);
        const int64_t overlap_end_us = std::min(to_elapsed_us, segment_end_us);
        if (overlap_end_us <= overlap_start_us) return uint64_t{0};
        return static_cast<uint64_t>(overlap_end_us - overlap_start_us) * rate / 1000u;
    };

    const int64_t tier_us = kNavSeekRateTierDurationUs;
    return distance_in_segment(0, tier_us, kNavSeekRateTier1) +
           distance_in_segment(tier_us, tier_us * 2, kNavSeekRateTier2) +
           distance_in_segment(tier_us * 2, tier_us * 3, kNavSeekRateTier3) +
           distance_in_segment(tier_us * 3, to_elapsed_us, kNavSeekRateTier4);
}

uint32_t nav_seek_target(uint32_t base_position_ms, uint64_t distance_ms,
                         uint32_t duration_ms, int direction)
{
    const uint32_t maximum_position_ms = duration_ms > 250 ? duration_ms - 250 : 0;
    if (direction > 0) {
        return static_cast<uint32_t>(std::min<uint64_t>(
            maximum_position_ms, static_cast<uint64_t>(base_position_ms) + distance_ms));
    }
    return distance_ms >= base_position_ms ? 0 :
        base_position_ms - static_cast<uint32_t>(distance_ms);
}

void update_nav_hold(NavHoldState &hold, int64_t now_us, int direction, bool force)
{
    if (!hold.pressed) return;

    if (!hold.long_press_handled && now_us - hold.pressed_at_us >= kNavLongPressThresholdUs) {
        hold.long_press_handled = true;
        if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) return;

        const lyra::audio::Status audio_status = lyra::audio::status();
        if (!hold.track_path[0] || std::strcmp(audio_status.path, hold.track_path) != 0 ||
            audio_status.last_error != ESP_OK || audio_status.duration_ms == 0) return;

        hold.seek_active = true;
        hold.was_paused = audio_status.paused;
        hold.duration_ms = audio_status.duration_ms;
        const uint32_t maximum_position_ms = audio_status.duration_ms > 250 ?
            audio_status.duration_ms - 250 : 0;
        hold.base_position_ms = std::min(audio_status.position_ms, maximum_position_ms);
        hold.target_position_ms = hold.base_position_ms;
        hold.seek_started_at_us = hold.pressed_at_us + kNavLongPressThresholdUs;
        if (!hold.was_paused) {
            const esp_err_t pause_ret = lyra::audio::toggle_pause();
            hold.temporary_pause_succeeded = pause_ret == ESP_OK;
            if (pause_ret != ESP_OK) {
                ESP_LOGW(kTag, "could not pause playback for seeking: %s",
                         esp_err_to_name(pause_ret));
            }
        }
    }

    if (!hold.seek_active) return;
    const uint64_t distance_ms = nav_seek_distance_ms(
        hold.seek_started_at_us, now_us, hold.seek_started_at_us);
    const uint32_t target_ms = nav_seek_target(
        hold.base_position_ms, distance_ms, hold.duration_ms, direction);
    hold.target_position_ms = target_ms;

    const lyra::audio::Status audio_status = lyra::audio::status();
    if (std::strcmp(audio_status.path, hold.track_path) != 0) {
        update_player_progress_seek_preview(0, 0, false);
        hold.seek_active = false;
        return;
    }

    if (!force) {
        if (now_us - hold.last_preview_update_us >= 100'000) {
            update_player_progress_seek_preview(target_ms, hold.duration_ms, true);
            hold.last_preview_update_us = now_us;
        }
        return;
    }

    esp_err_t seek_ret = ESP_OK;
    if (hold.was_paused) {
        if (target_ms != hold.base_position_ms) seek_ret = lyra::audio::seek(target_ms);
    } else {
        seek_ret = lyra::audio::play_from_position(hold.track_path, target_ms, false);
        if (seek_ret != ESP_OK && hold.temporary_pause_succeeded) {
            const esp_err_t resume_ret = lyra::audio::toggle_pause();
            if (resume_ret != ESP_OK) {
                ESP_LOGW(kTag, "could not resume playback after failed seek: %s",
                         esp_err_to_name(resume_ret));
            }
        }
    }
    if (seek_ret != ESP_OK) {
        ESP_LOGW(kTag, "could not seek playback: %s", esp_err_to_name(seek_ret));
    }
    update_player_progress_seek_preview(0, 0, false);
}

void request_manual_previous()
{
    if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) return;

    const lyra::audio::Status audio_status = lyra::audio::status();
    size_t queue_position = 0;
    const bool at_queue_start = s_has_active_queue &&
        current_queue_position(&queue_position) && queue_position == 0;
    if (audio_status.position_ms > kPreviousRestartThresholdMs || at_queue_start) {
        restart_current_track();
        return;
    }

    request_manual_queue_move(-1);
}

void nav_button_event(lv_event_t *event, int direction, NavHoldState &hold)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        hold = {};
        hold.pressed = true;
        hold.pressed_at_us = esp_timer_get_time();
        const lyra::audio::Status audio_status = lyra::audio::status();
        std::strncpy(hold.track_path, audio_status.path, sizeof(hold.track_path) - 1);
        hold.track_path[sizeof(hold.track_path) - 1] = '\0';
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        update_nav_hold(hold, esp_timer_get_time(), direction, false);
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (!hold.pressed) return;
        update_nav_hold(hold, esp_timer_get_time(), direction, true);
        if (!hold.long_press_handled) {
            if (direction < 0) request_manual_previous();
            else request_manual_queue_move(direction);
        }
        hold = {};
        return;
    }
    if (code == LV_EVENT_PRESS_LOST && hold.pressed) {
        update_nav_hold(hold, esp_timer_get_time(), direction, true);
        hold = {};
    }
}

} // namespace

void reset_shuffle_queue()
{
    if (s_shuffle_order) heap_caps_free(s_shuffle_order);
    s_shuffle_order = nullptr;
    s_shuffle_count = 0;
    s_shuffle_cursor = 0;
}

void clear_saved_queue()
{
    heap_caps_free(s_saved_queue);
    s_saved_queue = nullptr;
    s_saved_queue_count = 0;
}

void discard_pending_saved_queue()
{
    s_saved_queue_pending = false;
    s_saved_playback_position_ms = 0;
    s_saved_playback_position_pending = false;
    if (s_playback_scope == PlaybackScope::SavedQueue) clear_saved_queue();
}

bool restore_saved_queue_if_pending()
{
    if (s_has_active_queue) return true;
    if (!s_saved_queue_pending) return false;
    // A failed load is still final for this boot: a new selection should not
    // keep retrying a corrupt or stale snapshot.
    s_saved_queue_pending = false;
    clear_saved_queue();
    auto *queue = static_cast<size_t *>(heap_caps_malloc(
        lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!queue) {
        queue = static_cast<size_t *>(heap_caps_malloc(
            lyra::media::kMaxTracks * sizeof(size_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!queue) {
        ESP_LOGW(kTag, "cannot allocate saved queue");
        return false;
    }
    size_t current = 0;
    uint32_t playback_position_ms = 0;
    const size_t count = lyra::media::load_queue_snapshot(
        queue, lyra::media::kMaxTracks, &current, &playback_position_ms);
    if (count == 0) {
        heap_caps_free(queue);
        return false;
    }
    s_saved_queue = queue;
    s_saved_queue_count = count;
    s_playback_scope = PlaybackScope::SavedQueue;
    s_queue_position = std::min(current, count - 1);
    s_queue_position_valid = true;
    s_current_track = s_saved_queue[s_queue_position];
    s_saved_playback_position_ms = playback_position_ms;
    s_saved_playback_position_pending = true;
    s_shuffle = false;
    reset_shuffle_queue();
    s_audio_eof_seen = false;
    s_has_active_queue = true;
    return true;
}

bool resume_saved_track_if_pending()
{
    if (!s_saved_playback_position_pending) return true;

    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) {
        s_saved_playback_position_ms = 0;
        s_saved_playback_position_pending = false;
        return false;
    }

    uint32_t position_ms = s_saved_playback_position_ms;
    if (track.duration_ms > 0 && position_ms >= track.duration_ms) {
        position_ms = track.duration_ms > 250 ? track.duration_ms - 250 : 0;
    }
    const esp_err_t audio_ret = start_track_audio(track, position_ms, true, false);
    s_saved_playback_position_ms = 0;
    s_saved_playback_position_pending = false;
    if (audio_ret != ESP_OK) {
        ESP_LOGW(kTag, "cannot restore playback position for %s: %s", track.path,
                 esp_err_to_name(audio_ret));
        return false;
    }
    return true;
}

size_t playback_queue_count()
{
    switch (s_playback_scope) {
        case PlaybackScope::Single: return 1;
        case PlaybackScope::AllSongs: return lyra::media::track_count();
        case PlaybackScope::Group: {
            lyra::media::Group group{};
            return lyra::media::group_at(s_playback_group_kind, s_playback_group, &group) ?
                   group.track_count : 0;
        }
        case PlaybackScope::Playlist: {
            lyra::media::Playlist playlist{};
            return lyra::media::playlist_at(s_playback_playlist, &playlist) ?
                   playlist.track_count : 0;
        }
        case PlaybackScope::SmartPlaylist:
            return lyra::media::smart_playlist_track_count(s_playback_smart_playlist);
        case PlaybackScope::Folder: {
            size_t first = 0;
            size_t total = 0;
            lyra::media::folder_tracks(s_playback_folder_path, 0, &first, 1, &total);
            return total;
        }
        case PlaybackScope::Search: {
            const lyra::media::SearchStatus search = lyra::media::search_status();
            return search.ready ? search.result_count : 0;
        }
        case PlaybackScope::SavedQueue: return s_saved_queue_count;
    }
    return 0;
}

bool playback_queue_track_at(size_t position, size_t *track_index)
{
    if (!track_index || position >= playback_queue_count()) return false;
    switch (s_playback_scope) {
        case PlaybackScope::Single:
            *track_index = s_current_track;
            return true;
        case PlaybackScope::AllSongs:
            return lyra::media::sorted_track_at(position, track_index);
        case PlaybackScope::Group:
            return lyra::media::group_tracks(s_playback_group_kind, s_playback_group,
                                              position, track_index, 1) == 1;
        case PlaybackScope::Playlist:
            return lyra::media::playlist_tracks(s_playback_playlist, position,
                                                 track_index, 1) == 1;
        case PlaybackScope::SmartPlaylist:
            return lyra::media::smart_playlist_tracks(s_playback_smart_playlist, position,
                                                      track_index, 1) == 1;
        case PlaybackScope::Folder:
            return lyra::media::folder_tracks(s_playback_folder_path, position,
                                               track_index, 1) == 1;
        case PlaybackScope::Search:
        {
            lyra::media::SearchResult result{};
            if (lyra::media::search_results(position, &result, 1) != 1) return false;
            *track_index = result.track_index;
            return true;
        }
        case PlaybackScope::SavedQueue:
            *track_index = s_saved_queue[position];
            return true;
    }
    return false;
}

bool playback_queue_position(size_t track_index, size_t *position)
{
    if (!position) return false;
    const size_t count = playback_queue_count();
    for (size_t i = 0; i < count; ++i) {
        size_t candidate = 0;
        if (playback_queue_track_at(i, &candidate) && candidate == track_index) {
            *position = i;
            return true;
        }
    }
    return false;
}

bool build_shuffle_queue()
{
    reset_shuffle_queue();
    if (!s_shuffle) return true;

    const size_t count = playback_queue_count();
    if (count == 0 || count > static_cast<size_t>(0xFFFFFFFFu)) return false;
    s_shuffle_order = static_cast<uint32_t *>(heap_caps_malloc(
        count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!s_shuffle_order) {
        s_shuffle_order = static_cast<uint32_t *>(heap_caps_malloc(
            count * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!s_shuffle_order) return false;
    s_shuffle_count = count;
    for (size_t i = 0; i < count; ++i) s_shuffle_order[i] = static_cast<uint32_t>(i);
    for (size_t i = count; i > 1; --i) {
        const size_t other = esp_random() % i;
        std::swap(s_shuffle_order[i - 1], s_shuffle_order[other]);
    }

    size_t current_position = 0;
    size_t saved_track = 0;
    const bool retained_position = s_queue_position_valid && s_queue_position < count &&
                                   playback_queue_track_at(s_queue_position, &saved_track) &&
                                   saved_track == s_current_track;
    if (retained_position) {
        current_position = s_queue_position;
    } else if (!playback_queue_position(s_current_track, &current_position)) {
        reset_shuffle_queue();
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (s_shuffle_order[i] == current_position) {
            std::swap(s_shuffle_order[0], s_shuffle_order[i]);
            break;
        }
    }
    s_shuffle_cursor = 0;
    s_queue_position = 0;
    s_queue_position_valid = true;
    return true;
}

bool queue_track_at(size_t queue_position, size_t *track_index)
{
    const size_t count = playback_queue_count();
    if (!track_index || queue_position >= count) return false;
    if (!s_shuffle) return playback_queue_track_at(queue_position, track_index);
    if (!s_shuffle_order || s_shuffle_count != count) {
        if (!build_shuffle_queue()) return false;
    }
    return playback_queue_track_at(s_shuffle_order[queue_position], track_index);
}

bool current_queue_position(size_t *queue_position)
{
    if (!queue_position) return false;
    const size_t count = playback_queue_count();
    if (s_queue_position_valid && s_queue_position < count) {
        size_t saved_track = 0;
        if (queue_track_at(s_queue_position, &saved_track) && saved_track == s_current_track) {
            *queue_position = s_queue_position;
            return true;
        }
    }
    if (!s_shuffle) {
        if (!playback_queue_position(s_current_track, queue_position)) return false;
        s_queue_position = *queue_position;
        s_queue_position_valid = true;
        return true;
    }
    if (!s_shuffle_order || s_shuffle_count != count) {
        if (!build_shuffle_queue()) return false;
    }
    if (s_shuffle_cursor >= count) return false;
    *queue_position = s_shuffle_cursor;
    s_queue_position = *queue_position;
    s_queue_position_valid = true;
    return true;
}

void apply_replay_gain_to_current_track()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) return;
    const int16_t adjustment = s_replay_gain ? track.replay_gain_tenths_db : 0;
    const esp_err_t result = lyra::audio::set_replay_gain_adjustment(adjustment);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "could not apply ReplayGain for %s: %s", track.path,
                 esp_err_to_name(result));
    }
}

esp_err_t start_track_audio(const lyra::media::Track &track,
                            uint32_t start_position_ms,
                            bool start_paused,
                            bool record_play)
{
    s_saved_playback_position_ms = 0;
    s_saved_playback_position_pending = false;
    s_pending_track_advance = false;
    s_pending_track_advance_us = 0;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = 0;
    s_crossfade_fade_out_ends_us = 0;
    s_crossfade_transition_direction = 0;
    s_crossfade_pause_pending = false;
    const esp_err_t transition_result = lyra::audio::set_transition_gain(100);
    if (transition_result != ESP_OK) return transition_result;
    const esp_err_t replay_gain_result = lyra::audio::set_replay_gain_adjustment(
        s_replay_gain ? track.replay_gain_tenths_db : 0);
    if (replay_gain_result != ESP_OK) return replay_gain_result;
    const esp_err_t play_result = lyra::audio::play_from_position(
        track.path, start_position_ms, start_paused);
    if (play_result == ESP_OK && record_play) {
        const esp_err_t stats_result = lyra::media::record_track_play(s_current_track);
        if (stats_result != ESP_OK) {
            ESP_LOGW(kTag, "could not record playback for %s: %s", track.path,
                     esp_err_to_name(stats_result));
        }
    }
    return play_result;
}

void play_queue_position(size_t queue_position)
{
    size_t track_index;
    if (!queue_track_at(queue_position, &track_index)) return;
    if (s_shuffle) s_shuffle_cursor = queue_position;
    s_queue_position = queue_position;
    s_queue_position_valid = true;
    s_current_track = track_index;
    s_has_active_queue = true;
    s_audio_eof_seen = false;
    lyra::media::Track track{};
    if (lyra::media::track_at(s_current_track, &track)) {
        const esp_err_t audio_ret = start_track_audio(track);
        if (audio_ret != ESP_OK) {
            ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
        }
    }
    navigate_to(View::Player);
}

bool move_in_playback_queue(int direction, bool automatic)
{
    if (direction == 0) return false;
    const size_t count = playback_queue_count();
    if (count == 0) return false;

    size_t candidate_position = 0;
    if (s_shuffle) {
        if (!s_shuffle_order || s_shuffle_count != count) {
            if (!build_shuffle_queue()) return false;
        }
        const int64_t next = static_cast<int64_t>(s_shuffle_cursor) + direction;
        if (next < 0 || next >= static_cast<int64_t>(count)) {
            if (s_repeat_mode != RepeatMode::All || !build_shuffle_queue()) return false;
            s_shuffle_cursor = direction < 0 ? count - 1 : (count > 1 ? 1 : 0);
            candidate_position = s_shuffle_order[s_shuffle_cursor];
        } else {
            s_shuffle_cursor = static_cast<size_t>(next);
            candidate_position = s_shuffle_order[s_shuffle_cursor];
        }
    } else {
        size_t current_position = 0;
        if (!current_queue_position(&current_position)) return false;
        const int64_t next = static_cast<int64_t>(current_position) + direction;
        if (next < 0 || next >= static_cast<int64_t>(count)) {
            if (s_repeat_mode != RepeatMode::All) return false;
            candidate_position = direction < 0 ? count - 1 : 0;
        } else {
            candidate_position = static_cast<size_t>(next);
        }
    }

    size_t candidate = 0;
    if (!playback_queue_track_at(candidate_position, &candidate)) return false;
    s_queue_position = s_shuffle ? s_shuffle_cursor : candidate_position;
    s_queue_position_valid = true;
    s_current_track = candidate;
    lyra::media::Track track{};
    if (lyra::media::track_at(s_current_track, &track)) {
        const esp_err_t audio_ret = start_track_audio(track);
        if (audio_ret != ESP_OK) {
            ESP_LOGW(kTag, "cannot start %s: %s", track.path, esp_err_to_name(audio_ret));
        }
    }
    s_audio_eof_seen = false;
    if (!automatic || s_view == View::Player) render(View::Player);
    return true;
}

bool can_move_in_playback_queue(int direction)
{
    if (direction == 0) return false;
    const size_t count = playback_queue_count();
    if (count == 0) return false;
    if (s_shuffle) {
        if (!s_shuffle_order || s_shuffle_count != count) {
            if (!build_shuffle_queue()) return false;
        }
        const int64_t next = static_cast<int64_t>(s_shuffle_cursor) + direction;
        return (next >= 0 && next < static_cast<int64_t>(count)) ||
               s_repeat_mode == RepeatMode::All;
    }
    size_t current_position = 0;
    if (!current_queue_position(&current_position)) return false;
    const int64_t next = static_cast<int64_t>(current_position) + direction;
    return (next >= 0 && next < static_cast<int64_t>(count)) ||
           s_repeat_mode == RepeatMode::All;
}

bool begin_manual_crossfade(int direction)
{
    if (s_crossfade_seconds == 0 || s_crossfade_transition_direction != 0 ||
        s_crossfade_pause_pending ||
        !can_move_in_playback_queue(direction)) return false;
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!audio_status.playing || audio_status.paused || audio_status.eof) return false;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = esp_timer_get_time();
    s_crossfade_fade_out_ends_us = s_crossfade_fade_out_started_us +
        static_cast<int64_t>(s_crossfade_seconds) * 1000 * 1000;
    s_crossfade_transition_direction = direction;
    lyra::audio::set_transition_gain(100);
    return true;
}

bool begin_crossfade_pause()
{
    if (s_crossfade_seconds == 0 || s_crossfade_transition_direction != 0 ||
        s_crossfade_pause_pending) return false;
    const lyra::audio::Status audio_status = lyra::audio::status();
    if (!audio_status.playing || audio_status.paused || audio_status.eof) return false;
    s_crossfade_fade_in_started_us = 0;
    s_crossfade_fade_in_ends_us = 0;
    s_crossfade_fade_out_started_us = esp_timer_get_time();
    s_crossfade_fade_out_ends_us = s_crossfade_fade_out_started_us +
        static_cast<int64_t>(s_crossfade_seconds) * 1000 * 1000;
    s_crossfade_pause_pending = true;
    lyra::audio::set_transition_gain(100);
    return true;
}

void request_manual_queue_move(int direction)
{
    if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) return;
    if (!begin_manual_crossfade(direction)) move_in_playback_queue(direction, false);
}

void nav_play_cb(lv_event_t *)
{
    if (s_view == View::Player) {
        if (s_crossfade_transition_direction != 0 || s_crossfade_pause_pending) return;
        const lyra::audio::Status audio_status = lyra::audio::status();
        if (audio_status.playing && !audio_status.paused) {
            if (!begin_crossfade_pause()) lyra::audio::toggle_pause();
        } else if (audio_status.playing && audio_status.paused) {
            if (s_crossfade_seconds == 0) {
                lyra::audio::toggle_pause();
            } else {
                lyra::audio::set_transition_gain(0);
                if (lyra::audio::toggle_pause() == ESP_OK) begin_crossfade_fade_in();
                else lyra::audio::set_transition_gain(100);
            }
        } else if (restart_current_track()) {
            begin_crossfade_fade_in();
        }
    } else {
        show_now_playing();
    }
}

void nav_previous_cb(lv_event_t *event)
{
    nav_button_event(event, -1, s_previous_hold);
}

void nav_next_cb(lv_event_t *event)
{
    nav_button_event(event, 1, s_next_hold);
}

bool restart_current_track()
{
    lyra::media::Track track{};
    if (!lyra::media::track_at(s_current_track, &track)) return false;
    const esp_err_t audio_ret = start_track_audio(track);
    if (audio_ret != ESP_OK) {
        ESP_LOGW(kTag, "cannot restart %s: %s", track.path, esp_err_to_name(audio_ret));
        return false;
    }
    s_audio_eof_seen = false;
    return true;
}

void make_virtual_nav()
{
    lv_obj_t *dock = make_box(s_screen, 0, kScreenHeight - kNavHeight,
                              kScreenWidth, kNavHeight, kNavSurface);
    make_box(dock, 0, 0, kScreenWidth, 1, kAccentDark);
    const char *icons[] = {LV_SYMBOL_BARS, LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT, LV_SYMBOL_LEFT};
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *button = make_button(dock, 5 + i * 63, 4, 58, 36, kSurface, 6);
        lv_obj_t *icon = make_label(button, icons[i], i == 0 && s_view == View::Menu ? kAccent : kTextSecondary);
        lv_obj_center(icon);
        if (i == 0) {
            add_route(button, View::Menu);
        } else if (i == 1) {
            lv_obj_add_event_cb(button, nav_previous_cb, LV_EVENT_PRESSED, nullptr);
            lv_obj_add_event_cb(button, nav_previous_cb, LV_EVENT_PRESSING, nullptr);
            lv_obj_add_event_cb(button, nav_previous_cb, LV_EVENT_RELEASED, nullptr);
            lv_obj_add_event_cb(button, nav_previous_cb, LV_EVENT_PRESS_LOST, nullptr);
        } else if (i == 2) {
            lv_obj_add_event_cb(button, nav_play_cb, LV_EVENT_CLICKED, nullptr);
        } else if (i == 3) {
            lv_obj_add_event_cb(button, nav_next_cb, LV_EVENT_PRESSED, nullptr);
            lv_obj_add_event_cb(button, nav_next_cb, LV_EVENT_PRESSING, nullptr);
            lv_obj_add_event_cb(button, nav_next_cb, LV_EVENT_RELEASED, nullptr);
            lv_obj_add_event_cb(button, nav_next_cb, LV_EVENT_PRESS_LOST, nullptr);
        } else {
            lv_obj_add_event_cb(button, nav_back_cb, LV_EVENT_CLICKED, nullptr);
        }
    }
}

uint8_t display_volume_percent(uint8_t volume_percent)
{
    const uint8_t maximum = lyra::audio::maximum_volume_percent();
    if (maximum == 0) return 0;
    return static_cast<uint8_t>(std::min<uint32_t>(
        100u, (static_cast<uint32_t>(volume_percent) * 100u + maximum / 2u) / maximum));
}

uint8_t audio_volume_percent_from_display(uint8_t display_percent)
{
    const uint8_t maximum = lyra::audio::maximum_volume_percent();
    return static_cast<uint8_t>(std::min<uint32_t>(
        maximum, (static_cast<uint32_t>(display_percent) * maximum + 50u) / 100u));
}

void update_status_volume_label(uint8_t volume_percent)
{
    if (!s_status_volume_label) return;
    char volume_text[24];
    std::snprintf(volume_text, sizeof(volume_text), "%s %u", LV_SYMBOL_VOLUME_MID,
                  static_cast<unsigned>(display_volume_percent(volume_percent)));
    lv_label_set_text(s_status_volume_label, volume_text);
}

void close_volume_popup()
{
    if (!s_volume_popup) return;
    const esp_err_t result = lyra::audio::save_volume();
    if (result != ESP_OK) ESP_LOGW(kTag, "could not save volume: %s", esp_err_to_name(result));
    lv_obj_delete(s_volume_popup);
    s_volume_popup = nullptr;
}

void volume_popup_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_current_target_obj(event);
    const uint8_t display_value = static_cast<uint8_t>(lv_slider_get_value(slider));
    const uint8_t audio_value = audio_volume_percent_from_display(display_value);
    lyra::audio::set_volume(audio_value);
    update_status_volume_label(audio_value);
    lv_obj_t *value_label = static_cast<lv_obj_t *>(lv_obj_get_user_data(slider));
    if (value_label) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u%%",
                      static_cast<unsigned>(display_value));
        lv_label_set_text(value_label, text);
    }
}

void volume_popup_slider_released_cb(lv_event_t *)
{
    const esp_err_t result = lyra::audio::save_volume();
    if (result != ESP_OK) ESP_LOGW(kTag, "could not save volume: %s", esp_err_to_name(result));
}

void show_volume_popup_cb(lv_event_t *)
{
    if (s_volume_popup) {
        lv_obj_move_foreground(s_volume_popup);
        return;
    }

    const lyra::audio::Status audio_status = lyra::audio::status();
    s_volume_popup = make_box(s_screen, 8, kStatusHeight + 4, 224, 104, kSurfaceRaised, 10);
    lv_obj_set_style_border_width(s_volume_popup, 1, 0);
    lv_obj_set_style_border_color(s_volume_popup, kAccentDark, 0);
    lv_obj_set_style_shadow_width(s_volume_popup, 12, 0);
    lv_obj_set_style_shadow_color(s_volume_popup, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_volume_popup, LV_OPA_40, 0);
    lv_obj_move_foreground(s_volume_popup);

    lv_obj_t *title = make_label(s_volume_popup, tr(lyra::i18n::StringId::Volume), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);
    char value_text[16];
    std::snprintf(value_text, sizeof(value_text), "%u%%",
                  static_cast<unsigned>(display_volume_percent(audio_status.volume_percent)));
    lv_obj_t *value = make_label(s_volume_popup, value_text, kAccent);
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -52, 12);
    lv_obj_t *close = make_button(s_volume_popup, 184, 7, 32, 32, kSurface, 16);
    lv_obj_t *close_icon = make_label(close, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(close_icon);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_volume_popup(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *slider = lv_slider_create(s_volume_popup);
    lv_obj_set_pos(slider, 14, 60);
    lv_obj_set_size(slider, 196, 14);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, display_volume_percent(audio_status.volume_percent), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, kDivider, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, kAccentDark, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, kTextOnAccent, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, kAccent, LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_user_data(slider, value);
    lv_obj_add_event_cb(slider, volume_popup_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, volume_popup_slider_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(slider, volume_popup_slider_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

} // namespace lyra::gui::internal
