/*
 * main.cpp — Hangar control panel.
 *
 * Dockable ImGui panels: Core & ROM (loading), Run Controls (step/run/reset),
 * Video Output (live framebuffer view). Built on SDL2 + OpenGL3 + Dear ImGui,
 * all fetched automatically via CMake FetchContent.
 */

#include <SDL.h>
#include <SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include "hangar_core.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/* Platform-specific extension for core libraries. */
#if defined(_WIN32)
    static const char* kCoreExtension = ".dll";
#else
    static const char* kCoreExtension = ".so";
#endif

/* Scans a directory (non-recursive) for files with the given extension.
 * Returns an empty list if the directory doesn't exist — that's not an
 * error, it just means nothing has been dropped in there yet. */
static std::vector<fs::path> scanDirectory(const fs::path& dir, const char* extension) {
    std::vector<fs::path> results;
    if (!fs::exists(dir) || !fs::is_directory(dir)) return results;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == extension) {
            results.push_back(entry.path());
        }
    }
    return results;
}

/* Reads an entire file into memory. Returns true on success. */
static bool readFileBytes(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    std::streamsize size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);

    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), size));
}

/* Converts a core framebuffer to tightly-packed RGBA8 for GL upload.
 * out must be at least width*height*4 bytes. Unhandled formats (YUV420)
 * are left as opaque black rather than guessing wrong. */
