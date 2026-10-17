/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"
#include "../search/lyra_gui_ime.h"

namespace lyra::gui::internal {

char s_wifi_password[64]{};
char s_wifi_selected_ssid[33]{};
bool s_wifi_password_visible = false;

namespace {

lv_timer_t *s_wifi_refresh_timer;
lv_timer_t *s_network_test_refresh_timer;
uint32_t s_wifi_rendered_generation;
uint32_t s_network_test_rendered_generation;
lyra::wifi::AccessPoint s_wifi_rendered_access_points[lyra::wifi::kMaxAccessPoints]{};
size_t s_wifi_rendered_access_point_count;

void wifi_toggle_cb(lv_event_t *)
{
    const lyra::wifi::Status wifi = lyra::wifi::status();
    const esp_err_t result = lyra::wifi::set_enabled(!wifi.enabled);
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::WifiOperationFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::WifiSettings), message);
    }
    render(View::WifiSettings);
}

void scan_networks_cb(lv_event_t *)
{
    const esp_err_t result = lyra::wifi::scan();
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::WifiOperationFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::WifiSettings), message);
    }
}

void disconnect_wifi_cb(lv_event_t *)
{
    const esp_err_t result = lyra::wifi::disconnect();
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::WifiOperationFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::WifiSettings), message);
    }
    render(View::WifiSettings);
}

void forget_network_cb(lv_event_t *)
{
    const esp_err_t result = lyra::wifi::forget_network();
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::WifiOperationFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::WifiSettings), message);
    }
    render(View::WifiSettings);
}

void begin_connection(const char *ssid, const char *password)
{
    const esp_err_t result = lyra::wifi::connect(ssid, password);
    if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::WifiConnectFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::WifiSettings), message);
        return;
    }
    s_wifi_password[0] = '\0';
    s_wifi_password_visible = false;
    navigate_back(View::WifiSettings);
}

void access_point_cb(lv_event_t *event)
{
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (encoded == 0) return;
    const lyra::wifi::Status wifi = lyra::wifi::status();
    const size_t index = static_cast<size_t>(encoded - 1);
    if (index >= s_wifi_rendered_access_point_count) return;
    const lyra::wifi::AccessPoint &access_point = s_wifi_rendered_access_points[index];
    if (access_point.saved && wifi.has_saved_network &&
        std::strcmp(access_point.ssid, wifi.saved_ssid) == 0) {
        const esp_err_t result = lyra::wifi::connect_saved();
        if (result != ESP_OK) {
            char message[112];
            format_text(lyra::i18n::StringId::WifiConnectFailed,
                        esp_err_to_name(result), message, sizeof(message));
            show_notice(tr(lyra::i18n::StringId::WifiSettings), message);
        }
        render(View::WifiSettings);
    } else if (!access_point.secured) {
        begin_connection(access_point.ssid, "");
    } else {
        copy_ui_text(s_wifi_selected_ssid, sizeof(s_wifi_selected_ssid), access_point.ssid);
        s_wifi_password[0] = '\0';
        s_wifi_password_visible = false;
        lyra::gui::ime::reset();
        navigate_to(View::WifiPassword);
    }
}

void make_wifi_toggle(lv_obj_t *parent, const lyra::wifi::Status &wifi)
{
    lv_obj_t *row = make_button(parent, 7, 0, 306, 62, kSurface, 6, true);
    lv_obj_t *title = make_label(row, tr(lyra::i18n::StringId::WifiSettings), kTextPrimary);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);
    const char *summary = nullptr;
    char summary_text[80];
    if (wifi.connected) {
        format_text(lyra::i18n::StringId::ConnectedTo, wifi.connected_ssid,
                    summary_text, sizeof(summary_text));
        summary = summary_text;
    } else if (wifi.connecting && wifi.saved_ssid[0]) {
        format_text(lyra::i18n::StringId::ConnectingTo, wifi.saved_ssid,
                    summary_text, sizeof(summary_text));
        summary = summary_text;
    } else {
        summary = wifi.enabled ? tr(wifi.connection_failed ?
            lyra::i18n::StringId::WifiConnectionFailed :
            lyra::i18n::StringId::NotConnected) :
                                 tr(lyra::i18n::StringId::Off);
    }
    lv_obj_t *subtitle = make_label(row, summary, kTextMuted);
    make_marquee(subtitle, 220);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 12, 33);
    const lv_color_t track_color = wifi.enabled ? kAccent : kDivider;
    lv_obj_t *switch_track = make_box(row, 250, 18, 42, 24, track_color, 12);
    make_box(switch_track, wifi.enabled ? 21 : 3, 3, 18, 18,
             wifi.enabled ? kTextOnAccent : kTextPrimary, 9);
    lv_obj_add_event_cb(row, wifi_toggle_cb, LV_EVENT_CLICKED, nullptr);
}

