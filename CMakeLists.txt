cmake_minimum_required(VERSION 3.16)
project(DesertStrikeHD C CXX)

# Targets
#   StrikeCollectionHD  the PC game: Desert, Jungle and Urban Strike (SDL3)
#   ds_test         headless recompiled build, used for lockstep tests
#   ds_ref          Musashi 68000 reference, the other side of lockstep
#
# The recompiled C is derived from your ROM and never committed. Generate it
# first:  python tools/trace.py && python tools/recomp.py
option(DS_BUILD_HOST  "Build the SDL3 game" ON)
option(DS_BUILD_TESTS "Build ds_test and ds_ref" ON)
option(DS_SYSTEM_SDL  "Use an installed SDL3 instead of building it" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_CXX_STANDARD 14)

if(MSVC)
    set(DS_FLAGS /W1 /bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(DS_FLAGS -w)
endif()

# ---- games in the collection ----
set(DS_GAMES desert jungle urban)
set(DS_GAME_SOURCES games/registry.c games/strike_world.c)
foreach(g ${DS_GAMES})
    file(GLOB gen ${CMAKE_SOURCE_DIR}/generated/${g}/recomp_*.c)
    if(NOT gen)
        message(FATAL_ERROR "generated/${g}/ is empty: run `python tools/trace.py --game ${g}` and `python tools/recomp.py --game ${g}` first")
    endif()
    list(APPEND DS_GAME_SOURCES games/${g}/${g}.c ${gen})
endforeach()

# ---- hardware model: bus, VDP, renderer, Z80, YM2612, PSG ----
add_library(ds_hw STATIC
    src/hw/genesis.c src/hw/sound.c src/hw/vdp_render.c src/hw/ym2612_ymfm.cpp
    third_party/z80/z80.c
    third_party/ymfm/src/ymfm_opn.cpp third_party/ymfm/src/ymfm_adpcm.cpp third_party/ymfm/src/ymfm_ssg.cpp)
target_include_directories(ds_hw PUBLIC src src/hw PRIVATE third_party/z80 third_party/ymfm/src)
target_compile_options(ds_hw PRIVATE ${DS_FLAGS})

# ---- recompiled games + CPU runtime ----
add_library(ds_core STATIC src/runtime/cpu.c src/util/sha1.c ${DS_GAME_SOURCES})
target_include_directories(ds_core PUBLIC src src/runtime games)
target_link_libraries(ds_core PUBLIC ds_hw)
target_compile_options(ds_core PRIVATE ${DS_FLAGS})

if(DS_BUILD_TESTS)
    add_executable(ds_test src/runtime/test_main.c)
    target_link_libraries(ds_test PRIVATE ds_core)
    target_compile_options(ds_test PRIVATE ${DS_FLAGS})

    if(EXISTS ${CMAKE_SOURCE_DIR}/harness/musashi/m68kops.c)
        add_executable(ds_ref
            harness/harness.c
            harness/musashi/m68kcpu.c harness/musashi/m68kops.c
            harness/musashi/m68kdasm.c harness/musashi/softfloat/softfloat.c)
        target_include_directories(ds_ref PRIVATE harness/musashi)
        target_link_libraries(ds_ref PRIVATE ds_hw)
        target_compile_options(ds_ref PRIVATE ${DS_FLAGS})
        if(NOT MSVC)
            target_link_libraries(ds_ref PRIVATE m)
        endif()
    endif()
endif()

if(DS_BUILD_HOST)
    if(DS_SYSTEM_SDL)
        find_package(SDL3 REQUIRED CONFIG)
        set(DS_SDL SDL3::SDL3)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/SDL3/CMakeLists.txt)
            add_subdirectory(third_party/SDL3 EXCLUDE_FROM_ALL)
        else()
            include(FetchContent)
            FetchContent_Declare(SDL3
                GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                GIT_TAG release-3.2.24
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(SDL3)
        endif()
        set(DS_SDL SDL3::SDL3-static)
    endif()

    set(DS_HOST_SOURCES src/host/main.c src/host/input.c src/host/settings.c
        src/host/ui.c src/host/menu.c src/host/embedded_fonts.c src/host/hdpack.c)
    if(WIN32)
        list(APPEND DS_HOST_SOURCES res/desertstrikehd.rc)
    endif()
    add_executable(StrikeCollectionHD WIN32 ${DS_HOST_SOURCES})
    target_link_libraries(StrikeCollectionHD PRIVATE ds_core ${DS_SDL})
    target_include_directories(StrikeCollectionHD PRIVATE third_party/stb)
    target_compile_options(StrikeCollectionHD PRIVATE ${DS_FLAGS})
    if(MINGW)
        target_link_options(StrikeCollectionHD PRIVATE -static)
    endif()
endif()

# ---- HD pack upscaler (separate program: xBRZ is GPL-3, see tools/xbrz/License.txt) ----
add_executable(hdpack_xbrz tools/xbrz/hdpack_xbrz.cpp tools/xbrz/xbrz.cpp)
target_compile_features(hdpack_xbrz PRIVATE cxx_std_17)
target_compile_options(hdpack_xbrz PRIVATE ${DS_FLAGS})
if(MINGW)
    target_link_options(hdpack_xbrz PRIVATE -static)
endif()
