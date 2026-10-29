/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../lyra_gui_internal.h"

#include <initializer_list>

namespace lyra::gui::internal {
namespace {

// Dot Link: join each pair of same-coloured dots with a path so that every
// cell of the board is used. Boards are generated on the device: a random
// Hamiltonian path is cut into segments, so each puzzle has a full solution.
constexpr int kMinSize = 5;
constexpr int kMaxSize = 8;
constexpr int kMaxCells = kMaxSize * kMaxSize;
constexpr int kMaxColors = kMaxSize;
constexpr uint8_t kNone = 0xFF;
constexpr uint32_t kPalette[kMaxColors] = {
    0xEF4444, 0x3B82F6, 0x22C55E, 0xEAB308, 0xF97316, 0x06B6D4, 0xD946EF, 0x8B5CF6,
};

struct Path {
    uint8_t cells[kMaxCells];
    uint8_t length;
};

struct Puzzle {
    int size;
    int colors;
    uint8_t dot[kMaxCells];           // colour of the endpoint in a cell, or kNone
    Path solution[kMaxColors];
};

Puzzle s_puzzle{};
Path s_paths[kMaxColors]{};
uint8_t s_owner[kMaxCells];
int s_drawing = -1;
int s_last_color = -1;
uint32_t s_moves;
uint32_t s_level[kMaxSize + 1];
int s_next_size = 6;
bool s_solved;
lv_obj_t *s_board;
lv_obj_t *s_status;

uint32_t random_below(uint32_t limit)
{
    return limit ? esp_random() % limit : 0;
}

int cell_of(int row, int column) { return row * s_puzzle.size + column; }

bool adjacent(int a, int b)
{
    const int size = s_puzzle.size;
    const int dr = std::abs(a / size - b / size), dc = std::abs(a % size - b % size);
    return dr + dc == 1;
}

// Backbite moves keep the path Hamiltonian while shuffling its shape.
void random_hamiltonian_path(int size, uint8_t *path)
{
    const int count = size * size;
    uint8_t position[kMaxCells];
    for (int row = 0, index = 0; row < size; ++row) {
        for (int column = 0; column < size; ++column, ++index) {
            path[index] = static_cast<uint8_t>(row * size + (row % 2 ? size - 1 - column : column));
        }
    }
    const auto reverse = [&](int from, int to) {
        while (from < to) std::swap(path[from++], path[to--]);
    };
    for (int iteration = 0; iteration < count * 48; ++iteration) {
        if (esp_random() & 1u) reverse(0, count - 1);
        for (int i = 0; i < count; ++i) position[path[i]] = static_cast<uint8_t>(i);
        const int end = path[count - 1];
        const int row = end / size, column = end % size;
        const int moves[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        const auto &move = moves[random_below(4)];
        const int next_row = row + move[0], next_column = column + move[1];
        if (next_row < 0 || next_row >= size || next_column < 0 || next_column >= size) continue;
        const int neighbour = position[next_row * size + next_column];
        if (neighbour == count - 2) continue;
        reverse(neighbour + 1, count - 1);
    }
}

void generate_puzzle(int size)
{
    s_puzzle = {};
    s_puzzle.size = size;
    s_puzzle.colors = size;
    const int count = size * size;
    uint8_t path[kMaxCells];
    int lengths[kMaxColors];
    for (int attempt = 0; attempt < 40; ++attempt) {
        random_hamiltonian_path(size, path);
        // Each segment gets at least three cells; the rest is spread randomly.
        for (int color = 0; color < size; ++color) lengths[color] = 3;
        for (int spare = count - 3 * size; spare > 0; --spare) ++lengths[random_below(size)];
        bool trivial = false;
        for (int color = 0, start = 0; color < size; start += lengths[color++]) {
            trivial |= adjacent(path[start], path[start + lengths[color] - 1]);
        }
        if (!trivial) break; // Otherwise a one-step shortcut would hide the intended route.
    }
    std::memset(s_puzzle.dot, kNone, sizeof(s_puzzle.dot));
    uint8_t order[kMaxColors];
    for (int color = 0; color < size; ++color) order[color] = static_cast<uint8_t>(color);
    for (int color = size - 1; color > 0; --color) std::swap(order[color], order[random_below(color + 1)]);
    for (int segment = 0, start = 0; segment < size; start += lengths[segment++]) {
        Path &solution = s_puzzle.solution[order[segment]];
        solution.length = static_cast<uint8_t>(lengths[segment]);
        std::memcpy(solution.cells, path + start, solution.length);
        s_puzzle.dot[solution.cells[0]] = order[segment];
        s_puzzle.dot[solution.cells[solution.length - 1]] = order[segment];
    }
    std::memset(s_paths, 0, sizeof(s_paths));
    std::memset(s_owner, kNone, sizeof(s_owner));
    s_drawing = -1;
    s_last_color = -1;
    s_moves = 0;
    s_solved = false;
    ++s_level[size];
}

void rebuild_owner()
{
    std::memset(s_owner, kNone, sizeof(s_owner));
    for (int color = 0; color < s_puzzle.colors; ++color) {
        for (int i = 0; i < s_paths[color].length; ++i) {
            s_owner[s_paths[color].cells[i]] = static_cast<uint8_t>(color);
        }
    }
}

bool path_complete(int color)
{
    const Path &path = s_paths[color];
    return path.length >= 2 && path.cells[0] != path.cells[path.length - 1] &&
           s_puzzle.dot[path.cells[0]] == color && s_puzzle.dot[path.cells[path.length - 1]] == color;
}

int index_in_path(const Path &path, int cell)
{
    for (int i = 0; i < path.length; ++i) if (path.cells[i] == cell) return i;
    return -1;
}

int completed_links()
{
    int links = 0;
    for (int color = 0; color < s_puzzle.colors; ++color) links += path_complete(color);
    return links;
}

int filled_cells()
{
    int filled = 0;
    for (int cell = 0; cell < s_puzzle.size * s_puzzle.size; ++cell) filled += s_owner[cell] != kNone;
    return filled;
}

void update_status()
{
    if (!s_status) return;
    if (s_moves == 0 && filled_cells() == 0) {
        set_label_text_if_changed(s_status, tr(lyra::i18n::StringId::DotLinkHowTo));
        return;
    }
    char links[32], filled[32], moves[32], text[112];
    format_u32_u32(lyra::i18n::StringId::DotLinkLinks, static_cast<uint32_t>(completed_links()),
                   static_cast<uint32_t>(s_puzzle.colors), links, sizeof(links));
    format_u32_u32(lyra::i18n::StringId::DotLinkFilled, static_cast<uint32_t>(filled_cells()),
                   static_cast<uint32_t>(s_puzzle.size * s_puzzle.size), filled, sizeof(filled));
    format_u32(lyra::i18n::StringId::DotLinkMoves, s_moves, moves, sizeof(moves));
    std::snprintf(text, sizeof(text), "%s   %s   %s", links, filled, moves);
    set_label_text_if_changed(s_status, text);
}

void changed()
{
    rebuild_owner();
    if (s_board) lv_obj_invalidate(s_board);
    update_status();
}

// Extends the active path by one orthogonal step, cutting other paths.
bool extend_to(int cell)
{
    Path &path = s_paths[s_drawing];
    if (!path.length || !adjacent(path.cells[path.length - 1], cell)) return false;
    const int own = index_in_path(path, cell);
    if (own >= 0) {
        path.length = static_cast<uint8_t>(own + 1);
        return true;
    }
    if (path_complete(s_drawing)) return false;
    const uint8_t dot = s_puzzle.dot[cell];
    if (dot != kNone && dot != s_drawing) return false;
    const uint8_t owner = s_owner[cell];
    if (owner != kNone && owner != s_drawing) {
        Path &other = s_paths[owner];
        other.length = static_cast<uint8_t>(index_in_path(other, cell));
    }
    path.cells[path.length++] = static_cast<uint8_t>(cell);
    rebuild_owner();
    return true;
}

int cell_at(lv_obj_t *board, int *row_out = nullptr, int *column_out = nullptr)
{
    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);
    lv_area_t area;
    lv_obj_get_coords(board, &area);
    const int cell_size = lv_area_get_width(&area) / s_puzzle.size;
    const int column = std::clamp<int>((point.x - area.x1) / cell_size, 0, s_puzzle.size - 1);
    const int row = std::clamp<int>((point.y - area.y1) / cell_size, 0, s_puzzle.size - 1);
    if (row_out) *row_out = row;
    if (column_out) *column_out = column;
    return cell_of(row, column);
}

void show_solved();

void board_input_cb(lv_event_t *event)
{
    if (s_solved) return;
    lv_obj_t *board = lv_event_get_current_target_obj(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        const int cell = cell_at(board);
        s_drawing = -1;
        if (s_puzzle.dot[cell] != kNone) {
            s_drawing = s_puzzle.dot[cell];
            s_paths[s_drawing].length = 1;
            s_paths[s_drawing].cells[0] = static_cast<uint8_t>(cell);
        } else if (s_owner[cell] != kNone) {
            s_drawing = s_owner[cell];
            s_paths[s_drawing].length = static_cast<uint8_t>(index_in_path(s_paths[s_drawing], cell) + 1);
        }
        if (s_drawing >= 0 && s_drawing != s_last_color) {
            s_last_color = s_drawing;
            ++s_moves;
        }
        changed();
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (s_drawing < 0) return;
        int target_row, target_column;
        cell_at(board, &target_row, &target_column);
        bool moved = false;
        // Fast drags can skip cells; walk towards the finger one step at a time.
        for (int guard = 0; guard < kMaxCells; ++guard) {
            const Path &path = s_paths[s_drawing];
            const int head = path.cells[path.length - 1];
            const int row = head / s_puzzle.size, column = head % s_puzzle.size;
            if (row == target_row && column == target_column) break;
            const int dr = target_row - row, dc = target_column - column;
            const int step = std::abs(dr) >= std::abs(dc) ? cell_of(row + (dr > 0 ? 1 : -1), column) :
                                                            cell_of(row, column + (dc > 0 ? 1 : -1));
            if (!extend_to(step)) break;
            moved = true;
        }
        if (moved) changed();
        return;
    }
    // Released or lost.
    s_drawing = -1;
    if (completed_links() == s_puzzle.colors && filled_cells() == s_puzzle.size * s_puzzle.size) {
        s_solved = true;
        show_solved();
    }
}

void board_draw_cb(lv_event_t *event)
{
    lv_obj_t *board = lv_event_get_current_target_obj(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(board, &area);
    const int size = s_puzzle.size;
    const int cell = lv_area_get_width(&area) / size;
    const auto fill = [&](int x1, int y1, int x2, int y2, lv_color_t color, lv_opa_t opa, int radius) {
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_color = color;
        rect.bg_opa = opa;
        rect.radius = radius;
        const lv_area_t bounds{area.x1 + x1, area.y1 + y1, area.x1 + x2, area.y1 + y2};
        lv_draw_rect(layer, &rect, &bounds);
    };
    for (int index = 0; index < size * size; ++index) {
        if (s_owner[index] == kNone) continue;
        const int x = index % size * cell, y = index / size * cell;
        fill(x + 1, y + 1, x + cell - 1, y + cell - 1, lv_color_hex(kPalette[s_owner[index]]), LV_OPA_20, 0);
    }
    for (int line = 1; line < size; ++line) {
        fill(line * cell, 0, line * cell, size * cell - 1, kDivider, LV_OPA_COVER, 0);
        fill(0, line * cell, size * cell - 1, line * cell, kDivider, LV_OPA_COVER, 0);
    }
    const int pipe = std::max(4, cell * 3 / 10);
    for (int color = 0; color < s_puzzle.colors; ++color) {
        const Path &path = s_paths[color];
        const lv_color_t tint = lv_color_hex(kPalette[color]);
        for (int i = 1; i < path.length; ++i) {
            const int a = path.cells[i - 1], b = path.cells[i];
            const int ax = a % size * cell + cell / 2, ay = a / size * cell + cell / 2;
            const int bx = b % size * cell + cell / 2, by = b / size * cell + cell / 2;
            fill(std::min(ax, bx) - pipe / 2, std::min(ay, by) - pipe / 2,
                 std::max(ax, bx) + pipe / 2, std::max(ay, by) + pipe / 2, tint, LV_OPA_COVER, pipe / 2);
        }
    }
    const int dot = cell * 7 / 10;
    for (int index = 0; index < size * size; ++index) {
        if (s_puzzle.dot[index] == kNone) continue;
        const int x = index % size * cell + (cell - dot) / 2, y = index / size * cell + (cell - dot) / 2;
        fill(x, y, x + dot - 1, y + dot - 1, lv_color_hex(kPalette[s_puzzle.dot[index]]),
             LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
}

void new_puzzle(int size)
{
    generate_puzzle(size);
    render(View::DotLink);
}

void show_solved()
{
    lv_obj_t *overlay = make_box(s_screen, 0, 0, kScreenWidth, kScreenHeight, kOverlay);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(overlay);
    lv_obj_t *dialog = make_box(overlay, 30, 150, 260, 180, kSurfaceRaised, 12);
    lv_obj_t *title = make_label(dialog, tr(lyra::i18n::StringId::DotLinkSolved), kAccent);
    make_marquee(title, 232);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    char moves[32];
    format_u32(lyra::i18n::StringId::DotLinkMoves, s_moves, moves, sizeof(moves));
    lv_obj_t *detail = make_label(dialog, moves, kTextSecondary);
    lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t *stay = make_button(dialog, 14, 112, 100, 48, kSurface, 7);
    lv_obj_t *stay_label = make_label(stay, LV_SYMBOL_CLOSE, kTextSecondary);
    lv_obj_center(stay_label);
    lv_obj_add_event_cb(stay, dismiss_overlay_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *next = make_button(dialog, 124, 112, 122, 48, kAccentDark, 7);
    lv_obj_t *next_label = make_label(next, tr(lyra::i18n::StringId::DotLinkNext), kTextOnAccent);
    make_marquee(next_label, 112);
    lv_obj_center(next_label);
    lv_obj_add_event_cb(next, [](lv_event_t *) { new_puzzle(s_puzzle.size); }, LV_EVENT_CLICKED, nullptr);
}

void reset_cb(lv_event_t *)
{
    std::memset(s_paths, 0, sizeof(s_paths));
    s_solved = false;
    s_last_color = -1;
    changed();
}

// Reveals one route that is not yet right and counts it as a move.
void hint_cb(lv_event_t *)
{
    if (s_solved) return;
    for (int color = 0; color < s_puzzle.colors; ++color) {
        const Path &solution = s_puzzle.solution[color];
        Path &path = s_paths[color];
        if (path.length == solution.length &&
            std::memcmp(path.cells, solution.cells, solution.length) == 0) continue;
        for (int other = 0; other < s_puzzle.colors; ++other) {
            if (other == color) continue;
            Path &cut = s_paths[other];
            for (int i = 0; i < cut.length; ++i) {
                if (index_in_path(solution, cut.cells[i]) >= 0) {
                    cut.length = static_cast<uint8_t>(i);
                    break;
                }
            }
        }
        path = solution;
        s_last_color = color;
        ++s_moves;
        changed();
        if (completed_links() == s_puzzle.colors && filled_cells() == s_puzzle.size * s_puzzle.size) {
            s_solved = true;
            show_solved();
        }
        return;
    }
}

void make_control(lv_obj_t *parent, int x, int y, int width, const char *text, lv_event_cb_t callback)
{
    lv_obj_t *button = make_button(parent, x, y, width, 42, kSurfaceRaised, 7, true);
    lv_obj_t *label = make_label(button, text, kAccent);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
}

} // namespace

void stop_dot_link_runtime()
{
    s_drawing = -1;
    s_board = nullptr;
    s_status = nullptr;
}

void render_dot_link()
{
    if (s_puzzle.size < kMinSize) generate_puzzle(s_next_size);
    char level[24];
    std::snprintf(level, sizeof(level), "%dx%d #%u", s_puzzle.size, s_puzzle.size,
                  static_cast<unsigned>(s_level[s_puzzle.size]));
    make_header(tr(lyra::i18n::StringId::DotLink), View::Apps, true, level);
    const int height = content_height(72);
    lv_obj_t *body = make_box(s_screen, 0, 72, kScreenWidth, height, kBackground);
    s_status = make_label(body, "", kTextSecondary);
    make_marquee(s_status, 300);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 6);
    const int available = std::min(304, height - 32 - 8 - 42 - 6);
    const int cell = available / s_puzzle.size;
    const int board_size = cell * s_puzzle.size;
    s_board = make_box(body, (kScreenWidth - board_size) / 2, 32, board_size, board_size, kSurfaceRaised, 6);
    lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_board, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_board, board_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(s_board, board_input_cb, code, nullptr);
    }
    const int controls_y = 32 + board_size + 8;
    make_control(body, 8, controls_y, 72, LV_SYMBOL_REFRESH, reset_cb);
    make_control(body, 84, controls_y, 72, LV_SYMBOL_EYE_OPEN, hint_cb);
    char size_text[8];
    std::snprintf(size_text, sizeof(size_text), "%dx%d", s_next_size, s_next_size);
    lv_obj_t *size_button = make_button(body, 160, controls_y, 72, 42, kSurfaceRaised, 7, true);
    lv_obj_t *size_label = make_label(size_button, size_text, kTextPrimary);
    lv_obj_center(size_label);
    lv_obj_add_event_cb(size_button, [](lv_event_t *) {
        s_next_size = s_next_size >= kMaxSize ? kMinSize : s_next_size + 1;
        new_puzzle(s_next_size);
    }, LV_EVENT_CLICKED, nullptr);
    make_control(body, 236, controls_y, 76, LV_SYMBOL_SHUFFLE, [](lv_event_t *) {
        new_puzzle(s_next_size);
    });
    rebuild_owner();
    update_status();
    if (s_solved) show_solved();
}

} // namespace lyra::gui::internal
