cmake_minimum_required(VERSION 3.14)

project(cui_anime_denoise
    VERSION 0.1.0
    DESCRIPTION "Anime line-art de-screening GUI: CUI front end, Python worker"
    LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CUI build; examples and tests are not part of this app.
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/CUI)

# Worker bridge sources (src/backend.c) join this same target when the integration lands.
add_executable(denoise_app
    src/main.c
    src/backend.c)
target_link_libraries(denoise_app PRIVATE cui::cui)
cui_app_target(denoise_app)
