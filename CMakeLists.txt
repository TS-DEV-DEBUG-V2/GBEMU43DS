cmake_minimum_required(VERSION 3.12)
project(gbemu CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.cpp
    src/gb_cpu.cpp
    src/gb_memory.cpp
    src/gb_ppu.cpp
    src/gb_timer.cpp
    src/gb_input.cpp
    src/gb_apu.cpp
    src/gb_cartridge.cpp
    src/bios.cpp
    src/bios4gbc.cpp
    src/gb_log.cpp
    src/unzip.cpp
    src/gb_savestate.cpp
    src/gb_gamegenie.cpp
    src/gb_gameshark.cpp
    src/gb_printer.cpp
)

if(EMSCRIPTEN)
    add_executable(gbemu ${SOURCES})
    target_compile_options(gbemu PRIVATE -s USE_SDL=2)
    target_link_options(gbemu PRIVATE
        "SHELL:-s USE_SDL=2"
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s TOTAL_MEMORY=67108864"
        "SHELL:-s FORCE_FILESYSTEM=1"
        "SHELL:-s MIN_WEBGL_VERSION=1"
        "SHELL:-s MAX_WEBGL_VERSION=1"
        "SHELL:--shell-file shell.html"
    )
    set_target_properties(gbemu PROPERTIES SUFFIX ".html")
else()
    find_package(SDL2 REQUIRED)
    add_executable(gbemu ${SOURCES})
    target_include_directories(gbemu PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gbemu ${SDL2_LIBRARIES})
    target_link_libraries(gbemu SDL2::SDL2)
endif()

target_include_directories(gbemu PRIVATE src)
