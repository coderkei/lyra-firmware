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
