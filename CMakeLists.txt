cmake_minimum_required(VERSION 3.14)

project(anime_denoise
    VERSION 1.1.0
    DESCRIPTION "Anime Denoise: native CUI front end, Python inference worker"
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

# Everything but the entry point lives in a static library, so the benchmark
# project (lab/测试工程) can drive the same UI code without a window.
add_library(anime_core STATIC
    src/app.c
    src/ui_shell.c
    src/ui_stage.c
    src/backend.c
    src/backend_json.c
    src/chrome.c
    src/config.c
    src/log.c
    src/img.c
    src/loader.c
    src/mask.c
    src/runtime.c
    src/update.c
    src/update_io.c)

target_include_directories(anime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(AnimeDenoise
    src/main.c
    src/app.rc)

target_compile_definitions(anime_core PUBLIC
    APP_VERSION="${PROJECT_VERSION}"
    _CRT_SECURE_NO_WARNINGS=1
    UNICODE=1
    _UNICODE=1)

if(MSVC)
    # Source files are UTF-8; without this MSVC reads the Chinese literals in
    # the current ANSI code page and mangles them.
    target_compile_options(anime_core PRIVATE /utf-8 /W4 /permissive-)
    target_compile_options(anime_core PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(AnimeDenoise PRIVATE /utf-8 /W4 /permissive-)
endif()

target_link_libraries(anime_core PUBLIC
    cui::cui
    ole32 oleaut32 uuid windowscodecs bcrypt winhttp
    shell32 shlwapi advapi32 user32 gdi32 comctl32 dwmapi)

target_link_libraries(AnimeDenoise PRIVATE anime_core)
cui_app_target(AnimeDenoise)

# A runnable build directory: <exe dir>\assets\... and <exe dir>\worker\...
add_custom_command(TARGET AnimeDenoise POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:AnimeDenoise>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/worker" "$<TARGET_FILE_DIR:AnimeDenoise>/worker"
    COMMENT "Copying assets and worker next to the executable")
