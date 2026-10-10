/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
namespace {

constexpr int kSetupButtonY = 420;
constexpr uint32_t kWelcomeLanguageIntervalMs = 4000;
constexpr uint32_t kWelcomeFadeDurationMs = 300;

enum class WelcomeFadePhase : uint8_t { Waiting, FadeOut, FadeIn };
lv_obj_t *s_welcome_title = nullptr;
lv_obj_t *s_welcome_hint = nullptr;
lv_obj_t *s_welcome_start_label = nullptr;
lv_timer_t *s_welcome_language_timer = nullptr;
size_t s_welcome_language_index = 0;
WelcomeFadePhase s_welcome_fade_phase = WelcomeFadePhase::Waiting;
uint32_t s_welcome_phase_started = 0;
uint32_t s_welcome_next_transition = 0;

void set_welcome_text_opacity(lv_opa_t opacity)
{
    if (s_welcome_title) lv_obj_set_style_opa(s_welcome_title, opacity, 0);
    if (s_welcome_hint) lv_obj_set_style_opa(s_welcome_hint, opacity, 0);
    if (s_welcome_start_label) lv_obj_set_style_opa(s_welcome_start_label, opacity, 0);
}

void set_welcome_language(size_t index)
{
    if (index >= lyra::i18n::kLanguageCount) index = 0;
    s_welcome_language_index = index;
    const auto language = static_cast<lyra::i18n::Language>(index);
    if (s_welcome_title) {
        lv_label_set_text(s_welcome_title,
            tr(lyra::i18n::StringId::WelcomeToLyra, language));
    }
    if (s_welcome_hint) {
        lv_label_set_text(s_welcome_hint,
            tr(lyra::i18n::StringId::SetupWelcomeHint, language));
    }
    if (s_welcome_start_label) {
        lv_label_set_text(s_welcome_start_label,
            tr(lyra::i18n::StringId::StartSetup, language));
    }
}

void welcome_language_timer_cb(lv_timer_t *)
{
    const uint32_t now = lv_tick_get();
    if (s_welcome_fade_phase == WelcomeFadePhase::Waiting) {
        if (static_cast<int32_t>(now - s_welcome_next_transition) < 0) return;
        s_welcome_fade_phase = WelcomeFadePhase::FadeOut;
        s_welcome_phase_started = now;
    }

    const uint32_t elapsed = now - s_welcome_phase_started;
    if (s_welcome_fade_phase == WelcomeFadePhase::FadeOut) {
        if (elapsed >= kWelcomeFadeDurationMs) {
            set_welcome_text_opacity(LV_OPA_TRANSP);
            set_welcome_language((s_welcome_language_index + 1) %
                                 lyra::i18n::kLanguageCount);
            s_welcome_fade_phase = WelcomeFadePhase::FadeIn;
            s_welcome_phase_started = now;
        } else {
            const uint32_t opacity = LV_OPA_COVER -
                (elapsed * LV_OPA_COVER) / kWelcomeFadeDurationMs;
            set_welcome_text_opacity(static_cast<lv_opa_t>(opacity));
        }
        return;
    }

    if (s_welcome_fade_phase == WelcomeFadePhase::FadeIn) {
        if (elapsed >= kWelcomeFadeDurationMs) {
            set_welcome_text_opacity(LV_OPA_COVER);
            s_welcome_fade_phase = WelcomeFadePhase::Waiting;
            s_welcome_next_transition += kWelcomeLanguageIntervalMs;
            if (static_cast<int32_t>(now - s_welcome_next_transition) >= 0) {
                s_welcome_next_transition = now + kWelcomeLanguageIntervalMs;
            }
        } else {
            const uint32_t opacity = (elapsed * LV_OPA_COVER) /
                                     kWelcomeFadeDurationMs;
            set_welcome_text_opacity(static_cast<lv_opa_t>(opacity));
        }
    }
}

void setup_header(lyra::i18n::StringId title_id,
                  lyra::i18n::StringId hint_id, uint8_t step)
{
    for (uint8_t index = 0; index < 4; ++index) {
        const lv_color_t color = index < step ? kAccent : kDivider;
        make_box(s_screen, 20 + static_cast<int>(index) * 72, 25, 64, 4,
                 color, 2);
    }

    lv_obj_t *title = make_label(s_screen, tr(title_id), kTextPrimary);
    lv_obj_set_width(title, 288);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 47);

    if (hint_id != lyra::i18n::StringId::Count) {
        lv_obj_t *hint = make_label(s_screen, tr(hint_id), kTextSecondary);
        lv_obj_set_width(hint, 286);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 75);
    }
}

