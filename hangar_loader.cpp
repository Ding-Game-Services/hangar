/*
 * hangar_loader.cpp
 *
 * Hangar - offline native test tool for Ding cores.
 * Loads a compiled core (.dll / .so), verifies it implements the
 * ding_core.h API, loads a ROM, runs N frames, and dumps the resulting
 * framebuffer to a .ppm image so you can visually confirm it rendered.
 *
 * Usage:
 *   hangar <path-to-core.dll-or-.so> <path-to-rom> [frame-count]
 *
 * Build (example, adjust to your setup):
 *   Windows (MSVC):  cl hangar_loader.cpp
 *   Linux/GCC:       g++ hangar_loader.cpp -o hangar -ldl
 *
 * This file has no dependency on any specific core — it only depends
 * on ding_core.h / ding_types.h, same as every other consumer (Hydra,
 * Cockpit, Ding Engine).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ding_core.h"

#if defined(_WIN32)
    #include <windows.h>
    typedef HMODULE LibHandle;
#else
    #include <dlfcn.h>
    typedef void* LibHandle;
#endif

/* ─────────────────────────────────────────────────────────────────────────
 * Cross-platform load/lookup/close helpers
 * ───────────────────────────────────────────────────────────────────────── */

static LibHandle loadLibrary(const char* path) {
#if defined(_WIN32)
    return LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW);
#endif
}

static void* getSymbol(LibHandle lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(lib, name));
#else
    return dlsym(lib, name);
#endif
}

static void closeLibrary(LibHandle lib) {
#if defined(_WIN32)
    FreeLibrary(lib);
#else
    dlclose(lib);
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

/* ─────────────────────────────────────────────────────────────────────────
 * Every function ding_core.h declares, as a function-pointer struct.
 * This is the single place that needs updating if ding_core.h grows.
 * ───────────────────────────────────────────────────────────────────────── */

struct DingCoreApi {
    // Lifecycle
    void        (*ding_init)();
    void        (*ding_destroy)();
    void        (*ding_reset)();
    void        (*ding_run_frame)();

    // ROM / disc loading
    DingResult  (*ding_load_rom)(const uint8_t*, size_t);
    DingResult  (*ding_load_disc)(DingDiscImage*);
    DingResult  (*ding_load_bios)(uint32_t, const uint8_t*, size_t);
    uint8_t     (*ding_is_disc_swap_pending)();
    void        (*ding_swap_disc)(DingDiscImage*);

    // Capability queries
    const DingCoreInfo*      (*ding_get_core_info)();
    const DingVideoInfo*     (*ding_get_video_info)();
    const DingAudioInfo*     (*ding_get_audio_info)();
    const DingRomIdentity*   (*ding_get_rom_identity)();
    const DingSaveStateInfo* (*ding_get_savestate_info)();
    uint32_t    (*ding_get_memory_region_count)();
    void        (*ding_get_memory_region)(uint32_t, DingMemoryRegion*);
    uint32_t    (*ding_get_bios_count)();
    void        (*ding_get_bios_descriptor)(uint32_t, DingBiosDescriptor*);
    uint32_t    (*ding_get_input_descriptor_count)();
    void        (*ding_get_input_descriptor)(uint32_t, DingInputDescriptor*);

    // Video output
    const uint8_t* (*ding_get_framebuffer)();
    void        (*ding_get_current_dimensions)(uint32_t*, uint32_t*);

    // Audio output
    uint32_t    (*ding_get_audio_sample_count)();
    uint32_t    (*ding_read_audio_samples)(float*, uint32_t);

    // Input
    void        (*ding_set_button)(uint8_t, uint8_t, uint8_t);
    void        (*ding_set_axis)(uint8_t, uint8_t, int16_t);

    // Save states
    size_t      (*ding_save_state)(uint8_t*, size_t);
    DingResult  (*ding_load_state)(const uint8_t*, size_t);

    // Region config
    void        (*ding_set_region)(const char*);

    // Diagnostics (optional per ding_core.h — allowed to be absent)
    size_t      (*ding_diag_cpu_state)(char*, size_t);
    size_t      (*ding_diag_video_state)(char*, size_t);
    uint8_t     (*ding_has_error)();
    const char* (*ding_diag_last_error)();
};

/* Table of every REQUIRED symbol name, paired with where to store it.
 * Diagnostics are intentionally excluded — ding_core.h marks them optional,
 * so a missing diagnostic function is not a load failure. */
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

        // Optional diagnostics
        {"ding_diag_cpu_state",              (void**)&api.ding_diag_cpu_state,              false},
        {"ding_diag_video_state",            (void**)&api.ding_diag_video_state,            false},
        {"ding_has_error",                   (void**)&api.ding_has_error,                   false},
        {"ding_diag_last_error",             (void**)&api.ding_diag_last_error,             false},
    };
}