void make_access_point_row(lv_obj_t *body, int y, size_t index,
                           const lyra::wifi::AccessPoint &access_point)
{
    const char *kind = access_point.saved ? tr(lyra::i18n::StringId::SavedNetwork) :
        access_point.secured ? tr(lyra::i18n::StringId::SecuredNetwork) :
                               tr(lyra::i18n::StringId::OpenNetwork);
    const char *subtitle = kind;
    lv_obj_t *row = make_row(body, y, LV_SYMBOL_SETTINGS, access_point.ssid,
                             subtitle, View::WifiSettings, 56);
    make_wifi_signal_bars(row, 269, 21, wifi_signal_bar_count(access_point.rssi),
                          kAccent, kDivider);
    lv_obj_remove_event_cb(row, route_cb);
    lv_obj_add_event_cb(row, access_point_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index + 1)));
}

const char *ping_summary(const lyra::wifi::PingStatus &ping, bool dns,
                         char *output, size_t capacity)
{
    if (ping.running) return tr(lyra::i18n::StringId::PingRunning);
    if (!ping.complete) return tr(lyra::i18n::StringId::PingReady);
    if (dns && ping.dns_lookup_failed) return tr(lyra::i18n::StringId::PingDnsFailed);
    if (ping.received == 0) {
        if (ping.resolved_address[0]) {
            std::snprintf(output, capacity, "%s · %s",
                          tr(lyra::i18n::StringId::PingNoReply), ping.resolved_address);
            return output;
        }
        return tr(lyra::i18n::StringId::PingNoReply);
    }
    char replies[32];
    format_u32_u32(lyra::i18n::StringId::PingReplies, ping.received,
                   ping.transmitted, replies, sizeof(replies));
    if (ping.resolved_address[0]) {
        std::snprintf(output, capacity, "%s · %s", replies, ping.resolved_address);
        return output;
    }
    copy_ui_text(output, capacity, replies);
    return output;
}

void run_ping_cb(lv_event_t *event)
{
    const uintptr_t encoded = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if (encoded == 0) return;
    const auto target = static_cast<lyra::wifi::PingTarget>(encoded - 1);
    const esp_err_t result = lyra::wifi::start_ping(target);
    if (result == ESP_ERR_INVALID_STATE) {
        const lyra::wifi::Status wifi = lyra::wifi::status();
        if (!wifi.connected) {
            show_notice(tr(lyra::i18n::StringId::NetworkTests),
                        tr(lyra::i18n::StringId::PingNotConnected));
        } else if (!wifi.ip_ping.running && !wifi.dns_ping.running) {
            char message[112];
            format_text(lyra::i18n::StringId::NetworkTestFailed,
                        esp_err_to_name(result), message, sizeof(message));
            show_notice(tr(lyra::i18n::StringId::NetworkTests), message);
        }
    } else if (result != ESP_OK) {
        char message[112];
        format_text(lyra::i18n::StringId::NetworkTestFailed,
                    esp_err_to_name(result), message, sizeof(message));
        show_notice(tr(lyra::i18n::StringId::NetworkTests), message);
    }
}

void wifi_refresh_cb(lv_timer_t *timer)
{
    if (s_view != View::WifiSettings) {
        if (s_wifi_refresh_timer == timer) s_wifi_refresh_timer = nullptr;
        lv_timer_delete(timer);
        return;
    }
    const lyra::wifi::Status wifi = lyra::wifi::status();
    if (wifi.generation == s_wifi_rendered_generation) return;
    if (s_scroll_body) {
        s_pending_scroll_restore_y = std::max<int32_t>(0, lv_obj_get_scroll_y(s_scroll_body));
    }
    s_wifi_refresh_timer = nullptr;
    lv_timer_delete(timer);
    render(View::WifiSettings);
}

void network_test_refresh_cb(lv_timer_t *timer)
{
    if (s_view != View::NetworkTests) {
        if (s_network_test_refresh_timer == timer) s_network_test_refresh_timer = nullptr;
        lv_timer_delete(timer);
        return;
    }
    const lyra::wifi::Status wifi = lyra::wifi::status();
    if (wifi.generation == s_network_test_rendered_generation) return;
    if (s_scroll_body) {
        s_pending_scroll_restore_y = std::max<int32_t>(0, lv_obj_get_scroll_y(s_scroll_body));
    }
    s_network_test_refresh_timer = nullptr;
    lv_timer_delete(timer);
    render(View::NetworkTests);
}

} // namespace