static void convertFramebufferToRGBA8(const uint8_t* fb, uint32_t width, uint32_t height,
                                       DingPixelFormat format, std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(width) * height * 4);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            size_t outIdx = (static_cast<size_t>(y) * width + x) * 4;
            uint8_t r = 0, g = 0, b = 0, a = 255;

            switch (format) {
                case DING_PIXFMT_RGBA8: {
                    const uint8_t* px = fb + (static_cast<size_t>(y) * width + x) * 4;
                    r = px[0]; g = px[1]; b = px[2]; a = px[3];
                    break;
                }
                case DING_PIXFMT_XRGB8: {
                    const uint8_t* px = fb + (static_cast<size_t>(y) * width + x) * 4;
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
                    break; // left black
            }

            out[outIdx + 0] = r;
            out[outIdx + 1] = g;
            out[outIdx + 2] = b;
            out[outIdx + 3] = a;
        }
    }
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const char* glslVersion = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    SDL_WindowFlags windowFlags = static_cast<SDL_WindowFlags>(
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow(
        "Hangar - Ding Core Test Tool",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 800, windowFlags);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, glContext);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, glContext);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // ── Hangar state ──

    // "cores" and "roms" live next to the executable, not the working
    // directory, so this still works no matter where Hangar is launched from.
    char* basePathCStr = SDL_GetBasePath();
    fs::path basePath = basePathCStr ? fs::path(basePathCStr) : fs::current_path();
    if (basePathCStr) SDL_free(basePathCStr);

    fs::path coresDir = basePath / "cores";
    fs::path romsDir  = basePath / "roms";

    std::vector<fs::path> coreFiles = scanDirectory(coresDir, kCoreExtension);
    std::vector<fs::path> romFiles;   // rescanned once a core tells us nothing about extensions,
                                       // so for now this lists every file in roms/ regardless of type

    auto rescanRoms = [&]() {
        romFiles.clear();
        if (fs::exists(romsDir) && fs::is_directory(romsDir)) {
            for (const auto& entry : fs::directory_iterator(romsDir)) {
                if (entry.is_regular_file()) romFiles.push_back(entry.path());
            }
        }
    };
    rescanRoms();

    LoadedCore currentCore;
    char manualCorePathBuf[512] = "";
    char manualRomPathBuf[512]  = "";
    std::string statusMessage = "No core loaded.";
    std::vector<uint8_t> lastRomData;
    std::string lastRomName;

    // Live view state
    GLuint framebufferTexture = 0;
    glGenTextures(1, &framebufferTexture);
    uint32_t textureWidth = 0, textureHeight = 0;
    std::vector<uint8_t> rgbaScratch;

    bool coreRunning = false;      // continuous run/pause state
    int stepOneFrame = 0;          // set to N to advance N frames this iteration, then reset to 0

    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                running = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Advance the core, if running, before drawing UI so the frame we
        // display this iteration reflects it.
        if (currentCore.loaded && currentCore.romLoaded) {
            if (coreRunning) {
                currentCore.api.ding_run_frame();
            } else if (stepOneFrame > 0) {
                currentCore.api.ding_run_frame();
                stepOneFrame--;
            }

            if (currentCore.api.ding_has_error && currentCore.api.ding_has_error()) {
                coreRunning = false;
                stepOneFrame = 0;
                statusMessage = "Core reported an error.";
                if (currentCore.api.ding_diag_last_error) {
                    const char* err = currentCore.api.ding_diag_last_error();
                    if (err) statusMessage += std::string(" ") + err;
                }
            }
        }

        // Full-window dockspace so the panels below can be dragged/resized/
        // rearranged instead of being crammed into one window.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        ImGui::Begin("Core & ROM");

        // ── Core selection ──
        ImGui::SeparatorText("Core");
        ImGui::TextDisabled("%s", coresDir.string().c_str());

        auto tryLoadCore = [&](const std::string& path, const std::string& displayName) {
            unloadCore(currentCore);
            coreRunning = false;
            stepOneFrame = 0;
            textureWidth = 0;
            textureHeight = 0;
            currentCore = loadCore(path);
            if (currentCore.loaded) {
                currentCore.api.ding_init();
                statusMessage = "Core loaded: " + displayName;
            } else if (!currentCore.loadError.empty()) {
                statusMessage = "Failed to load library: " + currentCore.loadError;
            } else {
                statusMessage = "Core loaded but missing " +
                                 std::to_string(currentCore.missingRequired.size()) +
                                 " required symbol(s).";
            }
        };

        if (coreFiles.empty()) {
            ImGui::TextDisabled("(no %s files found in cores/)", kCoreExtension);
        }
        for (size_t i = 0; i < coreFiles.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Button(coreFiles[i].filename().string().c_str())) {
                tryLoadCore(coreFiles[i].string(), coreFiles[i].filename().string());
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (!coreFiles.empty()) ImGui::NewLine();

        if (ImGui::Button("Rescan cores/")) {
            coreFiles = scanDirectory(coresDir, kCoreExtension);
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Or load from a specific path:");
        ImGui::SetNextItemWidth(-120);
        ImGui::InputText("##manualCorePath", manualCorePathBuf, sizeof(manualCorePathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Load##loadManualCore") && manualCorePathBuf[0] != '\0') {
            fs::path p(manualCorePathBuf);
            tryLoadCore(p.string(), p.filename().string());
        }

        // ── Core status ──
        ImGui::Spacing();
        ImGui::TextWrapped("%s", statusMessage.c_str());

        if (!currentCore.missingRequired.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Missing required symbols:");
            for (auto& name : currentCore.missingRequired) {
                ImGui::BulletText("%s", name.c_str());
            }
        }
        if (!currentCore.missingOptional.empty()) {
            ImGui::TextDisabled("Missing optional diagnostics: %zu (not a problem)",
                                 currentCore.missingOptional.size());
        }

        if (currentCore.loaded) {
            const DingCoreInfo* info = currentCore.api.ding_get_core_info();
            if (info) {
                ImGui::Spacing();
                ImGui::Text("Name:     %s", info->core_name ? info->core_name : "(null)");
                ImGui::Text("Platform: %s", info->platform_name ? info->platform_name : "(null)");
                ImGui::Text("Version:  %s", info->version ? info->version : "(null)");
                ImGui::Text("API:      %u.%u", info->api_version_major, info->api_version_minor);
            }
        }

        // ── ROM selection (only meaningful once a core is loaded) ──
        ImGui::SeparatorText("ROM");
        ImGui::TextDisabled("%s", romsDir.string().c_str());

        if (!currentCore.loaded) {
            ImGui::TextDisabled("(load a core first)");
        }

        ImGui::BeginDisabled(!currentCore.loaded);

        auto tryLoadRom = [&](const fs::path& path) {
            coreRunning = false;
            stepOneFrame = 0;
            textureWidth = 0;
            textureHeight = 0;
            if (!readFileBytes(path, lastRomData)) {
                statusMessage = "Could not read ROM file: " + path.string();
                return;
            }
            DingResult result = currentCore.api.ding_load_rom(lastRomData.data(), lastRomData.size());
            if (result == DING_OK) {
                currentCore.romLoaded = true;
                lastRomName = path.filename().string();
                statusMessage = "ROM loaded: " + lastRomName;
            } else {
                currentCore.romLoaded = false;
                statusMessage = "ding_load_rom() failed (code " + std::to_string(result) + ")";
                if (currentCore.api.ding_diag_last_error) {
                    const char* err = currentCore.api.ding_diag_last_error();
                    if (err) statusMessage += std::string(" - ") + err;
                }
            }
        };

        if (romFiles.empty()) {
            ImGui::TextDisabled("(no files found in roms/)");
        }
        for (size_t i = 0; i < romFiles.size(); i++) {
            ImGui::PushID(static_cast<int>(1000 + i));
            if (ImGui::Button(romFiles[i].filename().string().c_str())) {
                tryLoadRom(romFiles[i]);
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        if (!romFiles.empty()) ImGui::NewLine();

        if (ImGui::Button("Rescan roms/")) {
            rescanRoms();
        }

        ImGui::Spacing();
        ImGui::TextUnformatted("Or load from a specific path:");
        ImGui::SetNextItemWidth(-120);
        ImGui::InputText("##manualRomPath", manualRomPathBuf, sizeof(manualRomPathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Load##loadManualRom") && manualRomPathBuf[0] != '\0') {
            tryLoadRom(fs::path(manualRomPathBuf));
        }

        ImGui::EndDisabled();

        if (currentCore.romLoaded) {
            ImGui::Spacing();
            const DingRomIdentity* identity = currentCore.api.ding_get_rom_identity();
            if (identity) {
                ImGui::Text("ROM MD5: ");
                ImGui::SameLine();
                char hashStr[33];
                for (int i = 0; i < 16; i++) std::snprintf(hashStr + i * 2, 3, "%02x", identity->hash[i]);
                ImGui::TextUnformatted(hashStr);
            }
        }

        ImGui::End(); // Core & ROM

        // ── Run controls (separate dockable window) ──
        ImGui::Begin("Run Controls");
        ImGui::BeginDisabled(!currentCore.romLoaded);

        if (coreRunning) {
            if (ImGui::Button("Pause")) coreRunning = false;
        } else {
            if (ImGui::Button("Run")) coreRunning = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Step 1 Frame")) stepOneFrame += 1;
        ImGui::SameLine();
        if (ImGui::Button("Step 60 Frames")) stepOneFrame += 60;
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            currentCore.api.ding_reset();
            coreRunning = false;
            stepOneFrame = 0;
        }

        ImGui::EndDisabled();
        ImGui::End(); // Run Controls

        // ── Video output (separate dockable window) ──
        ImGui::Begin("Video Output");

        if (currentCore.romLoaded) {
            const DingVideoInfo* video = currentCore.api.ding_get_video_info();
            const uint8_t* fb = currentCore.api.ding_get_framebuffer();

            if (video && fb) {
                uint32_t curWidth = video->base_width;
                uint32_t curHeight = video->base_height;
                if (video->dynamic && currentCore.api.ding_get_current_dimensions) {
                    currentCore.api.ding_get_current_dimensions(&curWidth, &curHeight);
                }

                if (curWidth > 0 && curHeight > 0) {
                    convertFramebufferToRGBA8(fb, curWidth, curHeight, video->format, rgbaScratch);

                    glBindTexture(GL_TEXTURE_2D, framebufferTexture);
                    if (curWidth != textureWidth || curHeight != textureHeight) {
                        // Resolution changed (or first frame) — reallocate storage.
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, curWidth, curHeight, 0,
                                     GL_RGBA, GL_UNSIGNED_BYTE, rgbaScratch.data());
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        textureWidth = curWidth;
                        textureHeight = curHeight;
                    } else {
                        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, curWidth, curHeight,
                                        GL_RGBA, GL_UNSIGNED_BYTE, rgbaScratch.data());
                    }

                    if (video->format == DING_PIXFMT_YUV420) {
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                            "YUV420 conversion not implemented yet — image will be black.");
                    }

                    // Scale up 3x with nearest-neighbor (GL_NEAREST above) so pixel
                    // art doesn't render as a postage stamp.
                    ImVec2 displaySize(static_cast<float>(curWidth) * 3.0f,
                                       static_cast<float>(curHeight) * 3.0f);
                    ImGui::Image((ImTextureID)(intptr_t)framebufferTexture, displaySize);
                }
            }
        } else {
            ImGui::TextDisabled("(load a core and ROM to see live output)");
        }

        ImGui::End(); // Video Output

        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    unloadCore(currentCore);
    if (framebufferTexture) glDeleteTextures(1, &framebufferTexture);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

