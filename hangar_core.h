/*
 * hangar_core.h
 *
 * Reusable "load a Ding core and resolve its ding_core.h API" logic.
 * This is the same logic from the original CLI prototype, pulled out
 * so the GUI (and anything else later) can call it without duplicating
 * platform-specific LoadLibrary/dlopen code.
 */

#ifndef HANGAR_CORE_H
#define HANGAR_CORE_H

#include <string>
#include <vector>

#include "ding_core.h"

#if defined(_WIN32)
    #include <windows.h>
    typedef HMODULE LibHandle;
#else
    typedef void* LibHandle;
#endif

/* Every function ding_core.h declares, as a function-pointer struct. */
struct DingCoreApi {
    void        (*ding_init)() = nullptr;
    void        (*ding_destroy)() = nullptr;
    void        (*ding_reset)() = nullptr;
    void        (*ding_run_frame)() = nullptr;

    DingResult  (*ding_load_rom)(const uint8_t*, size_t) = nullptr;
    DingResult  (*ding_load_disc)(DingDiscImage*) = nullptr;
    DingResult  (*ding_load_bios)(uint32_t, const uint8_t*, size_t) = nullptr;
    uint8_t     (*ding_is_disc_swap_pending)() = nullptr;
    void        (*ding_swap_disc)(DingDiscImage*) = nullptr;

    const DingCoreInfo*      (*ding_get_core_info)() = nullptr;
    const DingVideoInfo*     (*ding_get_video_info)() = nullptr;
    const DingAudioInfo*     (*ding_get_audio_info)() = nullptr;
    const DingRomIdentity*   (*ding_get_rom_identity)() = nullptr;
    const DingSaveStateInfo* (*ding_get_savestate_info)() = nullptr;
    uint32_t    (*ding_get_memory_region_count)() = nullptr;
    void        (*ding_get_memory_region)(uint32_t, DingMemoryRegion*) = nullptr;
    uint32_t    (*ding_get_bios_count)() = nullptr;
    void        (*ding_get_bios_descriptor)(uint32_t, DingBiosDescriptor*) = nullptr;
    uint32_t    (*ding_get_input_descriptor_count)() = nullptr;
    void        (*ding_get_input_descriptor)(uint32_t, DingInputDescriptor*) = nullptr;

    const uint8_t* (*ding_get_framebuffer)() = nullptr;
    void        (*ding_get_current_dimensions)(uint32_t*, uint32_t*) = nullptr;

    uint32_t    (*ding_get_audio_sample_count)() = nullptr;
    uint32_t    (*ding_read_audio_samples)(float*, uint32_t) = nullptr;

    void        (*ding_set_button)(uint8_t, uint8_t, uint8_t) = nullptr;
    void        (*ding_set_axis)(uint8_t, uint8_t, int16_t) = nullptr;

    size_t      (*ding_save_state)(uint8_t*, size_t) = nullptr;
    DingResult  (*ding_load_state)(const uint8_t*, size_t) = nullptr;

    void        (*ding_set_region)(const char*) = nullptr;

    // Optional diagnostics — allowed to be absent per ding_core.h.
    size_t      (*ding_diag_cpu_state)(char*, size_t) = nullptr;
    size_t      (*ding_diag_video_state)(char*, size_t) = nullptr;
    uint8_t     (*ding_has_error)() = nullptr;
    const char* (*ding_diag_last_error)() = nullptr;
};

/* Result of attempting to load a core. */
struct LoadedCore {
    bool        loaded = false;        // true only if load + all required symbols succeeded
    LibHandle   handle = nullptr;
    DingCoreApi api;
    std::string path;

    std::vector<std::string> missingRequired;
    std::vector<std::string> missingOptional;
    std::string loadError;             // set if the library itself failed to load

    bool romLoaded = false;

    // Hangar extension diagnostics (see hangar_ext_template.h) — entirely
    // optional, not part of ding_core.h. Both null if a core hasn't
    // implemented hangar.cpp yet; hasHangarExt is true only if both resolved.
    uint32_t (*hangar_get_entry_count)() = nullptr;
    void (*hangar_get_entry)(uint32_t, char*, char*, char*, uint32_t) = nullptr;
    bool hasHangarExt = false;
};

/* Loads a core library and resolves its API. Does not call ding_init(). */
LoadedCore loadCore(const std::string& path);

/* Unloads a core. Safe to call on an already-unloaded/invalid LoadedCore. */
void unloadCore(LoadedCore& core);

#endif // HANGAR_CORE_H