char *wifi_password_input_text()
{
    return s_wifi_password;
}

size_t wifi_password_input_capacity()
{
    return sizeof(s_wifi_password);
}

bool wifi_password_is_visible()
{
    return s_wifi_password_visible;
}

void submit_wifi_password()
{
    if (!s_wifi_selected_ssid[0]) return;
    if (!s_wifi_password[0]) {
        show_notice(tr(lyra::i18n::StringId::Password),
                    tr(lyra::i18n::StringId::WifiPasswordRequired));
        return;
    }
    begin_connection(s_wifi_selected_ssid, s_wifi_password);
}

void render_wifi_settings()
{
    make_header(tr(lyra::i18n::StringId::WifiSettings), View::SystemSettings, true);
    lv_obj_t *body = make_scroll_body(72);
    const lyra::wifi::Status wifi = lyra::wifi::status();
    s_wifi_rendered_access_point_count = wifi.access_point_count;
    std::memcpy(s_wifi_rendered_access_points, wifi.access_points,
                s_wifi_rendered_access_point_count * sizeof(s_wifi_rendered_access_points[0]));
    make_wifi_toggle(body, wifi);

    int next_y = 66;
    if (wifi.connected && wifi.ip_address[0]) {
        make_row(body, next_y, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::IpAddress),
                 wifi.ip_address, View::WifiSettings, 54);
        next_y += 58;
    }
    lv_obj_t *scan = make_row(body, next_y, LV_SYMBOL_REFRESH,
                              tr(lyra::i18n::StringId::ScanNetworks),
                              wifi.scanning ? tr(lyra::i18n::StringId::ScanningNetworks) : nullptr,
                              View::WifiSettings, 54);
    lv_obj_remove_event_cb(scan, route_cb);
    if (wifi.enabled) {
        lv_obj_add_event_cb(scan, scan_networks_cb, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_set_style_opa(scan, LV_OPA_60, 0);
        lv_obj_clear_flag(scan, LV_OBJ_FLAG_CLICKABLE);
    }
    next_y += 58;

    if (wifi.connected) {
        lv_obj_t *disconnect = make_row(body, next_y, LV_SYMBOL_CLOSE,
            tr(lyra::i18n::StringId::Disconnect), wifi.connected_ssid,
            View::WifiSettings, 54);
        lv_obj_remove_event_cb(disconnect, route_cb);
        lv_obj_add_event_cb(disconnect, disconnect_wifi_cb, LV_EVENT_CLICKED, nullptr);
        next_y += 58;
    }
    if (wifi.has_saved_network) {
        lv_obj_t *forget = make_row(body, next_y, LV_SYMBOL_CLOSE,
            tr(lyra::i18n::StringId::ForgetNetwork), wifi.saved_ssid,
            View::WifiSettings, 54);
        lv_obj_remove_event_cb(forget, route_cb);
        lv_obj_add_event_cb(forget, forget_network_cb, LV_EVENT_CLICKED, nullptr);
        next_y += 58;
    }

    lv_obj_t *heading = make_label(body, tr(lyra::i18n::StringId::AvailableNetworks),
                                   kTextSecondary);
    lv_obj_set_pos(heading, 14, next_y + 2);
    next_y += 34;
    if (!wifi.enabled) {
        lv_obj_t *off = make_label(body, tr(lyra::i18n::StringId::Off), kTextMuted);
        lv_obj_set_pos(off, 14, next_y + 6);
    } else if (wifi.scanning && wifi.access_point_count == 0) {
        lv_obj_t *scanning = make_label(body, tr(lyra::i18n::StringId::ScanningNetworks),
                                        kTextMuted);
        lv_obj_set_pos(scanning, 14, next_y + 6);
    } else if (wifi.access_point_count == 0) {
        lv_obj_t *empty = make_label(body, tr(lyra::i18n::StringId::WifiNoNetworksFound),
                                     kTextMuted);
        lv_obj_set_pos(empty, 14, next_y + 6);
    } else {
        for (size_t index = 0; index < wifi.access_point_count; ++index) {
            make_access_point_row(body, next_y + static_cast<int>(index) * 58,
                                  index, wifi.access_points[index]);
        }
    }
    s_wifi_rendered_generation = wifi.generation;
    if (!s_wifi_refresh_timer) {
        s_wifi_refresh_timer = lv_timer_create(wifi_refresh_cb, 500, nullptr);
    }
}

