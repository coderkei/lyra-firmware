/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace lyra::i18n {

enum class Language : uint8_t {
    English,
    French,
    German,
    Spanish,
    Italian,
    Japanese,
    Korean,
    Russian,
    SimplifiedChinese,
    TraditionalChinese,
    Count,
};

enum class StringId : uint16_t {
#define LYRA_I18N_COUNT(id, en_one, en_many, fr_one, fr_many, de_one, de_many, \
                       es_one, es_many, it_one, it_many, ja_one, ja_many, \
                       ko_one, ko_many, ru_one, ru_few, ru_many, \
                       zh_hans_one, zh_hans_many, zh_hant_one, zh_hant_many) id,
#define LYRA_I18N_ENTRY(id, en, fr, de, es, it, ja, ko, ru, zh_hans, zh_hant) id,
#include "../../tools/i18n/lyra_i18n_catalog.inc"
#undef LYRA_I18N_ENTRY
#undef LYRA_I18N_COUNT
    Count,
};

constexpr size_t kLanguageCount = static_cast<size_t>(Language::Count);

const char *tr(StringId id);
const char *tr(StringId id, Language language);
const char *language_name(Language language);
bool is_valid_language(uint8_t value);
Language current_language();
void set_language(Language language);

void format_text(StringId id, const char *value, char *output, size_t capacity);
void format_text_text(StringId id, const char *first, const char *second,
                      char *output, size_t capacity);
void format_u32(StringId id, uint32_t value, char *output, size_t capacity);
void format_u32_u32(StringId id, uint32_t first, uint32_t second,
                   char *output, size_t capacity);
void format_u32_text(StringId id, uint32_t value, const char *text,
                     char *output, size_t capacity);
void format_text_u32(StringId id, const char *text, uint32_t value,
                     char *output, size_t capacity);
void format_u64(StringId id, uint64_t value, char *output, size_t capacity);
void format_float(StringId id, double value, char *output, size_t capacity);
void format_count(StringId id, uint32_t count, char *output, size_t capacity);

} // namespace lyra::i18n
