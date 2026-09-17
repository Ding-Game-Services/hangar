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
#include <cmath>
#include <cctype>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <array>
#include <utility>

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

/* Reads one byte from a memory region regardless of its access mode.
 * Returns 0 for opaque regions (caller should check access before
 * trusting the value is meaningful, not just "zero"). */
static uint8_t readRegionByte(const DingMemoryRegion& region, uint32_t offset) {
    switch (region.access) {
        case DING_MEM_DIRECT:
            return region.ptr ? region.ptr[offset] : 0;
        case DING_MEM_MANAGED:
            return region.read8 ? region.read8(region.base_addr + offset) : 0;
        case DING_MEM_OPAQUE:
        default:
            return 0;
    }
}

/* Heuristic name -> SDL_GameControllerButton mapping for typical retro
 * button names ("A", "B", "Up", "Start", ...). This is intentionally
 * simple — not a full remapper — since most cores declare inputs with
 * plain, conventional names. Returns false if nothing matched, leaving
 * that input under manual/checkbox control only. */
static bool mapButtonNameToSDL(const char* rawName, SDL_GameControllerButton& out) {
    if (!rawName) return false;
    std::string n(rawName);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });

    if (n == "a") { out = SDL_CONTROLLER_BUTTON_A; return true; }
    if (n == "b") { out = SDL_CONTROLLER_BUTTON_B; return true; }
    if (n == "x") { out = SDL_CONTROLLER_BUTTON_X; return true; }
    if (n == "y") { out = SDL_CONTROLLER_BUTTON_Y; return true; }
    if (n.find("start") != std::string::npos)  { out = SDL_CONTROLLER_BUTTON_START; return true; }
    if (n.find("select") != std::string::npos) { out = SDL_CONTROLLER_BUTTON_BACK;  return true; }
    if (n.find("up") != std::string::npos)     { out = SDL_CONTROLLER_BUTTON_DPAD_UP;    return true; }
    if (n.find("down") != std::string::npos)   { out = SDL_CONTROLLER_BUTTON_DPAD_DOWN;  return true; }
    if (n.find("left") != std::string::npos && n.find("shoulder") == std::string::npos) {
        out = SDL_CONTROLLER_BUTTON_DPAD_LEFT; return true;
    }
    if (n.find("right") != std::string::npos && n.find("shoulder") == std::string::npos) {
        out = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; return true;
    }
    if (n.find("l shoulder") != std::string::npos || n == "l" || n.find("l1") != std::string::npos) {
        out = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; return true;
    }
    if (n.find("r shoulder") != std::string::npos || n == "r" || n.find("r1") != std::string::npos) {
        out = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; return true;
    }
    return false;
}

/* Heuristic platform-name -> accepted ROM extensions, since ding_core.h
 * doesn't declare a "supported extensions" field. Unrecognized platform
 * strings return an empty list, which callers should treat as "no filter,
 * show everything" — this must never accidentally hide a valid ROM just
 * because a newer/unusual core's platform name isn't in this table yet. */
static std::vector<std::string> extensionsForPlatform(const std::string& platformName) {
    std::string p = platformName;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return std::tolower(c); });

    if (p.find("game boy advance") != std::string::npos) return {".gba"};
    if (p.find("game boy") != std::string::npos)          return {".gb", ".gbc"};
    if (p.find("super nintendo") != std::string::npos ||
        p.find("super famicom") != std::string::npos)     return {".sfc", ".smc"};
    if (p.find("entertainment system") != std::string::npos) return {".nes"}; // NES (checked after SNES above)
    if (p.find("2600") != std::string::npos)               return {".a26", ".bin"};
    if (p.find("genesis") != std::string::npos ||
        p.find("mega drive") != std::string::npos)         return {".md", ".gen", ".bin"};

    return {}; // unrecognized — don't filter
}

/* Draws one button per file, wrapping to a new line instead of overflowing
 * the window — the standard ImGui "wrapping button row" pattern, estimating
 * whether the *next* button will fit before deciding to stay on this line. */