void render_wifi_password()
{
    make_header(tr(lyra::i18n::StringId::Password), View::WifiSettings, true);
    lv_obj_t *body = make_box(s_screen, 0, 72, 320, content_height(72), kBackground);
    lv_obj_t *network = make_label(body, s_wifi_selected_ssid, kTextSecondary);
    lv_obj_set_width(network, 292);
    lv_label_set_long_mode(network, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(network, 14, 6);

    lv_obj_t *input = make_box(body, 9, 34, 220, 48, kSurfaceRaised, 7, true);
    s_search_label = make_label(input, "", kTextMuted);
    make_marquee(s_search_label, 196);
    lv_obj_align(s_search_label, LV_ALIGN_LEFT_MID, 12, 0);
    update_search_label();
    lv_obj_t *visibility = make_button(body, 239, 34, 72, 48, kSurfaceRaised, 7, true);
    lv_obj_t *visibility_label = make_label(visibility,
        tr(s_wifi_password_visible ? lyra::i18n::StringId::HidePassword :
                                     lyra::i18n::StringId::ShowPassword), kTextSecondary);
    make_marquee(visibility_label, 62);
    lv_obj_center(visibility_label);
    lv_obj_add_event_cb(visibility, [](lv_event_t *) {
        commit_keyboard_preedit();
        s_wifi_password_visible = !s_wifi_password_visible;
        render(View::WifiPassword);
    }, LV_EVENT_CLICKED, nullptr);

    const int keyboard_height = library_keyboard_height();
    const int keyboard_y = content_height(72) - keyboard_height;
    lv_obj_t *keyboard = make_box(body, 0, keyboard_y, 320, keyboard_height,
                                  kKeyboardSurface);
    make_library_keyboard(keyboard, tr(lyra::i18n::StringId::Connect));
}

void render_network_tests()
{
    make_header(tr(lyra::i18n::StringId::NetworkTests), View::DebugMenu, true);
    lv_obj_t *body = make_scroll_body(72);
    const lyra::wifi::Status wifi = lyra::wifi::status();
    char connection[80];
    if (wifi.connected) {
        format_text(lyra::i18n::StringId::ConnectedTo, wifi.connected_ssid,
                    connection, sizeof(connection));
    } else {
        copy_ui_text(connection, sizeof(connection), wifi.enabled ?
            tr(wifi.connection_failed ? lyra::i18n::StringId::WifiConnectionFailed :
                                       lyra::i18n::StringId::NotConnected) :
            tr(lyra::i18n::StringId::Off));
    }
    make_row(body, 0, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::WifiSettings),
             connection, View::WifiSettings, 58);

    char ip_status[80];
    const char *ip_summary = ping_summary(wifi.ip_ping, false, ip_status, sizeof(ip_status));
    lv_obj_t *ip = make_row(body, 64, LV_SYMBOL_REFRESH,
                            tr(lyra::i18n::StringId::PingIp), ip_summary,
                            View::NetworkTests, 62);
    lv_obj_remove_event_cb(ip, route_cb);
    lv_obj_add_event_cb(ip, run_ping_cb, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::wifi::PingTarget::Ip) + 1));

    char dns_status[80];
    const char *dns_summary = ping_summary(wifi.dns_ping, true, dns_status, sizeof(dns_status));
    lv_obj_t *dns = make_row(body, 130, LV_SYMBOL_REFRESH,
                             tr(lyra::i18n::StringId::PingDns), dns_summary,
                             View::NetworkTests, 62);
    lv_obj_remove_event_cb(dns, route_cb);
    lv_obj_add_event_cb(dns, run_ping_cb, LV_EVENT_CLICKED,
        reinterpret_cast<void *>(static_cast<uintptr_t>(lyra::wifi::PingTarget::Dns) + 1));

    if (wifi.connected && wifi.ip_address[0]) {
        make_row(body, 196, LV_SYMBOL_SETTINGS, tr(lyra::i18n::StringId::IpAddress),
                 wifi.ip_address, View::NetworkTests, 54);
    }
    s_network_test_rendered_generation = wifi.generation;
    if (!s_network_test_refresh_timer) {
        s_network_test_refresh_timer = lv_timer_create(network_test_refresh_cb, 500, nullptr);
    }
}

} // namespace lyra::gui::internal
