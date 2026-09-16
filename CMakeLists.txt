cmake_minimum_required(VERSION 3.20)
project(hangar LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ─────────────────────────────────────────────────────────────────────────
# SDL2 — pinned release, built from source. No system install required.
# ─────────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    SDL2
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-2.30.9
    GIT_SHALLOW    TRUE
)
# We only need the library, not SDL2's own test/demo targets.
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# ─────────────────────────────────────────────────────────────────────────
# Dear ImGui — has no CMakeLists of its own, so we fetch the source and
# build it as a small static library ourselves (this is the normal way
# ImGui is used — it's meant to be dropped into a project, not prebuilt).
# ─────────────────────────────────────────────────────────────────────────
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        d7950e2c42cfd57020c7f9a5a90010d9c702a29f  # docking branch, pinned commit
    GIT_SHALLOW    FALSE  # a specific commit (not a branch/tag ref) needs the full history to resolve
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC SDL2::SDL2 SDL2::SDL2main)

# ─────────────────────────────────────────────────────────────────────────
# OpenGL — needed by ImGui's OpenGL3 backend.
# ─────────────────────────────────────────────────────────────────────────
find_package(OpenGL REQUIRED)

# ─────────────────────────────────────────────────────────────────────────
# Hangar itself
# ─────────────────────────────────────────────────────────────────────────

# Path to ding_core.h / ding_types.h — copy them into sdk/ next to this file.
set(DING_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sdk" CACHE PATH "Path to ding-core-sdk headers")

add_executable(hangar
    main.cpp
    hangar_core.cpp
)
target_include_directories(hangar PRIVATE ${DING_SDK_DIR})
target_link_libraries(hangar PRIVATE imgui SDL2::SDL2 SDL2::SDL2main OpenGL::GL)

if(UNIX AND NOT APPLE)
    target_link_libraries(hangar PRIVATE dl)
endif()

# SDL2 builds as a shared library (SDL2.dll / SDL2d.dll) — copy it next to
# the executable after every build so hangar.exe can actually find it.
add_custom_command(TARGET hangar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:hangar>
)

# Create cores/ and roms/ next to the executable so they exist on first run
# even before anything's been dropped into them.
add_custom_command(TARGET hangar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:hangar>/cores
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:hangar>/roms
)