static void drawWrappingFileButtons(const std::vector<fs::path>& files,
                                     const std::function<void(const fs::path&)>& onClick,
                                     int idBase) {
    ImGuiStyle& style = ImGui::GetStyle();
    float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    for (size_t i = 0; i < files.size(); i++) {
        ImGui::PushID(idBase + static_cast<int>(i));
        std::string label = files[i].filename().string();
        if (ImGui::Button(label.c_str())) onClick(files[i]);
        float lastButtonX2 = ImGui::GetItemRectMax().x;
        ImGui::PopID();

        if (i + 1 < files.size()) {
            std::string nextLabel = files[i + 1].filename().string();
            float nextButtonWidth = ImGui::CalcTextSize(nextLabel.c_str()).x + style.FramePadding.x * 2.0f;
            float nextButtonX2 = lastButtonX2 + style.ItemSpacing.x + nextButtonWidth;
            if (nextButtonX2 < windowVisibleX2) ImGui::SameLine();
        }
    }
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
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
    fs::path biosDir  = basePath / "bios";

    std::vector<fs::path> coreFiles = scanDirectory(coresDir, kCoreExtension);
    std::vector<fs::path> romFiles;   // rescanned once a core tells us nothing about extensions,
                                       // so for now this lists every file in roms/ regardless of type
    std::vector<fs::path> biosFiles;  // same approach — BIOS file extensions vary too widely to filter

    auto rescanRoms = [&]() {
        romFiles.clear();
        if (fs::exists(romsDir) && fs::is_directory(romsDir)) {
            for (const auto& entry : fs::directory_iterator(romsDir)) {
                if (entry.is_regular_file()) romFiles.push_back(entry.path());
            }
        }
    };
    rescanRoms();

    auto rescanBios = [&]() {
        biosFiles.clear();
        if (fs::exists(biosDir) && fs::is_directory(biosDir)) {
            for (const auto& entry : fs::directory_iterator(biosDir)) {
                if (entry.is_regular_file()) biosFiles.push_back(entry.path());
            }
        }
    };
    rescanBios();

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

    // Panel visibility — toggled from the Window menu
    bool showCoreRom      = true;
    bool showRunControls  = true;
    bool showVideoOutput  = true;
    bool showMemory       = true;
    bool showDiagnostics  = true;
    bool showInput        = true;
    bool showAudio        = true;
    bool showBios         = true;
    bool showAllRomFiles  = false; // bypass the platform-extension filter
    bool showSystemDiag   = true;
    bool resetLayoutRequested = false;

    // Diagnostics state
    std::vector<std::string> eventLog;
    auto logEvent = [&](const std::string& msg) {
        Uint32 ms = SDL_GetTicks();
        char line[512];
        std::snprintf(line, sizeof(line), "[%6u.%03us] %s", ms / 1000, ms % 1000, msg.c_str());
        eventLog.push_back(line);
        if (eventLog.size() > 300) eventLog.erase(eventLog.begin()); // cap growth
    };

    bool liveUpdateDiagText = true;
    std::vector<char> cpuStateBuf(4096, '\0');
    std::vector<char> videoStateBuf(4096, '\0');

    // Frame timing — rolling average over the last N real ding_run_frame() calls
    Uint64 perfFreq = SDL_GetPerformanceFrequency();
    Uint64 lastFrameCounter = 0;
    bool haveLastFrameCounter = false;
    std::vector<double> recentFrameTimesMs;
    double measuredFps = 0.0;

    std::string saveStateTestResult;

    // Input testing state — persists across frames, indexed by input descriptor index
    std::vector<uint8_t> buttonHeld;   // manual checkbox state only — never written by controller code
    std::vector<int16_t> axisValue;    // only meaningful for AXIS type
    std::vector<uint8_t> controllerPressed; // recomputed fresh every frame, display-only + OR'd at send time

    // Audio diagnostics state
    std::vector<float> audioScratch;   // reused each frame to drain the core's ring buffer
    std::vector<float> audioWaveform;  // rolling mono-mixed history for the level display
    float audioPeakLevel = 0.0f;

    // Audio playback state
    SDL_AudioDeviceID audioDevice = 0;
    uint32_t audioDeviceSampleRate = 0;
    uint32_t audioDeviceChannels = 0;
    bool audioOutputEnabled = true;
    float audioVolume = 0.5f;

    // Physical controller state
    SDL_GameController* activeController = nullptr;
    std::vector<int> availableControllerIndices;
    auto rescanControllers = [&]() {
        availableControllerIndices.clear();
        int n = SDL_NumJoysticks();
        for (int i = 0; i < n; i++) {
            if (SDL_IsGameController(i)) availableControllerIndices.push_back(i);
        }
    };
    rescanControllers();
    int stepOneFrame = 0;          // set to N to advance N frames this iteration, then reset to 0

    // Memory viewer state
    int selectedMemoryRegion = -1;

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
            if (event.type == SDL_CONTROLLERDEVICEREMOVED && activeController) {
                SDL_Joystick* j = SDL_GameControllerGetJoystick(activeController);
                if (j && SDL_JoystickInstanceID(j) == event.cdevice.which) {
                    SDL_GameControllerClose(activeController);
                    activeController = nullptr;
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Advance the core, if running, before drawing UI so the frame we
        // display this iteration reflects it.
        if (currentCore.loaded && currentCore.romLoaded) {
            // Feed manual input state to the core every frame, regardless of
            // whether the Input window is open, so held buttons/axes keep
            // taking effect while the panel is hidden.
            if (currentCore.api.ding_get_input_descriptor_count && currentCore.api.ding_get_input_descriptor) {
                uint32_t inputCount = currentCore.api.ding_get_input_descriptor_count();
                if (buttonHeld.size() != inputCount) buttonHeld.resize(inputCount, 0);
                if (axisValue.size() != inputCount) axisValue.resize(inputCount, 0);
                if (controllerPressed.size() != inputCount) controllerPressed.resize(inputCount, 0);

                for (uint32_t i = 0; i < inputCount; i++) {
                    DingInputDescriptor desc;
                    currentCore.api.ding_get_input_descriptor(i, &desc);

                    // Controller state is recomputed fresh every frame and never
                    // written into buttonHeld (the manual checkbox's own state) —
                    // otherwise a press would permanently latch the checkbox on
                    // with no way for a release to undo it. What's actually sent
                    // to the core is manual OR controller, computed each frame.
                    controllerPressed[i] = 0;
                    if (activeController && desc.type == DING_INPUT_BUTTON) {
                        SDL_GameControllerButton sdlButton;
                        if (mapButtonNameToSDL(desc.name, sdlButton) &&
                            SDL_GameControllerGetButton(activeController, sdlButton)) {
                            controllerPressed[i] = 1;
                        }
                    }

                    uint8_t effective = (buttonHeld[i] || controllerPressed[i]) ? 1 : 0;

                    if (desc.type == DING_INPUT_BUTTON && currentCore.api.ding_set_button) {
                        currentCore.api.ding_set_button(desc.port, desc.index, effective);
                    } else if (desc.type == DING_INPUT_AXIS && currentCore.api.ding_set_axis) {
                        currentCore.api.ding_set_axis(desc.port, desc.index, axisValue[i]);
                    }
                }
            }

            bool advanced = false;
            if (coreRunning) {
                currentCore.api.ding_run_frame();
                advanced = true;
            } else if (stepOneFrame > 0) {
                currentCore.api.ding_run_frame();
                stepOneFrame--;
                advanced = true;
            }

            if (advanced) {
                Uint64 now = SDL_GetPerformanceCounter();
                if (haveLastFrameCounter) {
                    double ms = static_cast<double>(now - lastFrameCounter) * 1000.0 / static_cast<double>(perfFreq);
                    recentFrameTimesMs.push_back(ms);
                    if (recentFrameTimesMs.size() > 60) recentFrameTimesMs.erase(recentFrameTimesMs.begin());

                    double sum = 0.0;
                    for (double t : recentFrameTimesMs) sum += t;
                    double avgMs = sum / static_cast<double>(recentFrameTimesMs.size());
                    measuredFps = avgMs > 0.0 ? 1000.0 / avgMs : 0.0;
                }
                lastFrameCounter = now;
                haveLastFrameCounter = true;

                // Drain the audio ring buffer every frame regardless of
                // whether the Audio window is open — a core with a naive
                // append-only buffer could otherwise grow unbounded while
                // the panel is hidden.
                if (currentCore.api.ding_get_audio_sample_count && currentCore.api.ding_read_audio_samples) {
                    const DingAudioInfo* audioInfo = currentCore.api.ding_get_audio_info
                                                          ? currentCore.api.ding_get_audio_info()
                                                          : nullptr;
                    uint32_t channels = (audioInfo && audioInfo->channels > 0) ? audioInfo->channels : 1;
                    uint32_t sampleRate = (audioInfo && audioInfo->sample_rate > 0) ? audioInfo->sample_rate : 44100;
                    uint32_t available = currentCore.api.ding_get_audio_sample_count();

                    // (Re)open the playback device if this is the first time, or the
                    // core's reported format changed (e.g. a different ROM/core loaded).
                    if (audioInfo && (!audioDevice || audioDeviceSampleRate != sampleRate || audioDeviceChannels != channels)) {
                        if (audioDevice) {
                            SDL_CloseAudioDevice(audioDevice);
                            audioDevice = 0;
                        }
                        SDL_AudioSpec desired{};
                        desired.freq = static_cast<int>(sampleRate);
                        desired.format = AUDIO_F32SYS;
                        desired.channels = static_cast<Uint8>(channels);
                        desired.samples = 1024;
                        SDL_AudioSpec obtained{};
                        // Flags = 0: demand the exact format rather than letting SDL
                        // silently substitute one, since we're feeding it raw core output.
                        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
                        if (audioDevice) {
                            audioDeviceSampleRate = sampleRate;
                            audioDeviceChannels = channels;
                            SDL_PauseAudioDevice(audioDevice, 0);
                            logEvent("Opened audio device: " + std::to_string(sampleRate) +
                                     " Hz, " + std::to_string(channels) + " channel(s)");
                        } else {
                            logEvent(std::string("Failed to open audio device: ") + SDL_GetError());
                        }
                    }

                    if (available > 0) {
                        audioScratch.resize(static_cast<size_t>(available) * channels);
                        uint32_t got = currentCore.api.ding_read_audio_samples(audioScratch.data(), available);

                        if (audioOutputEnabled && audioDevice && got > 0) {
                            if (audioVolume != 1.0f) {
                                for (size_t s = 0; s < static_cast<size_t>(got) * channels; s++) {
                                    audioScratch[s] *= audioVolume;
                                }
                            }
                            SDL_QueueAudio(audioDevice, audioScratch.data(),
                                           static_cast<Uint32>(got * channels * sizeof(float)));
                        }

                        for (uint32_t s = 0; s < got; s++) {
                            // Mono-mixdown across channels for a simple waveform/level display.
                            float mixed = 0.0f;
                            for (uint32_t c = 0; c < channels; c++) mixed += audioScratch[s * channels + c];
                            mixed /= static_cast<float>(channels);

                            audioWaveform.push_back(mixed);
                            audioPeakLevel = std::max(audioPeakLevel * 0.95f, std::fabs(mixed)); // slow-decay peak
                        }
                        const size_t maxWaveformSamples = 2048;
                        if (audioWaveform.size() > maxWaveformSamples) {
                            audioWaveform.erase(audioWaveform.begin(),
                                                 audioWaveform.begin() + (audioWaveform.size() - maxWaveformSamples));
                        }
                    }
                }
            }

            if (currentCore.api.ding_has_error && currentCore.api.ding_has_error()) {
                coreRunning = false;
                stepOneFrame = 0;
                statusMessage = "Core reported an error.";
                std::string errMsg = "Core reported an error.";
                if (currentCore.api.ding_diag_last_error) {
                    const char* err = currentCore.api.ding_diag_last_error();
                    if (err) {
                        statusMessage += std::string(" ") + err;
                        errMsg += std::string(" ") + err;
                    }
                }
                logEvent(errMsg);
            }
        } else {
            haveLastFrameCounter = false; // don't let a stale delta carry over across loads
            recentFrameTimesMs.clear();
            measuredFps = 0.0;
        }

        if (resetLayoutRequested) {
            // Wipes remembered dock positions/sizes so windows fall back to
            // their default (overlapping, top-left) placement — a simple
            // escape hatch for when the layout gets dragged somewhere awkward.
            // Windows will need to be re-arranged by hand afterward.
            ImGui::LoadIniSettingsFromMemory("", 0);
            resetLayoutRequested = false;
        }

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("Window")) {
                ImGui::MenuItem("Core & ROM", nullptr, &showCoreRom);
                ImGui::MenuItem("Run Controls", nullptr, &showRunControls);
                ImGui::MenuItem("Video Output", nullptr, &showVideoOutput);
                ImGui::MenuItem("Memory", nullptr, &showMemory);
                ImGui::MenuItem("Diagnostics", nullptr, &showDiagnostics);
                ImGui::MenuItem("Input", nullptr, &showInput);
                ImGui::MenuItem("Audio", nullptr, &showAudio);
                ImGui::MenuItem("BIOS", nullptr, &showBios);
                ImGui::MenuItem("System Diagnostics", nullptr, &showSystemDiag);
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Layout")) {
                    resetLayoutRequested = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Full-window dockspace so the panels below can be dragged/resized/
        // rearranged instead of being crammed into one window.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), ImGuiDockNodeFlags_PassthruCentralNode);

        if (showCoreRom) {
        ImGui::Begin("Core & ROM", &showCoreRom);

        // ── Core selection ──
        ImGui::SeparatorText("Core");
        ImGui::TextDisabled("%s", coresDir.string().c_str());

        auto tryLoadCore = [&](const std::string& path, const std::string& displayName) {
            unloadCore(currentCore);
            coreRunning = false;
            stepOneFrame = 0;
            textureWidth = 0;
            textureHeight = 0;
            selectedMemoryRegion = -1;
            if (audioDevice) SDL_ClearQueuedAudio(audioDevice);
            audioWaveform.clear();
            audioPeakLevel = 0.0f;
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
            logEvent(statusMessage);
        };

        if (coreFiles.empty()) {
            ImGui::TextDisabled("(no %s files found in cores/)", kCoreExtension);
        }
        drawWrappingFileButtons(coreFiles, [&](const fs::path& p) {
            tryLoadCore(p.string(), p.filename().string());
        }, 0);
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
            selectedMemoryRegion = -1;
            if (audioDevice) SDL_ClearQueuedAudio(audioDevice);
            audioWaveform.clear();
            audioPeakLevel = 0.0f;
            if (!readFileBytes(path, lastRomData)) {
                statusMessage = "Could not read ROM file: " + path.string();
                logEvent(statusMessage);
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
            logEvent(statusMessage);
        };

        // Filter the ROM list to what this core's platform can actually load,
        // when the platform is recognized — falls back to showing everything
        // for a platform this heuristic doesn't know about.
        std::vector<fs::path> displayedRomFiles;
        std::vector<std::string> allowedExt;
        if (currentCore.loaded) {
            const DingCoreInfo* info = currentCore.api.ding_get_core_info
                                            ? currentCore.api.ding_get_core_info()
                                            : nullptr;
            if (info && info->platform_name) allowedExt = extensionsForPlatform(info->platform_name);
        }
        if (allowedExt.empty()) {
            displayedRomFiles = romFiles;
        } else if (showAllRomFiles) {
            displayedRomFiles = romFiles;
        } else {
            for (const auto& f : romFiles) {
                std::string ext = f.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                if (std::find(allowedExt.begin(), allowedExt.end(), ext) != allowedExt.end()) {
                    displayedRomFiles.push_back(f);
                }
            }
        }

        if (!allowedExt.empty()) {
            ImGui::Checkbox("Show all files (ignore platform filter)", &showAllRomFiles);
        }

        if (displayedRomFiles.empty()) {
            if (romFiles.empty()) {
                ImGui::TextDisabled("(no files found in roms/)");
            } else {
                ImGui::TextDisabled("(no files in roms/ match this core's platform)");
            }
        }
        drawWrappingFileButtons(displayedRomFiles, [&](const fs::path& p) {
            tryLoadRom(p);
        }, 1000);
        if (!displayedRomFiles.empty()) ImGui::NewLine();

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
        }

        // ── Run controls (separate dockable window) ──
        if (showRunControls) {
        ImGui::Begin("Run Controls", &showRunControls);
        ImGui::BeginDisabled(!currentCore.romLoaded);

        if (coreRunning) {
            if (ImGui::Button("Pause")) { coreRunning = false; logEvent("Paused."); }
        } else {
            if (ImGui::Button("Run")) { coreRunning = true; logEvent("Running."); }
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
            if (audioDevice) SDL_ClearQueuedAudio(audioDevice);
            audioWaveform.clear();
            audioPeakLevel = 0.0f;
            logEvent("Core reset (" + lastRomName + ")");
        }

        ImGui::EndDisabled();
        ImGui::End(); // Run Controls
        }

        // ── Video output (separate dockable window) ──
        if (showVideoOutput) {
        ImGui::Begin("Video Output", &showVideoOutput);

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
        }

        // ── Memory viewer (separate dockable window) ──
        // Works identically for every core using only ding_core.h — no
        // per-core code needed. Region list is re-fetched every frame since
        // it's cheap (a handful of structs) and avoids stale-pointer issues
        // if a core reallocates memory on reset/reload.
        if (showMemory) {
        ImGui::Begin("Memory", &showMemory);

        if (!currentCore.romLoaded) {
            ImGui::TextDisabled("(load a core and ROM to inspect memory)");
        } else {
            uint32_t regionCount = currentCore.api.ding_get_memory_region_count
                                        ? currentCore.api.ding_get_memory_region_count()
                                        : 0;

            std::vector<DingMemoryRegion> regions(regionCount);
            for (uint32_t i = 0; i < regionCount; i++) {
                currentCore.api.ding_get_memory_region(i, &regions[i]);
            }

            if (regionCount == 0) {
                ImGui::TextDisabled("(this core exposes no memory regions)");
            }

            // Region picker — one selectable line per region, showing name,
            // access mode, and size so it's clear what's actually inspectable.
            ImGui::BeginChild("RegionList", ImVec2(220, 200), true);
            for (uint32_t i = 0; i < regionCount; i++) {
                const DingMemoryRegion& r = regions[i];
                const char* accessLabel =
                    r.access == DING_MEM_DIRECT  ? "direct"  :
                    r.access == DING_MEM_MANAGED ? "managed" : "opaque";

                char label[256];
                std::snprintf(label, sizeof(label), "%s (%s, %zu B)",
                              r.name ? r.name : "(unnamed)", accessLabel, r.size);

                bool isSelected = (selectedMemoryRegion == static_cast<int>(i));
                if (ImGui::Selectable(label, isSelected)) {
                    selectedMemoryRegion = static_cast<int>(i);
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("HexView", ImVec2(0, 0), true);

            if (selectedMemoryRegion < 0 || selectedMemoryRegion >= static_cast<int>(regionCount)) {
                ImGui::TextDisabled("(select a region on the left)");
            } else {
                const DingMemoryRegion& region = regions[selectedMemoryRegion];

                if (region.access == DING_MEM_OPAQUE) {
                    ImGui::TextDisabled("This region is opaque — not externally accessible.");
                } else {
                    ImGui::Text("Base: 0x%08X   Size: %zu bytes   %s",
                                region.base_addr, region.size,
                                region.writable ? "(writable)" : "(read-only)");
                    ImGui::Separator();

                    const int bytesPerRow = 16;
                    int rowCount = static_cast<int>((region.size + bytesPerRow - 1) / bytesPerRow);

                    ImGuiListClipper clipper;
                    clipper.Begin(rowCount);
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                            uint32_t rowOffset = static_cast<uint32_t>(row) * bytesPerRow;

                            char lineBuf[128];
                            int pos = std::snprintf(lineBuf, sizeof(lineBuf), "%06X: ", rowOffset);

                            char asciiBuf[bytesPerRow + 1];
                            int asciiLen = 0;

                            for (int col = 0; col < bytesPerRow; col++) {
                                uint32_t offset = rowOffset + col;
                                if (offset < region.size) {
                                    uint8_t byte = readRegionByte(region, offset);
                                    pos += std::snprintf(lineBuf + pos, sizeof(lineBuf) - pos, "%02X ", byte);
                                    asciiBuf[asciiLen++] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '.';
                                } else {
                                    pos += std::snprintf(lineBuf + pos, sizeof(lineBuf) - pos, "   ");
                                    asciiBuf[asciiLen++] = ' ';
                                }
                                if (col == 7) pos += std::snprintf(lineBuf + pos, sizeof(lineBuf) - pos, " ");
                            }
                            asciiBuf[asciiLen] = '\0';

                            ImGui::TextUnformatted(lineBuf);
                            ImGui::SameLine();
                            ImGui::TextUnformatted(asciiBuf);
                        }
                    }
                }
            }

            ImGui::EndChild();
        }

        ImGui::End(); // Memory
        }

        // ── Diagnostics (separate dockable window) ──
        // Everything here is generic — driven only by ding_core.h — so it
        // works the same for every core with zero per-core code. Deeper
        // per-core internals belong in a future hangar.cpp extension file,
        // not here.
        if (showDiagnostics) {
        ImGui::Begin("Diagnostics", &showDiagnostics);

        ImGui::SeparatorText("Frame Timing");
        if (coreRunning) {
            ImGui::Text("Measured: %.1f fps (avg over last %zu frames)",
                        measuredFps, recentFrameTimesMs.size());
            ImGui::TextDisabled("Tied to Run's per-UI-frame stepping, not the system's real clock — "
                                 "useful for spotting slowdowns, not yet frame-accurate.");
        } else {
            ImGui::TextDisabled("(only measured while Run is active)");
        }

        ImGui::SeparatorText("Core-Reported State");
        ImGui::BeginDisabled(!currentCore.romLoaded);
        ImGui::Checkbox("Live update", &liveUpdateDiagText);

        bool wantRefresh = liveUpdateDiagText;
        ImGui::SameLine();
        if (ImGui::Button("Refresh now")) wantRefresh = true;

        if (currentCore.romLoaded && wantRefresh) {
            if (currentCore.api.ding_diag_cpu_state) {
                currentCore.api.ding_diag_cpu_state(cpuStateBuf.data(), cpuStateBuf.size());
            }
            if (currentCore.api.ding_diag_video_state) {
                currentCore.api.ding_diag_video_state(videoStateBuf.data(), videoStateBuf.size());
            }
        }

        ImGui::Text("CPU state:");
        if (!currentCore.romLoaded) {
            ImGui::TextDisabled("(no ROM loaded)");
        } else if (!currentCore.api.ding_diag_cpu_state) {
            ImGui::TextDisabled("(not implemented by this core)");
        } else {
            ImGui::InputTextMultiline("##cpuState", cpuStateBuf.data(), cpuStateBuf.size(),
                                       ImVec2(-1, 100), ImGuiInputTextFlags_ReadOnly);
        }

        ImGui::Text("Video state:");
        if (!currentCore.romLoaded) {
            ImGui::TextDisabled("(no ROM loaded)");
        } else if (!currentCore.api.ding_diag_video_state) {
            ImGui::TextDisabled("(not implemented by this core)");
        } else {
            ImGui::InputTextMultiline("##videoState", videoStateBuf.data(), videoStateBuf.size(),
                                       ImVec2(-1, 100), ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText("Save State Round-Trip Test");
        ImGui::BeginDisabled(!currentCore.romLoaded);
        ImGui::TextWrapped("Saves state, then immediately loads it back, to catch a core "
                            "crashing or corrupting its own save/load round trip.");
        if (ImGui::Button("Run Round-Trip Test") && currentCore.romLoaded) {
            const DingSaveStateInfo* info = currentCore.api.ding_get_savestate_info
                                                 ? currentCore.api.ding_get_savestate_info()
                                                 : nullptr;
            if (!info || !info->supported || info->method == DING_SAVE_UNSUPPORTED) {
                saveStateTestResult = "This core does not report save state support.";
            } else {
                // max_size of 0 means "unknown/very large" per ding_core.h —
                // fall back to a generous buffer in that case.
                size_t bufSize = info->max_size > 0 ? info->max_size : (8u * 1024u * 1024u);
                std::vector<uint8_t> buf(bufSize);

                size_t written = currentCore.api.ding_save_state(buf.data(), buf.size());
                if (written == 0) {
                    saveStateTestResult = "ding_save_state() wrote 0 bytes — failed.";
                } else {
                    DingResult loadResult = currentCore.api.ding_load_state(buf.data(), written);
                    if (loadResult == DING_OK) {
                        saveStateTestResult = "Pass — saved " + std::to_string(written) +
                                               " bytes and loaded them back successfully.";
                    } else {
                        saveStateTestResult = "FAIL — ding_load_state() returned error code " +
                                               std::to_string(loadResult) + " after a " +
                                               std::to_string(written) + "-byte save.";
                    }
                }
            }
            logEvent("Save-state round-trip test: " + saveStateTestResult);
        }
        if (!saveStateTestResult.empty()) {
            ImGui::TextWrapped("%s", saveStateTestResult.c_str());
        }
        ImGui::EndDisabled();

        ImGui::SeparatorText("Event Log");
        if (ImGui::Button("Clear Log")) eventLog.clear();
        ImGui::BeginChild("EventLog", ImVec2(0, 150), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : eventLog) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (!eventLog.empty()) {
            ImGui::SetScrollHereY(1.0f); // auto-scroll to the latest entry
        }
        ImGui::EndChild();

        ImGui::End(); // Diagnostics
        }

        // ── Input testing (separate dockable window) ──
        // Lets buttons/axes be exercised without a physical controller.
        // Descriptors are declared at ding_init(), per ding_core.h, so this
        // is available as soon as a core is loaded — not gated on a ROM.
        if (showInput) {
        ImGui::Begin("Input", &showInput);

        ImGui::SeparatorText("Physical Controller");
        if (activeController) {
            ImGui::Text("Connected: %s", SDL_GameControllerName(activeController));
            if (ImGui::Button("Disconnect")) {
                SDL_GameControllerClose(activeController);
                activeController = nullptr;
            }
        } else {
            if (ImGui::Button("Scan for controllers")) rescanControllers();
            ImGui::SameLine();
            ImGui::TextDisabled("%zu found", availableControllerIndices.size());
            for (int idx : availableControllerIndices) {
                ImGui::PushID(idx);
                if (ImGui::Button(SDL_GameControllerNameForIndex(idx))) {
                    activeController = SDL_GameControllerOpen(idx);
                }
                ImGui::PopID();
                ImGui::SameLine();
            }
            if (!availableControllerIndices.empty()) ImGui::NewLine();
        }
        ImGui::TextDisabled("Mapping is a simple name heuristic (A/B/Up/Down/Left/Right/Start/"
                             "Select/shoulders). A green \"ctrl\" dot means the controller is "
                             "currently pressing that input; it's OR'd with the checkbox live and "
                             "releases cleanly — it does not latch the checkbox on.");

        ImGui::SeparatorText("Manual / Combined State");

        if (!currentCore.loaded) {
            ImGui::TextDisabled("(load a core to see its input descriptors)");
        } else if (!currentCore.api.ding_get_input_descriptor_count || !currentCore.api.ding_get_input_descriptor) {
            ImGui::TextDisabled("(this core does not report input descriptors)");
        } else {
            uint32_t inputCount = currentCore.api.ding_get_input_descriptor_count();
            if (buttonHeld.size() != inputCount) buttonHeld.resize(inputCount, 0);
            if (axisValue.size() != inputCount) axisValue.resize(inputCount, 0);

            if (inputCount == 0) {
                ImGui::TextDisabled("(no inputs declared)");
            }

            for (uint32_t i = 0; i < inputCount; i++) {
                DingInputDescriptor desc;
                currentCore.api.ding_get_input_descriptor(i, &desc);

                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("[Port %u] %s", desc.port, desc.name ? desc.name : "(unnamed)");
                ImGui::SameLine(220);

                if (desc.type == DING_INPUT_BUTTON) {
                    bool held = buttonHeld[i] != 0;
                    if (ImGui::Checkbox("Held", &held)) buttonHeld[i] = held ? 1 : 0;

                    if (i < controllerPressed.size() && controllerPressed[i]) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "\xE2\x97\x8F ctrl"); // filled circle
                    }
                } else if (desc.type == DING_INPUT_AXIS) {
                    int value = axisValue[i];
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::SliderInt("Axis", &value, -32768, 32767)) {
                        axisValue[i] = static_cast<int16_t>(value);
                    }
                } else {
                    ImGui::TextDisabled("(motion input testing not yet implemented)");
                }
                ImGui::PopID();
            }

            ImGui::Spacing();
            ImGui::TextDisabled("Values here are sent to the core every frame while it's running, "
                                 "whether or not this window is visible.");
        }

        ImGui::End(); // Input
        }

        // ── Audio diagnostics (separate dockable window) ──
        // Drains and mixes down the core's audio ring buffer every frame
        // (see the core-advance block above) purely to confirm output is
        // non-silent — this does not play sound through the system yet.
        if (showAudio) {
        ImGui::Begin("Audio", &showAudio);

        if (!currentCore.romLoaded) {
            ImGui::TextDisabled("(load a core and ROM to see audio output)");
        } else {
            const DingAudioInfo* audioInfo = currentCore.api.ding_get_audio_info
                                                  ? currentCore.api.ding_get_audio_info()
                                                  : nullptr;
            if (audioInfo) {
                ImGui::Text("Sample rate: %u Hz    Channels: %u", audioInfo->sample_rate, audioInfo->channels);
            } else {
                ImGui::TextDisabled("(this core does not report audio info)");
            }

            ImGui::Checkbox("Enable audio output", &audioOutputEnabled);
            ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("Volume", &audioVolume, 0.0f, 1.0f);

            ImGui::Text("Device: %s", audioDevice ? "open" : "not open");

            ImGui::Text("Peak level:");
            ImGui::SameLine();
            ImGui::ProgressBar(std::min(audioPeakLevel, 1.0f), ImVec2(200, 0));

            if (!audioWaveform.empty()) {
                ImGui::PlotLines("##waveform", audioWaveform.data(), static_cast<int>(audioWaveform.size()),
                                  0, nullptr, -1.0f, 1.0f, ImVec2(-1, 100));
            } else {
                ImGui::TextDisabled("(no audio samples produced yet — try Run)");
            }
        }

        ImGui::End(); // Audio
        }

        // ── BIOS loading (separate dockable window) ──
        if (showBios) {
        ImGui::Begin("BIOS", &showBios);
        ImGui::TextDisabled("%s", biosDir.string().c_str());

        if (!currentCore.loaded) {
            ImGui::TextDisabled("(load a core to see its BIOS requirements)");
        } else if (!currentCore.api.ding_get_bios_count || !currentCore.api.ding_get_bios_descriptor) {
            ImGui::TextDisabled("(this core does not report BIOS requirements)");
        } else {
            uint32_t biosCount = currentCore.api.ding_get_bios_count();

            if (ImGui::Button("Rescan bios/")) rescanBios();
            ImGui::Spacing();

            if (biosCount == 0) {
                ImGui::TextDisabled("(this core requires no BIOS files)");
            }

            for (uint32_t i = 0; i < biosCount; i++) {
                DingBiosDescriptor desc;
                currentCore.api.ding_get_bios_descriptor(i, &desc);

                ImGui::PushID(static_cast<int>(1000 + i));
                ImGui::SeparatorText(desc.name ? desc.name : "(unnamed BIOS)");
                ImGui::Text("Expected filename: %s   Size: %u bytes   %s",
                            desc.filename ? desc.filename : "(none)", desc.size,
                            desc.required ? "REQUIRED" : "optional");
                ImGui::Text("Status: %s", desc.loaded ? "loaded" : "not loaded");

                // Offer every file currently in bios/ as a candidate for this slot —
                // Hangar doesn't try to match filenames automatically since the
                // descriptor's expected filename is only a hint, not a guarantee.
                for (size_t f = 0; f < biosFiles.size(); f++) {
                    ImGui::PushID(static_cast<int>(f));
                    if (ImGui::Button(biosFiles[f].filename().string().c_str())) {
                        std::vector<uint8_t> biosData;
                        if (readFileBytes(biosFiles[f], biosData)) {
                            DingResult result = currentCore.api.ding_load_bios(i, biosData.data(), biosData.size());
                            std::string msg = "ding_load_bios(" + std::to_string(i) + ", " +
                                               biosFiles[f].filename().string() + "): " +
                                               (result == DING_OK ? "OK" : "failed (code " + std::to_string(result) + ")");
                            statusMessage = msg;
                            logEvent(msg);
                        } else {
                            statusMessage = "Could not read BIOS file: " + biosFiles[f].string();
                            logEvent(statusMessage);
                        }
                    }
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                if (!biosFiles.empty()) ImGui::NewLine();
                ImGui::PopID();
            }
        }

        ImGui::End(); // BIOS
        }

        // ── System Diagnostics (separate dockable window) ──
        // Generic renderer for whatever a core's own hangar.cpp exposes via
        // hangar_get_entry_count()/hangar_get_entry() — see
        // hangar_ext_template.h for the contract. Hangar has zero knowledge
        // of what these entries mean; it only groups by category string and
        // displays name/value pairs.
        if (showSystemDiag) {
        ImGui::Begin("System Diagnostics", &showSystemDiag);

        if (!currentCore.loaded) {
            ImGui::TextDisabled("(load a core to see its system-specific diagnostics)");
        } else if (!currentCore.hasHangarExt) {
            ImGui::TextWrapped("This core hasn't implemented Hangar's extension diagnostics yet "
                                "(hangar_get_entry_count/hangar_get_entry — see hangar_ext_template.h). "
                                "This is separate from ding_core.h and entirely optional.");
        } else {
            static bool liveUpdateSystemDiag = true;
            ImGui::Checkbox("Live update", &liveUpdateSystemDiag);
            ImGui::SameLine();
            bool refreshNow = ImGui::Button("Refresh now");

            static std::vector<std::array<std::string, 3>> cachedEntries; // {category, name, value}
            if (liveUpdateSystemDiag || refreshNow) {
                cachedEntries.clear();
                uint32_t count = currentCore.hangar_get_entry_count();

                const uint32_t bufSize = 256;
                char categoryBuf[bufSize], nameBuf[bufSize], valueBuf[bufSize];

                for (uint32_t i = 0; i < count; i++) {
                    categoryBuf[0] = nameBuf[0] = valueBuf[0] = '\0';
                    currentCore.hangar_get_entry(i, categoryBuf, nameBuf, valueBuf, bufSize);
                    // Defensive: force null-termination regardless of what the
                    // core wrote, in case a hangar.cpp implementation is buggy.
                    categoryBuf[bufSize - 1] = nameBuf[bufSize - 1] = valueBuf[bufSize - 1] = '\0';
                    cachedEntries.push_back({categoryBuf, nameBuf, valueBuf});
                }
            }

            if (cachedEntries.empty()) {
                ImGui::TextDisabled("(this core reports zero entries right now)");
            } else {
                // Group by category, preserving first-seen order.
                std::vector<std::string> categoryOrder;
                std::map<std::string, std::vector<std::pair<std::string, std::string>>> byCategory;
                for (const auto& entry : cachedEntries) {
                    const std::string& cat = entry[0];
                    if (byCategory.find(cat) == byCategory.end()) categoryOrder.push_back(cat);
                    byCategory[cat].push_back({entry[1], entry[2]});
                }

                for (const auto& cat : categoryOrder) {
                    if (ImGui::CollapsingHeader(cat.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (ImGui::BeginTable(cat.c_str(), 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                            ImGui::TableSetupColumn("Value");
                            ImGui::TableHeadersRow();
                            for (const auto& [name, value] : byCategory[cat]) {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextUnformatted(name.c_str());
                                ImGui::TableSetColumnIndex(1);
                                ImGui::TextUnformatted(value.c_str());
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }
        }

        ImGui::End(); // System Diagnostics
        }

        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x), static_cast<int>(io.DisplaySize.y));
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        SDL_GL_SwapWindow(window);
    }

    unloadCore(currentCore);
    if (framebufferTexture) glDeleteTextures(1, &framebufferTexture);
    if (activeController) SDL_GameControllerClose(activeController);
    if (audioDevice) SDL_CloseAudioDevice(audioDevice);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}

