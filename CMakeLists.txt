cmake_minimum_required(VERSION 3.15)
project(phosphoric VERSION 1.19.0 LANGUAGES C CXX)

# C/C++ Standards
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

# Options
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_TOOLS "Build conversion tools" ON)
option(ENABLE_COVERAGE "Enable code coverage" OFF)
option(ENABLE_SANITIZERS "Enable address/undefined behavior sanitizers" OFF)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_MIXER sdl2_mixer)

# Include directories
include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${SDL2_INCLUDE_DIRS}
)

# SDL2 is linked unconditionally here, so enable the display/audio/input code
# paths (the Makefile passes this via SDL2=1 → -DHAS_SDL2). Without it the
# binary would link SDL2 yet stay headless.
add_compile_definitions(HAS_SDL2)

# Source files
set(CPU_SOURCES
    src/cpu/cpu6502.c
    src/cpu/opcodes.c
    src/cpu/addressing.c
)

set(MEMORY_SOURCES
    src/memory/memory.c
    src/memory/banking.c
)

set(IO_SOURCES
    src/io/via6522.c
    src/io/keyboard.c
    src/io/joystick.c
    src/io/printer.c
    src/io/mcp40.c
    src/io/cassette.c
    src/io/microdisc.c
    src/io/loci_core.c
    src/io/loci_fs.c
    src/io/loci_bus.c
    src/io/loci_boot.c
    src/io/loci_sdimg.c
    src/io/acia6551.c
    src/io/serial_backend.c
    src/io/dtl2000.c
    src/io/serial_picowifi.c
)

set(VIDEO_SOURCES
    src/video/video.c
    src/video/textmode.c
    src/video/hires.c
    src/video/export.c
    src/video/renderer.c
)

set(AUDIO_SOURCES
    src/audio/ay3891x.c
    src/audio/audio_output.c
)

set(STORAGE_SOURCES
    src/storage/tap.c
    src/storage/sedoric.c
    src/storage/disk.c
)

set(HOSTFS_SOURCES
    src/hostfs/hostfs.c
    src/hostfs/vfs.c
)

set(DEBUGGER_SOURCES
    src/debugger.c
    src/control.c
    src/savestate.c
)

set(UTILS_SOURCES
    src/utils/logging.c
    src/utils/config.c
    src/utils/trace.c
    src/utils/profiler.c
    src/utils/rominfo.c
    src/utils/symbols.c
)

set(MAIN_SOURCE
    src/main.c
)

# Main emulator library
add_library(oric1_core STATIC
    ${CPU_SOURCES}
    ${MEMORY_SOURCES}
    ${IO_SOURCES}
    ${VIDEO_SOURCES}
    ${AUDIO_SOURCES}
    ${STORAGE_SOURCES}
    ${HOSTFS_SOURCES}
    ${DEBUGGER_SOURCES}
    ${UTILS_SOURCES}
)

target_link_libraries(oric1_core
    ${SDL2_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    m
)

# Main executable
add_executable(oric1-emu ${MAIN_SOURCE})
target_link_libraries(oric1-emu oric1_core)

# Conversion tools
if(BUILD_TOOLS)
    add_executable(bas2tap tools/bas2tap.c src/storage/tap.c src/utils/logging.c)
    add_executable(bin2tap tools/bin2tap.c src/storage/tap.c src/utils/logging.c)
    add_executable(tap2sedoric tools/tap2sedoric.c src/storage/tap.c src/storage/sedoric.c src/utils/logging.c)

    install(TARGETS bas2tap bin2tap tap2sedoric
        RUNTIME DESTINATION bin)
endif()

# Tests
if(BUILD_TESTS)
    enable_testing()

    # Unit tests
    add_executable(cpu_tests tests/unit/test_cpu.c)
    target_link_libraries(cpu_tests oric1_core)
    add_test(NAME cpu_tests COMMAND cpu_tests)

    add_executable(memory_tests tests/unit/test_memory.c)
    target_link_libraries(memory_tests oric1_core)
    add_test(NAME memory_tests COMMAND memory_tests)

    add_executable(io_tests tests/unit/test_io.c)
    target_link_libraries(io_tests oric1_core)
    add_test(NAME io_tests COMMAND io_tests)

    add_executable(video_tests tests/unit/test_video.c)
    target_link_libraries(video_tests oric1_core)
    add_test(NAME video_tests COMMAND video_tests)

    add_executable(audio_tests tests/unit/test_audio.c)
    target_link_libraries(audio_tests oric1_core)
    add_test(NAME audio_tests COMMAND audio_tests)

    add_executable(storage_tests tests/unit/test_storage.c)
    target_link_libraries(storage_tests oric1_core)
    add_test(NAME storage_tests COMMAND storage_tests)

    add_executable(debugger_tests tests/unit/test_debugger.c)
    target_link_libraries(debugger_tests oric1_core)
    add_test(NAME debugger_tests COMMAND debugger_tests)

    # Integration tests
    add_executable(integration_tests tests/integration/test_full_system.c)
    target_link_libraries(integration_tests oric1_core)
    add_test(NAME integration_tests COMMAND integration_tests)
endif()

# Code coverage
if(ENABLE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
endif()

# Sanitizers
if(ENABLE_SANITIZERS)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address,undefined")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

# Installation
install(TARGETS oric1-emu
    RUNTIME DESTINATION bin)

install(DIRECTORY roms/
    DESTINATION share/phosphoric/roms
    FILES_MATCHING PATTERN "*.rom")

install(FILES docs/user-guide/README.md
    DESTINATION share/doc/phosphoric)

# CPack configuration
set(CPACK_PACKAGE_NAME "phosphoric")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Phosphoric — Cycle-accurate ORIC-1/Atmos emulator")
set(CPACK_PACKAGE_CONTACT "bmarty <bmarty@mailo.com>")
set(CPACK_GENERATOR "DEB;RPM;TGZ")

include(CPack)

# Print configuration summary
message(STATUS "")
message(STATUS "Phosphoric Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  CXX Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  Build tests: ${BUILD_TESTS}")
message(STATUS "  Build tools: ${BUILD_TOOLS}")
message(STATUS "  Code coverage: ${ENABLE_COVERAGE}")
message(STATUS "  Sanitizers: ${ENABLE_SANITIZERS}")
message(STATUS "")
