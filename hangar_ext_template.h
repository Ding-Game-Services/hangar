/*
 * hangar.h — Hangar Extension Diagnostics Contract (template)
 *
 * Copy this file into your core's repo as `hangar.h`, and write a matching
 * `hangar.cpp` alongside it. This is NOT part of ding-core-sdk / ding_core.h
 * — it's Hangar's own optional convention for exposing deeper, genuinely
 * system-specific diagnostics (CPU registers, PPU scanline state, mapper
 * state, whatever your core has) that a generic API could never anticipate.
 *
 * WHY THIS EXISTS
 * ding_core.h's ding_diag_cpu_state()/ding_diag_video_state() already cover
 * a single freeform text dump each. This contract exists for anything
 * richer than that — a categorized, structured list of values — without
 * requiring Hangar to know anything about your specific system.
 *
 * HOW IT WORKS
 * Hangar doesn't know or care what a "PPU" or a "mapper" is. It just asks
 * your core "how many diagnostic entries do you have right now?" and then
 * "give me entry N as three strings: category, name, value." Hangar groups
 * by category and displays name/value pairs in a table. All the actual
 * knowledge of what's worth showing lives in your hangar.cpp, not in Hangar.
 *
 * WHERE THE DATA SHOULD COME FROM
 * Keep this file (and hangar.cpp) as a thin bridge, not a dumping ground.
 * Your CPU/PPU/APU classes should NOT friend hangar.cpp or expose their
 * internals directly — instead, give each class a narrow getter, e.g.
 * `CpuDebugState Cpu::getDebugState() const`, returning a small plain
 * struct of whatever's relevant. hangar.cpp calls those getters and turns
 * the results into entries. This keeps diagnostic code out of cpu.cpp/
 * ppu.cpp/apu.cpp entirely, and keeps your classes' encapsulation intact.
 *
 * EXPORT REQUIREMENTS
 * Both functions must have extern "C" linkage (same rule as ding_core.h)
 * and be exported the same way your ding_* functions already are. If your
 * CMakeLists already sets WINDOWS_EXPORT_ALL_SYMBOLS ON for the core target
 * (required for ding_core.h itself — see the SDK conventions doc), nothing
 * extra is needed on Windows. Nothing extra is needed on Linux either,
 * since GCC/Clang export global symbols from a .so by default.
 *
 * PERFORMANCE
 * Hangar may poll these every frame while its System Diagnostics panel is
 * open (there's a Live/Refresh toggle on the Hangar side, but don't rely on
 * it — implement both functions to be cheap and non-allocating regardless).
 *
 * BOTH FUNCTIONS ARE OPTIONAL. A core with neither is treated by Hangar as
 * "hasn't implemented this yet," not as an error.
 */

#ifndef HANGAR_EXT_H
#define HANGAR_EXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Returns how many diagnostic entries are currently available. This can
 * change frame to frame (e.g. a "current sprite" sub-view that only exists
 * mid-scanline) — Hangar re-queries it before every batch of
 * hangar_get_entry() calls, never caches it across frames.
 */
uint32_t hangar_get_entry_count(void);

/*
 * Fills category_buf/name_buf/value_buf with entry `index`'s data as
 * null-terminated strings. Each buffer is buf_size bytes, provided by
 * Hangar — write at most buf_size-1 characters plus the null terminator.
 *
 * `category` groups entries in Hangar's UI (e.g. "CPU", "PPU", "Mapper").
 * Use the same category string consistently across entries that belong
 * together; Hangar sorts/groups by exact string match, not by any other
 * ordering hint.
 *
 * Example entries a core might expose:
 *   category="CPU",    name="PC",       value="0x8123"
 *   category="CPU",    name="Flags",    value="Z N H C"
 *   category="PPU",    name="Scanline", value="142"
 *   category="Mapper",  name="Bank",     value="3 (MBC1)"
 *
 * `index` is always less than the most recent hangar_get_entry_count()
 * result — Hangar does not call this with an out-of-range index.
 */
void hangar_get_entry(uint32_t index,
                       char* category_buf,
                       char* name_buf,
                       char* value_buf,
                       uint32_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* HANGAR_EXT_H */