/* ─────────────────────────────────────────────────────────────────────────
 * File I/O helpers
 * ───────────────────────────────────────────────────────────────────────── */

/* Reads an entire file into a byte buffer. Returns true on success. */
static bool readFileBytes(const char* path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (size < 0) { std::fclose(f); return false; }

    out.resize(static_cast<size_t>(size));
    size_t readBytes = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);

    return readBytes == out.size();
}

/*
 * Writes a framebuffer to a .ppm file (simple uncompressed RGB image format —
 * no external library needed; opens in GIMP, IrfanView, most viewers).
 * Converts from the core's reported pixel format to plain 8-bit RGB.
 * Returns true on success.
 */
static bool writeFramebufferPPM(const char* path,
                                 const uint8_t* fb,
                                 uint32_t width,
                                 uint32_t height,
                                 DingPixelFormat format) {
    if (!fb) {
        std::fprintf(stderr, "writeFramebufferPPM: null framebuffer.\n");
        return false;
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "writeFramebufferPPM: could not open %s for writing.\n", path);
        return false;
    }

    std::fprintf(f, "P6\n%u %u\n255\n", width, height);

    std::vector<uint8_t> row(static_cast<size_t>(width) * 3);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint8_t r = 0, g = 0, b = 0;

            switch (format) {
                case DING_PIXFMT_RGBA8: {
                    const uint8_t* px = fb + (static_cast<size_t>(y) * width + x) * 4;
                    r = px[0]; g = px[1]; b = px[2];
                    break;
                }
                case DING_PIXFMT_XRGB8: {
                    const uint8_t* px = fb + (static_cast<size_t>(y) * width + x) * 4;
                    /* XRGB8: byte order is core/platform dependent in practice,
                     * but the common convention is [X, R, G, B]. */
                    r = px[1]; g = px[2]; b = px[3];
                    break;
                }
                case DING_PIXFMT_RGB565: {
                    const uint8_t* px = fb + (static_cast<size_t>(y) * width + x) * 2;
                    uint16_t packed = static_cast<uint16_t>(px[0] | (px[1] << 8));
                    uint8_t r5 = (packed >> 11) & 0x1F;
                    uint8_t g6 = (packed >> 5)  & 0x3F;
                    uint8_t b5 = packed & 0x1F;
                    r = static_cast<uint8_t>((r5 * 255) / 31);
                    g = static_cast<uint8_t>((g6 * 255) / 63);
                    b = static_cast<uint8_t>((b5 * 255) / 31);
                    break;
                }
                case DING_PIXFMT_YUV420:
                default:
                    /* Not handled yet — leave black rather than guess wrong. */
                    break;
            }

            row[x * 3 + 0] = r;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = b;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }

    std::fclose(f);

    if (format == DING_PIXFMT_YUV420) {
        std::fprintf(stderr,
            "Warning: YUV420 framebuffer conversion not yet implemented — "
            "%s was written but will be black.\n", path);
    }

    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────────────────────────────────── */

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "Usage: hangar <path-to-core.dll-or-.so> <path-to-rom> [frame-count]\n"
            "  frame-count defaults to 60 (about 1 second at 60fps).\n");
        return 1;
    }

    const char* corePath  = argv[1];
    const char* romPath   = argv[2];
    int frameCount        = (argc >= 4) ? std::atoi(argv[3]) : 60;
    if (frameCount <= 0) frameCount = 60;

    std::printf("Hangar - loading core: %s\n\n", corePath);

    LibHandle lib = loadLibrary(corePath);
    if (!lib) {
        std::fprintf(stderr, "FAILED to load library: %s\n", lastLoadError().c_str());
        return 1;
    }

    DingCoreApi api = {};
    std::vector<SymbolEntry> table = buildSymbolTable(api);

    int missingRequired = 0;
    int missingOptional  = 0;

    std::printf("Resolving ding_core.h symbols:\n");
    for (auto& entry : table) {
        void* sym = getSymbol(lib, entry.name);
        *entry.slot = sym;

        if (!sym) {
            if (entry.required) {
                std::printf("  [MISSING - REQUIRED] %s\n", entry.name);
                missingRequired++;
            } else {
                std::printf("  [missing - optional] %s\n", entry.name);
                missingOptional++;
            }
        }
    }

    std::printf("\n");

    if (missingRequired > 0) {
        std::fprintf(stderr,
            "Core is missing %d required function(s). Not calling into it.\n",
            missingRequired);
        closeLibrary(lib);
        return 1;
    }

    std::printf("All required functions present.");
    if (missingOptional > 0) {
        std::printf(" (%d optional diagnostic function(s) absent — that's allowed.)", missingOptional);
    }
    std::printf("\n\n");

    // Proof of life: init the core and print what it reports about itself.
    api.ding_init();

    const DingCoreInfo* info = api.ding_get_core_info();
    if (info) {
        std::printf("Core info:\n");
        std::printf("  Name:        %s\n", info->core_name ? info->core_name : "(null)");
        std::printf("  Platform:    %s\n", info->platform_name ? info->platform_name : "(null)");
        std::printf("  Version:     %s\n", info->version ? info->version : "(null)");
        std::printf("  API version: %u.%u (Hangar built against %d.%d.%d)\n",
                     info->api_version_major, info->api_version_minor,
                     DING_CORE_API_VERSION_MAJOR, DING_CORE_API_VERSION_MINOR,
                     DING_CORE_API_VERSION_PATCH);

        if (info->api_version_major != DING_CORE_API_VERSION_MAJOR) {
            std::printf("  WARNING: major API version mismatch — core may be incompatible.\n");
        }
    } else {
        std::fprintf(stderr, "ding_get_core_info() returned NULL.\n");
    }

    // Load the ROM.
    std::printf("\nLoading ROM: %s\n", romPath);
    std::vector<uint8_t> romData;
    if (!readFileBytes(romPath, romData)) {
        std::fprintf(stderr, "Could not read ROM file.\n");
        api.ding_destroy();
        closeLibrary(lib);
        return 1;
    }
    std::printf("  Read %zu bytes.\n", romData.size());

    DingResult loadResult = api.ding_load_rom(romData.data(), romData.size());
    if (loadResult != DING_OK) {
        std::fprintf(stderr, "ding_load_rom() failed with code %d.\n", loadResult);
        if (api.ding_diag_last_error) {
            const char* err = api.ding_diag_last_error();
            if (err) std::fprintf(stderr, "  Core error: %s\n", err);
        }
        api.ding_destroy();
        closeLibrary(lib);
        return 1;
    }
    std::printf("  ding_load_rom() returned DING_OK.\n");

    // Print ROM identity if available.
    const DingRomIdentity* identity = api.ding_get_rom_identity();
    if (identity) {
        std::printf("  ROM identity: ");
        for (int i = 0; i < 16; i++) std::printf("%02x", identity->hash[i]);
        std::printf("\n");
    }

    // Print video info so we know what we're about to dump.
    const DingVideoInfo* video = api.ding_get_video_info();
    if (!video) {
        std::fprintf(stderr, "ding_get_video_info() returned NULL — cannot dump framebuffer.\n");
        api.ding_destroy();
        closeLibrary(lib);
        return 1;
    }
    std::printf("\nVideo info: %ux%u (max %ux%u), format=%d, dynamic=%u\n",
                 video->base_width, video->base_height,
                 video->max_width, video->max_height,
                 static_cast<int>(video->format), video->dynamic);

    // Run frames.
    std::printf("\nRunning %d frame(s)...\n", frameCount);
    for (int i = 0; i < frameCount; i++) {
        api.ding_run_frame();

        if (api.ding_has_error && api.ding_has_error()) {
            std::fprintf(stderr, "Core reported an error on frame %d.\n", i);
            if (api.ding_diag_last_error) {
                const char* err = api.ding_diag_last_error();
                if (err) std::fprintf(stderr, "  Core error: %s\n", err);
            }
            break;
        }
    }
    std::printf("Done.\n");

    // Dump the final framebuffer.
    uint32_t curWidth = video->base_width;
    uint32_t curHeight = video->base_height;
    if (video->dynamic && api.ding_get_current_dimensions) {
        api.ding_get_current_dimensions(&curWidth, &curHeight);
    }

    const uint8_t* fb = api.ding_get_framebuffer();
    const char* outPath = "hangar_output.ppm";
    if (writeFramebufferPPM(outPath, fb, curWidth, curHeight, video->format)) {
        std::printf("\nFramebuffer written to %s (%ux%u)\n", outPath, curWidth, curHeight);
    } else {
        std::fprintf(stderr, "\nFailed to write framebuffer.\n");
    }

    api.ding_destroy();
    closeLibrary(lib);

    return 0;
}
