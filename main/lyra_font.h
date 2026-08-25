/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "lvgl.h"

namespace lyra::font {

// The 16 px multilingual font is compiled into the application image, so it
// is available before the MicroSD card is mounted.
void init();
const lv_font_t *ui();

} // namespace lyra::font
