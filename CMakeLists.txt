cmake_minimum_required(VERSION 3.14)

if(POLICY CMP0092)
    # Let our explicit /W4 be the only warning flag; with the OLD behavior
    # CMake prepends /W3 and every TU prints D9025 "overriding /W3 with /W4".
    # Must run before project(): that is when CMAKE_C_FLAGS gets populated.
    cmake_policy(SET CMP0092 NEW)
endif()

project(anime_denoise
    VERSION 1.1.5
    DESCRIPTION "Anime Denoise: native CUI app, ONNX Runtime inference"
    LANGUAGES C RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CUI build; examples and tests are not part of this app.
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/CUI)
if(MSVC)
    # cui_viz.c trips C4457 (shadowed parameter) under the /W4 we impose;
    # silence it on the vendored target rather than in upstream sources.
    target_compile_options(cui PRIVATE /wd4457)
endif()

# Everything but the entry point lives in a static library, so the benchmark
# project (lab/测试工程) can drive the same UI code without a window.
add_library(anime_core STATIC
    src/app.c
    src/app_jobs.c
    src/app_input.c
    src/ui_shell.c
    src/ui_stage.c
    src/ui_float.c
    src/ui_lasso.c
    src/ui_models.c
    src/ui_fluent.c
    src/denoiser.c
    src/denoiser_models.c
    src/chrome.c
    src/config.c
    src/log.c
    src/img.c
    src/img_common.c
    src/img_clip.c
    src/loader.c
    src/mask.c
    src/update.c
    src/update_io.c)

target_include_directories(anime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Native denoise engine (Phase 2): ONNX Runtime + DirectML inference,
# Pillow-exact resampling and the L0 pass. Standalone static library: it does
# not depend on the UI; models.c's download path reuses the updater's WinHTTP
# helper (linked from anime_core / CUI at exe link time). plat_win.c is the
# Win32 half of the platform seam (src/plat.h); sha256.c is the shared digest
# that replaced models.c's BCrypt call so the POSIX build uses the same code.
add_library(anime_engine STATIC
    src/engine/engine.c
    src/engine/l0.c
    src/engine/models.c
    src/engine/resample.c
    src/plat_win.c
    src/sha256.c
    third_party/pocketfft/pocketfft_double.c)

target_include_directories(anime_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pocketfft)

target_compile_definitions(anime_engine PUBLIC
    _CRT_SECURE_NO_WARNINGS=1
    UNICODE=1
    _UNICODE=1)

target_link_libraries(anime_engine PUBLIC dxgi winhttp bcrypt)

# Vendored pocketfft keeps upstream warnings (/W4); don't let them block ours.
set_source_files_properties(third_party/pocketfft/pocketfft_double.c
    PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4267;/wd4702")

configure_file(src/app.rc.in ${CMAKE_BINARY_DIR}/app.rc @ONLY)

add_executable(AnimeDenoise
    src/main.c
    ${CMAKE_BINARY_DIR}/app.rc)

target_compile_definitions(anime_core PUBLIC
    APP_VERSION="${PROJECT_VERSION}"
    APP_UPDATE_URL="https://github.com/yabo083/cui-anime-denoise/releases/latest/download/update.json"
    _CRT_SECURE_NO_WARNINGS=1
    UNICODE=1
    _UNICODE=1)

if(MSVC)
    # Source files are UTF-8; without this MSVC reads the Chinese literals in
    # the current ANSI code page and mangles them.
    target_compile_options(anime_core PRIVATE /utf-8 /W4 /permissive-)
    target_compile_options(anime_core PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(anime_engine PRIVATE /utf-8 /W4 /permissive-)
    target_compile_options(anime_engine PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(AnimeDenoise PRIVATE /utf-8 /W4 /permissive-)
endif()

target_link_libraries(anime_core PUBLIC
    cui::cui
    ole32 oleaut32 uuid windowscodecs bcrypt winhttp
    shell32 shlwapi advapi32 user32 gdi32 comctl32 dwmapi)

target_link_libraries(AnimeDenoise PRIVATE anime_core anime_engine)
cui_app_target(AnimeDenoise)

# Console front end (docs/cli-spec.md): shares the engine, the WIC image IO
# and the updater's download helper -- never the GUI modules. CUI's static
# lib is linked only so models.c's CUGAN zip extraction finds cui__inflate
# (the linker keeps just that one object).
add_executable(anime-denoise
    src/cli/main.c
    src/cli/sheet.c
    src/img.c
    src/img_common.c
    src/img_clip.c
    src/update_io.c)

target_compile_definitions(anime-denoise PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    APP_UPDATE_URL="https://github.com/yabo083/cui-anime-denoise/releases/latest/download/update.json"
    _CRT_SECURE_NO_WARNINGS=1
    UNICODE=1
    _UNICODE=1)

target_link_libraries(anime-denoise PRIVATE
    anime_engine
    cui::cui
    ole32 oleaut32 windowscodecs shlwapi uuid gdi32)

if(MSVC)
    target_compile_options(anime-denoise PRIVATE /utf-8 /W4 /permissive-)
    target_compile_options(anime-denoise PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

# A runnable build directory: <exe dir>\assets\... (fonts + icons)
add_custom_command(TARGET AnimeDenoise POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:AnimeDenoise>/assets"
    COMMENT "Copying assets next to the executable")

# Native engine runtime: the ORT/DirectML DLLs load from <exe dir>\runtime\ and
# the bundled model from <exe dir>\models\ (see src/engine/engine.c).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/_dl/runtime/onnxruntime.dll")
    add_custom_command(TARGET AnimeDenoise POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/_dl/runtime" "$<TARGET_FILE_DIR:AnimeDenoise>/runtime"
        COMMENT "Copying ORT/DirectML runtime next to the executable")
else()
    message(WARNING "_dl/runtime/onnxruntime.dll is missing: run tools/fetch_deps.ps1 "
                    "before running anything that uses the native engine")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/models/esrgan_anime6b_x4.onnx")
    add_custom_command(TARGET AnimeDenoise POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/models" "$<TARGET_FILE_DIR:AnimeDenoise>/models"
        COMMENT "Copying ONNX models next to the executable")
else()
    message(WARNING "models/esrgan_anime6b_x4.onnx is missing: run tools/export_onnx.py "
                    "to export the bundled model")
endif()
