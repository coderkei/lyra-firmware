/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace lyra::gui::ime {

enum class Layout : uint8_t {
    English,
    Russian,
    Korean,
    ChinesePinyin,
    JapaneseKana,
    Count,
};

enum class KanaModifier : uint8_t { Dakuten, Handakuten, Small };

void reset();
// T9 mode swaps the per-layout QWERTY composition for phone-keypad input.
// Changing the mode discards any composition in progress.
void set_t9_mode(bool enabled);
bool t9_mode();
// key is '0'..'9' or '*'; tapping the same key again within the cycle window
// replaces the last character with the next one on that key. Korean keys
// 1-3 are Cheonjiin vowel strokes. upper only selects the case of a newly
// started character.
void process_t9_key(Layout layout, char key, bool upper, char *text, size_t capacity);
const char *preedit();
bool dictionary_available(Layout layout);
size_t candidate_count(Layout layout);
const char *candidate(Layout layout, size_t index);
bool select_candidate(Layout layout, size_t index, char *text, size_t capacity);
void process_key(Layout layout, const char *key, char *text, size_t capacity);
void process_kana_flick(Layout layout, const char *kana, char *text, size_t capacity);
void apply_kana_modifier(KanaModifier modifier, char *text, size_t capacity);
void commit(Layout layout, char *text, size_t capacity);
bool backspace(Layout layout, char *text, size_t capacity);
void space(Layout layout, char *text, size_t capacity);

} // namespace lyra::gui::ime