lv_obj_t *setup_button(lv_obj_t *parent, int x, int y, int width, int height,
                       const char *text, bool primary, lv_event_cb_t callback)
{
    lv_obj_t *button = make_button(parent, x, y, width, height,
                                   primary ? kAccentDark : kSurfaceRaised,
                                   12, !primary);
    lv_obj_t *label = make_label(button, text, primary ? kTextOnAccent : kTextPrimary);
    lv_obj_center(label);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    return button;
}

void setup_start_cb(lv_event_t *)
{
    render(View::SetupLanguage);
}

void setup_next_cb(lv_event_t *)
{
    switch (s_view) {
    case View::SetupLanguage: render(View::SetupClock); break;
    case View::SetupClock: render(View::SetupTheme); break;
    case View::SetupTheme: render(View::SetupScan); break;
    default: break;
    }
}

void setup_language_option_cb(lv_event_t *event)
{
    const uint8_t value = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(
        lv_event_get_user_data(event)));
    if (!lyra::i18n::is_valid_language(value)) return;
    const auto language = static_cast<lyra::i18n::Language>(value);
    if (language == lyra::i18n::current_language()) return;

    lyra::i18n::set_language(language);
    s_search_error[0] = '\0';
    s_database_status[0] = '\0';
    s_debug_status[0] = '\0';
    const esp_err_t result = save_user_settings();
    render(View::SetupLanguage);
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::LanguageSaveFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::SelectLanguage), message);
    }
}

void setup_time_cb(lv_event_t *)
{
    begin_clock_input(ClockInputKind::Time);
}

void setup_date_cb(lv_event_t *)
{
    begin_clock_input(ClockInputKind::Date);
}

void setup_theme_option_cb(lv_event_t *event)
{
    const uintptr_t selection = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (selection >= static_cast<uintptr_t>(Theme::Count)) return;
    s_theme = static_cast<Theme>(selection);
    apply_theme_palette();
    const esp_err_t result = save_user_settings();
    render(View::SetupTheme);
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::SetupSaveFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::Theme), message);
    }
}

void setup_later_cb(lv_event_t *)
{
    render(View::SetupComplete);
}

void setup_finish_cb(lv_event_t *)
{
    s_setup_pending = false;
    const esp_err_t result = save_user_settings();
    if (result != ESP_OK) {
        s_setup_pending = true;
        char message[112];
        format_text(lyra::i18n::StringId::SetupSaveFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::SetupCompleteTitle), message);
        return;
    }
    render(View::Menu);
}

void setup_language_row(lv_obj_t *parent, int y,
                        lyra::i18n::Language language, size_t index)
{
    const bool active = language == lyra::i18n::current_language();
    lv_obj_t *row = make_button(parent, 10, y, 280, 38,
                                active ? kAccentSurface : kSurface, 9, true);
    lv_obj_t *label = make_label(row, lyra::i18n::language_name(language),
                                 active ? kAccent : kTextPrimary);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -12, 0);
    }
    lv_obj_add_event_cb(row, setup_language_option_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
}

void setup_theme_row(lv_obj_t *parent, int y, Theme theme, uintptr_t selection)
{
    const bool active = s_theme == theme;
    lv_obj_t *row = make_button(parent, 16, y, 288, 48,
                                active ? kAccentSurface : kSurface, 10, true);
    lv_obj_t *label = make_label(row, theme_name(theme),
                                 active ? kAccent : kTextPrimary);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);
    if (active) {
        lv_obj_t *check = make_label(row, LV_SYMBOL_OK, kAccent);
        lv_obj_align(check, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_obj_add_event_cb(row, setup_theme_option_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(selection));
}

} // namespace

void stop_setup_welcome_cycle()
{
    if (s_welcome_language_timer) lv_timer_delete(s_welcome_language_timer);
    s_welcome_language_timer = nullptr;
    s_welcome_title = nullptr;
    s_welcome_hint = nullptr;
    s_welcome_start_label = nullptr;
    s_welcome_fade_phase = WelcomeFadePhase::Waiting;
}

