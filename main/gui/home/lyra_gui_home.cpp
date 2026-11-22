/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

// Home and More: large two-column tiles, one tap from everything.

#include "../lyra_gui_internal.h"

namespace lyra::gui::internal {
namespace {

enum class HomeDestination : uintptr_t { Music, Radio, Wireless, Files, More, Settings };

struct HomeTile {
    lyra::i18n::StringId label;
    Icon icon;
    uint32_t hue;
    HomeDestination destination;
};

constexpr HomeTile kHomeTiles[] = {
    {lyra::i18n::StringId::Music, Icon::Music, 0xF2B320, HomeDestination::Music},
    {lyra::i18n::StringId::InternetRadio, Icon::Radio, 0x1FA7A8, HomeDestination::Radio},
    {lyra::i18n::StringId::WifiSettings, Icon::Wifi, 0xE8657A, HomeDestination::Wireless},
    {lyra::i18n::StringId::Files, Icon::Folder, 0x5DBB4A, HomeDestination::Files},
    {lyra::i18n::StringId::More, Icon::Apps, 0xD2A679, HomeDestination::More},
    {lyra::i18n::StringId::Settings, Icon::Settings, 0x3A6FD8, HomeDestination::Settings},
};

void home_tile_cb(lv_event_t *event)
{
    const auto destination = static_cast<HomeDestination>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    switch (destination) {
        case HomeDestination::Music: open_library_cb(event); break;
        case HomeDestination::Radio: navigate_to(View::InternetRadio); break;
        case HomeDestination::Wireless: navigate_to(View::WifiSettings); break;
        case HomeDestination::Files: open_folders_cb(event); break;
        case HomeDestination::More: navigate_to(View::Apps); break;
        case HomeDestination::Settings: navigate_to(View::Settings); break;
    }
}

enum class MoreDestination : uintptr_t { Photos, Ebook, Queue, Equalizer, Search, Playlists };

struct MoreTile {
    lyra::i18n::StringId label;
    Icon icon;
    uint32_t hue;
    MoreDestination destination;
};

constexpr MoreTile kMoreTiles[] = {
    {lyra::i18n::StringId::Photos, Icon::Photo, 0x3D8EF0, MoreDestination::Photos},
    {lyra::i18n::StringId::Ebook, Icon::Book, 0xE8874A, MoreDestination::Ebook},
    {lyra::i18n::StringId::Queue, Icon::Queue, 0x9B6BDA, MoreDestination::Queue},
    {lyra::i18n::StringId::Equalizer, Icon::Equalizer, 0x34C27C, MoreDestination::Equalizer},
    {lyra::i18n::StringId::Search, Icon::Search, 0xE05A8A, MoreDestination::Search},
    {lyra::i18n::StringId::Playlists, Icon::Playlist, 0x1FA7A8, MoreDestination::Playlists},
};

void more_tile_cb(lv_event_t *event)
{
    const auto destination = static_cast<MoreDestination>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    switch (destination) {
        case MoreDestination::Photos: navigate_to(View::Photos); break;
        case MoreDestination::Ebook: navigate_to(View::Ebook); break;
        case MoreDestination::Queue: open_queue_cb(event); break;
        case MoreDestination::Equalizer: navigate_to(View::Equalizer); break;
        case MoreDestination::Search: navigate_to(View::Search); break;
        case MoreDestination::Playlists:
            s_playlists_from_library = false;
            navigate_to(View::Playlists);
            break;
    }
}

} // namespace

void render_menu()
{
    // Home has no header: the status bar, then three rows of large tiles that
    // fill the screen above the mini player.
    lv_obj_t *body = make_scroll_body(kStatusHeight);
    constexpr int kGap = 10;
    constexpr int kTileWidth = (kScreenWidth - 2 * kPageInset - kGap) / 2;
    const int available = content_height(kStatusHeight) - 2 * kGap;
    const int tile_height = std::clamp((available - 2 * kGap) / 3, 96, 140);
    for (size_t i = 0; i < sizeof(kHomeTiles) / sizeof(kHomeTiles[0]); ++i) {
        const HomeTile &tile = kHomeTiles[i];
        const int x = kPageInset + static_cast<int>(i % 2) * (kTileWidth + kGap);
        const int y = kGap + static_cast<int>(i / 2) * (tile_height + kGap);
        lv_obj_t *button = make_tile(body, x, y, kTileWidth, tile_height, tile.icon, tile.hue,
                                     tr(tile.label));
        lv_obj_add_event_cb(button, home_tile_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(tile.destination)));
    }
}

void render_apps()
{
    make_header(tr(lyra::i18n::StringId::More), View::Menu, true);
    lv_obj_t *body = make_scroll_body(kContentTop);
    constexpr int kGap = 10;
    constexpr int kTileWidth = (kScreenWidth - 2 * kPageInset - kGap) / 2;
    constexpr int kTileHeight = 108;
    for (size_t i = 0; i < sizeof(kMoreTiles) / sizeof(kMoreTiles[0]); ++i) {
        const MoreTile &tile = kMoreTiles[i];
        const int x = kPageInset + static_cast<int>(i % 2) * (kTileWidth + kGap);
        const int y = 4 + static_cast<int>(i / 2) * (kTileHeight + kGap);
        lv_obj_t *button = make_tile(body, x, y, kTileWidth, kTileHeight, tile.icon, tile.hue,
                                     tr(tile.label));
        lv_obj_add_event_cb(button, more_tile_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(tile.destination)));
    }
}

} // namespace lyra::gui::internal
