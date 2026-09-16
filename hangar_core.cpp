#include "hangar_core.h"

#include <cstring>

#if defined(_WIN32)
    // windows.h already included via hangar_core.h
#else
    #include <dlfcn.h>
#endif

static void* getSymbol(LibHandle lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
#else
    return dlsym(lib, name);
#endif
}

static LibHandle loadLibraryPlatform(const std::string& path) {
#if defined(_WIN32)
    return LoadLibraryA(path.c_str());
#else
    return dlopen(path.c_str(), RTLD_NOW);
#endif
}

static std::string lastLoadError() {
#if defined(_WIN32)
    DWORD err = GetLastError();
    char msg[512] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, msg, sizeof(msg), nullptr);
    return std::string(msg);
#else
    const char* err = dlerror();
    return err ? std::string(err) : std::string("unknown error");
#endif
}

struct SymbolEntry {
    const char* name;
    void**      slot;
    bool        required;
};

static std::vector<SymbolEntry> buildSymbolTable(DingCoreApi& api) {
    return {
        {"ding_init",                       (void**)&api.ding_init,                       true},
        {"ding_destroy",                     (void**)&api.ding_destroy,                     true},
        {"ding_reset",                       (void**)&api.ding_reset,                       true},
        {"ding_run_frame",                   (void**)&api.ding_run_frame,                   true},

        {"ding_load_rom",                    (void**)&api.ding_load_rom,                    true},
        {"ding_load_disc",                   (void**)&api.ding_load_disc,                   true},
        {"ding_load_bios",                   (void**)&api.ding_load_bios,                   true},
        {"ding_is_disc_swap_pending",        (void**)&api.ding_is_disc_swap_pending,        true},
        {"ding_swap_disc",                   (void**)&api.ding_swap_disc,                   true},

        {"ding_get_core_info",               (void**)&api.ding_get_core_info,               true},
        {"ding_get_video_info",              (void**)&api.ding_get_video_info,              true},
        {"ding_get_audio_info",              (void**)&api.ding_get_audio_info,              true},
        {"ding_get_rom_identity",            (void**)&api.ding_get_rom_identity,            true},
        {"ding_get_savestate_info",          (void**)&api.ding_get_savestate_info,          true},
        {"ding_get_memory_region_count",     (void**)&api.ding_get_memory_region_count,     true},
        {"ding_get_memory_region",           (void**)&api.ding_get_memory_region,           true},
        {"ding_get_bios_count",              (void**)&api.ding_get_bios_count,              true},
        {"ding_get_bios_descriptor",         (void**)&api.ding_get_bios_descriptor,         true},
        {"ding_get_input_descriptor_count",  (void**)&api.ding_get_input_descriptor_count,  true},
        {"ding_get_input_descriptor",        (void**)&api.ding_get_input_descriptor,        true},

        {"ding_get_framebuffer",             (void**)&api.ding_get_framebuffer,             true},
        {"ding_get_current_dimensions",      (void**)&api.ding_get_current_dimensions,      true},

        {"ding_get_audio_sample_count",      (void**)&api.ding_get_audio_sample_count,      true},
        {"ding_read_audio_samples",          (void**)&api.ding_read_audio_samples,          true},

        {"ding_set_button",                  (void**)&api.ding_set_button,                  true},
        {"ding_set_axis",                    (void**)&api.ding_set_axis,                    true},

        {"ding_save_state",                  (void**)&api.ding_save_state,                  true},
        {"ding_load_state",                  (void**)&api.ding_load_state,                  true},

        {"ding_set_region",                  (void**)&api.ding_set_region,                  true},

        {"ding_diag_cpu_state",              (void**)&api.ding_diag_cpu_state,              false},
        {"ding_diag_video_state",            (void**)&api.ding_diag_video_state,            false},
        {"ding_has_error",                   (void**)&api.ding_has_error,                   false},
        {"ding_diag_last_error",             (void**)&api.ding_diag_last_error,             false},
    };
}

LoadedCore loadCore(const std::string& path) {
    LoadedCore core;
    core.path = path;

    core.handle = loadLibraryPlatform(path);
    if (!core.handle) {
        core.loadError = lastLoadError();
        return core;
    }

    std::vector<SymbolEntry> table = buildSymbolTable(core.api);

    for (auto& entry : table) {
        void* sym = getSymbol(core.handle, entry.name);
        *entry.slot = sym;
        if (!sym) {
            if (entry.required) core.missingRequired.push_back(entry.name);
            else                core.missingOptional.push_back(entry.name);
        }
    }

    core.loaded = core.missingRequired.empty();
    return core;
}

void unloadCore(LoadedCore& core) {
    if (core.handle) {
#if defined(_WIN32)
        FreeLibrary(core.handle);
#else
        dlclose(core.handle);
#endif
    }
    core = LoadedCore{};
}