void render_setup_welcome()
{
    stop_setup_welcome_cycle();
    if (load_brand_logo()) {
        lv_obj_t *logo = lv_image_create(s_screen);
        lv_image_set_src(logo, &s_brand_logo_descriptor);
        lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 104);
    }

    s_welcome_title = make_label(s_screen,
        tr(lyra::i18n::StringId::WelcomeToLyra), kTextPrimary);
    lv_obj_set_width(s_welcome_title, 290);
    lv_label_set_long_mode(s_welcome_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_welcome_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_welcome_title, LV_ALIGN_TOP_MID, 0, 214);

    s_welcome_hint = make_label(s_screen,
        tr(lyra::i18n::StringId::SetupWelcomeHint), kTextSecondary);
    lv_obj_set_width(s_welcome_hint, 280);
    lv_label_set_long_mode(s_welcome_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_welcome_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_welcome_hint, LV_ALIGN_TOP_MID, 0, 270);

    lv_obj_t *start = setup_button(s_screen, 24, 408, 272, 52,
        tr(lyra::i18n::StringId::StartSetup), true, setup_start_cb);
    s_welcome_start_label = lv_obj_get_child(start, 0);
    const size_t current_language = static_cast<size_t>(lyra::i18n::current_language());
    set_welcome_language(current_language);
    set_welcome_text_opacity(LV_OPA_COVER);
    s_welcome_fade_phase = WelcomeFadePhase::Waiting;
    s_welcome_next_transition = lv_tick_get() + kWelcomeLanguageIntervalMs;
    s_welcome_language_timer = lv_timer_create(welcome_language_timer_cb, 20, nullptr);
}

void render_setup_language()
{
    setup_header(lyra::i18n::StringId::SelectLanguage,
                 lyra::i18n::StringId::Count, 1);
    lv_obj_t *list = make_box(s_screen, 10, 96, 300, 306, kBackground);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (size_t index = 0; index < lyra::i18n::kLanguageCount; ++index) {
        setup_language_row(list, 4 + static_cast<int>(index) * 43,
                           static_cast<lyra::i18n::Language>(index), index);
    }
    setup_button(s_screen, 24, kSetupButtonY, 272, 48,
                 tr(lyra::i18n::StringId::NextSetup), true, setup_next_cb);
}

void render_setup_clock()
{
    setup_header(lyra::i18n::StringId::TimeAndDate,
                 lyra::i18n::StringId::SetupClockHint, 2);
    lv_obj_t *summary = make_box(s_screen, 16, 142, 288, 68, kAccentSurface, 12, true);
    char current[40];
    format_clock_date_time(lyra::clock::now(), current, sizeof(current));
    lv_obj_t *value = make_label(summary, current, kAccent);
    lv_obj_set_width(value, 268);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(value);

    setup_button(s_screen, 16, 228, 288, 54,
        tr(lyra::i18n::StringId::SetTime), false, setup_time_cb);
    setup_button(s_screen, 16, 296, 288, 54,
        tr(lyra::i18n::StringId::SetDate), false, setup_date_cb);
    setup_button(s_screen, 24, kSetupButtonY, 272, 48,
        tr(lyra::i18n::StringId::NextSetup), true, setup_next_cb);
}

void render_setup_theme()
{
    setup_header(lyra::i18n::StringId::Theme,
                 lyra::i18n::StringId::SetupThemeHint, 3);
    for (uintptr_t selection = 0; selection < static_cast<uintptr_t>(Theme::Count); ++selection) {
        setup_theme_row(s_screen, 133 + static_cast<int>(selection) * 54,
                        static_cast<Theme>(selection), selection);
    }
    setup_button(s_screen, 24, kSetupButtonY, 272, 48,
        tr(lyra::i18n::StringId::NextSetup), true, setup_next_cb);
}

void render_setup_scan()
{
    setup_header(lyra::i18n::StringId::ScanMusicLibrary,
                 lyra::i18n::StringId::SetupScanPrompt, 4);
    setup_button(s_screen, 16, 198, 288, 58,
        tr(lyra::i18n::StringId::ScanNowSetup), true, setup_scan_now_cb);
    setup_button(s_screen, 16, 274, 288, 54,
        tr(lyra::i18n::StringId::LaterSetup), false, setup_later_cb);
}

void render_setup_complete()
{
    for (uint8_t index = 0; index < 4; ++index) {
        make_box(s_screen, 20 + static_cast<int>(index) * 72, 25, 64, 4,
                 kAccent, 2);
    }
    lv_obj_t *badge = make_box(s_screen, 124, 142, 72, 72, kAccentSurface, 36);
    lv_obj_t *check = make_label(badge, LV_SYMBOL_OK, kAccent);
    lv_obj_center(check);

    lv_obj_t *title = make_label(s_screen,
        tr(lyra::i18n::StringId::SetupCompleteTitle), kTextPrimary);
    lv_obj_set_width(title, 292);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 244);

    lv_obj_t *hint = make_label(s_screen,
        tr(lyra::i18n::StringId::SetupCompleteHint), kTextSecondary);
    lv_obj_set_width(hint, 280);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 286);

    setup_button(s_screen, 24, 408, 272, 52,
        tr(lyra::i18n::StringId::FinishSetup), true, setup_finish_cb);
}

} // namespace lyra::gui::internal
