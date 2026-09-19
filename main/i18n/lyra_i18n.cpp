/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lyra_i18n.h"

#include <cstdio>

namespace lyra::i18n {
namespace {

struct CatalogEntry {
    const char *text[kLanguageCount];
    const char *one[kLanguageCount];
    const char *few[kLanguageCount];
    const char *many[kLanguageCount];
};

#define LYRA_I18N_COUNT(id, en_one, en_many, fr_one, fr_many, de_one, de_many, \
                        es_one, es_many, it_one, it_many, ja_one, ja_many, \
                        ko_one, ko_many, ru_one, ru_few, ru_many, \
                        zh_hans_one, zh_hans_many, zh_hant_one, zh_hant_many) \
    { {en_one, fr_one, de_one, es_one, it_one, ja_one, ko_one, ru_one, \
       zh_hans_one, zh_hant_one}, \
      {en_one, fr_one, de_one, es_one, it_one, ja_one, ko_one, ru_one, \
       zh_hans_one, zh_hant_one}, \
      {en_many, fr_many, de_many, es_many, it_many, ja_many, ko_many, ru_few, \
       zh_hans_many, zh_hant_many}, \
      {en_many, fr_many, de_many, es_many, it_many, ja_many, ko_many, ru_many, \
       zh_hans_many, zh_hant_many} },
#define LYRA_I18N_ENTRY(id, en, fr, de, es, it, ja, ko, ru, zh_hans, zh_hant) \
    { {en, fr, de, es, it, ja, ko, ru, zh_hans, zh_hant}, \
      {en, fr, de, es, it, ja, ko, ru, zh_hans, zh_hant}, \
      {en, fr, de, es, it, ja, ko, ru, zh_hans, zh_hant}, \
      {en, fr, de, es, it, ja, ko, ru, zh_hans, zh_hant} },
static const CatalogEntry kCatalog[] = {
#include "../../tools/i18n/lyra_i18n_catalog.inc"
};
#undef LYRA_I18N_ENTRY
#undef LYRA_I18N_COUNT

Language s_language = Language::English;

size_t index_of(StringId id)
{
    const size_t index = static_cast<size_t>(id);
    return index < static_cast<size_t>(StringId::Count) ? index : 0;
}

size_t language_index(Language language)
{
    const size_t index = static_cast<size_t>(language);
    return index < kLanguageCount ? index : static_cast<size_t>(Language::English);
}

bool russian_one(uint32_t value)
{
    return value % 10 == 1 && value % 100 != 11;
}

bool russian_few(uint32_t value)
{
    return value % 10 >= 2 && value % 10 <= 4 &&
           (value % 100 < 12 || value % 100 > 14);
}

const char *count_format(const CatalogEntry &entry, Language language, uint32_t count)
{
    const size_t index = language_index(language);
    if (language == Language::Russian) {
        if (russian_one(count)) return entry.one[index];
        if (russian_few(count)) return entry.few[index];
        return entry.many[index];
    }
    return count == 1 ? entry.one[index] : entry.many[index];
}

} // namespace

const char *tr(StringId id)
{
    return tr(id, s_language);
}

const char *tr(StringId id, Language language)
{
    return kCatalog[index_of(id)].text[language_index(language)];
}

const char *language_name(Language language)
{
    switch (language) {
        // Language names are deliberately looked up in their target locale,
        // so the picker remains native-name based even after the rest of the
        // catalog receives translated rows.
        case Language::English: return tr(StringId::LanguageEnglish, Language::English);
        case Language::French: return tr(StringId::LanguageFrench, Language::French);
        case Language::German: return tr(StringId::LanguageGerman, Language::German);
        case Language::Spanish: return tr(StringId::LanguageSpanish, Language::Spanish);
        case Language::Italian: return tr(StringId::LanguageItalian, Language::Italian);
        case Language::Japanese: return tr(StringId::LanguageJapanese, Language::Japanese);
        case Language::Korean: return tr(StringId::LanguageKorean, Language::Korean);
        case Language::Russian: return tr(StringId::LanguageRussian, Language::Russian);
        case Language::SimplifiedChinese:
            return tr(StringId::LanguageSimplifiedChinese, Language::SimplifiedChinese);
        case Language::TraditionalChinese:
            return tr(StringId::LanguageTraditionalChinese, Language::TraditionalChinese);
        case Language::Count: break;
    }
    return tr(StringId::LanguageEnglish, Language::English);
}

bool is_valid_language(uint8_t value)
{
    return value < static_cast<uint8_t>(Language::Count);
}

Language current_language()
{
    return s_language;
}

void set_language(Language language)
{
    if (static_cast<size_t>(language) < kLanguageCount) s_language = language;
}

void format_text(StringId id, const char *value, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id), value ? value : "");
}

void format_text_text(StringId id, const char *first, const char *second,
                      char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id), first ? first : "", second ? second : "");
}

void format_u32(StringId id, uint32_t value, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id), static_cast<unsigned>(value));
}

void format_u32_u32(StringId id, uint32_t first, uint32_t second,
                   char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id), static_cast<unsigned>(first),
                  static_cast<unsigned>(second));
}

void format_u32_text(StringId id, uint32_t value, const char *text,
                     char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id), static_cast<unsigned>(value),
                  text ? text : "");
}

void format_text_u32(StringId id, const char *text, uint32_t value,
                     char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id), text ? text : "",
                  static_cast<unsigned>(value));
}

void format_u64(StringId id, uint64_t value, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id),
                  static_cast<unsigned long long>(value));
}

void format_float(StringId id, double value, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    std::snprintf(output, capacity, tr(id), value);
}

void format_count(StringId id, uint32_t count, char *output, size_t capacity)
{
    if (!output || capacity == 0) return;
    const CatalogEntry &entry = kCatalog[index_of(id)];
    std::snprintf(output, capacity, count_format(entry, s_language, count),
                  static_cast<unsigned>(count));
}

} // namespace lyra::i18n
