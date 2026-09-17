/*
 * hangar.cpp — Hangar Extension Diagnostics (starter/dummy implementation)
 *
 * Copy this into your core's src/ alongside your copy of hangar.h, then:
 *   1. Replace every entry below with real calls into your CPU/PPU/APU/
 *      mapper classes' own narrow getters (e.g. `cpu.getDebugState()`,
 *      returning a small plain struct) — see hangar.h's header comment
 *      for why those getters should be narrow, not `friend` access.
 *   2. Add this file to your core's CMakeLists.txt sources list (e.g.
 *      next to core_api.cpp in PERIDOT_SOURCES) so it actually compiles
 *      into the .dll/.so. Nothing else in your build needs to change —
 *      WINDOWS_EXPORT_ALL_SYMBOLS already exports these alongside ding_*.
 *   3. Delete the TODO entries below once real ones replace them. An
 *      empty/placeholder entry list is harmless but not useful.
 *
 * PATTERN USED HERE
 * hangar_get_entry_count() rebuilds a small cache of entries and returns
 * its size; hangar_get_entry() just reads back from that cache. This
 * matches how Hangar actually calls these — count is always queried once,
 * immediately followed by get_entry() for every index in that same batch —
 * so rebuilding once per count() call (not once per entry) is both correct
 * and cheap. Do not skip rebuilding in count() and expect stale cached
 * data to still be right; Hangar may poll every frame.
 */

#include "hangar.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// TODO: include whatever headers give you access to this core's actual
// running instance(s), e.g.:
//   #include "cpu.h"
//   #include "ppu.h"
// and however core_api.cpp already exposes the live instance to other
// translation units (an extern pointer, a singleton accessor, etc.).

namespace {

struct HangarEntry {
    std::string category;
    std::string name;
    std::string value;
};

/* Rebuilt every time hangar_get_entry_count() is called; hangar_get_entry()
 * just indexes into whatever was built by the most recent count() call. */
std::vector<HangarEntry>& entryCache() {
    static std::vector<HangarEntry> cache;
    return cache;
}

void copyToBuf(char* buf, uint32_t bufSize, const std::string& s) {
    if (bufSize == 0) return;
    std::snprintf(buf, bufSize, "%s", s.c_str());
}

} // namespace

extern "C" uint32_t hangar_get_entry_count() {
    auto& cache = entryCache();
    cache.clear();

    // ── CPU — TODO: replace with real state ─────────────────────────────
    // Example of the intended shape once wired up:
    //   CpuDebugState state = g_cpu.getDebugState();
    //   char pcHex[16];
    //   std::snprintf(pcHex, sizeof(pcHex), "0x%04X", state.pc);
    //   cache.push_back({"CPU", "PC", pcHex});
    cache.push_back({"CPU", "PC", "0x0000 (TODO: wire up real CPU state)"});
    cache.push_back({"CPU", "Flags", "---- (TODO)"});

    // ── PPU / Video — TODO: replace with real state ─────────────────────
    cache.push_back({"PPU", "Scanline", "0 (TODO)"});

    // ── Mapper / Cartridge — TODO: replace with real state ──────────────
    cache.push_back({"Mapper", "Bank", "0 (TODO)"});

    return static_cast<uint32_t>(cache.size());
}

extern "C" void hangar_get_entry(uint32_t index,
                                  char* category_buf,
                                  char* name_buf,
                                  char* value_buf,
                                  uint32_t buf_size) {
    auto& cache = entryCache();

    if (index >= cache.size()) {
        // Shouldn't happen per the contract (Hangar never calls this with
        // an out-of-range index), but stay safe rather than read garbage.
        if (buf_size > 0) {
            category_buf[0] = '\0';
            name_buf[0] = '\0';
            value_buf[0] = '\0';
        }
        return;
    }

    const HangarEntry& entry = cache[index];
    copyToBuf(category_buf, buf_size, entry.category);
    copyToBuf(name_buf, buf_size, entry.name);
    copyToBuf(value_buf, buf_size, entry.value);
}
