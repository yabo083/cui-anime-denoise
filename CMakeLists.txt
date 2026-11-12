cmake_minimum_required(VERSION 3.14)

project(anime_denoise
    VERSION 1.1.0
    DESCRIPTION "Anime Denoise: native CUI front end, Python inference worker"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CUI build; examples and tests are not part of this app.
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/CUI)

add_executable(AnimeDenoise
    src/main.c
    src/backend.c
    src/img.c
    src/mask.c
    src/config.c
    src/update.c)

target_compile_definitions(AnimeDenoise PRIVATE
    APP_VERSION="${PROJECT_VERSION}"
    _CRT_SECURE_NO_WARNINGS=1
    UNICODE=1
    _UNICODE=1)

if(MSVC)
    # Source files are UTF-8; without this MSVC reads the Chinese literals in
    # the current ANSI code page and mangles them.
    target_compile_options(AnimeDenoise PRIVATE /utf-8 /W4 /permissive-)
    target_compile_options(AnimeDenoise PRIVATE $<$<CONFIG:Release>:/O2>)
endif()

target_link_libraries(AnimeDenoise PRIVATE
    cui::cui
    ole32 oleaut32 uuid windowscodecs bcrypt winhttp
    shell32 shlwapi advapi32 user32 gdi32 comctl32 dwmapi)

cui_app_target(AnimeDenoise)

# A runnable build directory: <exe dir>\assets\... and <exe dir>\worker\...
add_custom_command(TARGET AnimeDenoise POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:AnimeDenoise>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/worker" "$<TARGET_FILE_DIR:AnimeDenoise>/worker"
    COMMENT "Copying assets and worker next to the executable")
