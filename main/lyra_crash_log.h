/*
 * SPDX-FileCopyrightText: 2026 Emotivate Lyra contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

namespace lyra::crash_log {

// After a panic or watchdog reset, writes a readable report (and the raw core
// dump when one was stored in flash) to /sdcard/.lyra/crash, then clears the
// flash copy. Call once at startup after the MicroSD card is mounted.
void save_after_boot();

// True once, after save_after_boot() wrote a report, so the GUI can say so.
bool take_report_notice();

} // namespace lyra::crash_log
