cmake_minimum_required(VERSION 3.16)
# orange branch: Linux ARM64 port (Orange Pi Zero 2W) - DRM/KMS video,
# ALSA sound, evdev keyboards and game controllers.  The RP2350 firmware
# is built from the master branch.
project(gameboy-color VERSION 3.1.9 LANGUAGES C CXX)

# one place for the version: menu footer (PICO_PROGRAM_VERSION_STRING)
set(PICO_GAMEBOY_VERSION "3.1.9")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2 -g")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
find_package(Threads REQUIRED)

add_executable(gameboy
    # Emulator (Peanut-GB core in inc/peanut_gb.h, APU)
    src/main.cpp
    ext/minigb_apu/minigb_apu.c

    # Text output and backplane, shared with the RP2350 build
    drivers/graphics/graphics.c
    drivers/graphics/backplane.c

    # Linux host (counterparts of the RP2350 drivers)
    src/linux/hdmi_linux.c      # HDMI scanline composer (drivers/hdmi)
    src/linux/video_drm.c       # DRM/KMS output
    src/linux/audio_linux.c     # ALSA output (I2S)
    src/linux/input_linux.c     # evdev keyboards and gamepads (PS/2, USB HID, NES pad)
    src/linux/kbd_linux.cpp     # key events -> HID keyboard reports
    src/linux/ff_posix.c        # FatFs API over the data directory (SD card)
    src/linux/platform_linux.c  # reboot / quit
)

# src/linux first: its pico.h, pico/*.h, hdmi.h, nespad.h, ff.h and f_util.h
# replace the Pico SDK and RP2350 driver headers.
target_include_directories(gameboy PRIVATE
    src/linux
    src
    inc
    ext/minigb_apu
    drivers/graphics
)

target_compile_definitions(gameboy PRIVATE
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE=1
    ORANGE_GB=1
    HDMI=1
    PICO_PROGRAM_NAME="Gameboy emulator by xrip for Orange Pi Zero 2W"
    PICO_PROGRAM_VERSION_STRING="${PICO_GAMEBOY_VERSION}"
)

target_compile_options(gameboy PRIVATE
    -Wall
    -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

target_link_libraries(gameboy PRIVATE PkgConfig::LIBDRM PkgConfig::ALSA Threads::Threads)
install(TARGETS gameboy RUNTIME DESTINATION bin)

# Debian binary package. Dependencies are derived from the actual linked ELF
# on the build machine (e.g. libdrm2 and the distribution's ALSA library).
install(FILES README.md DESTINATION share/doc/pico-gameboy)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "pico-gameboy")
set(CPACK_PACKAGE_VENDOR "DnCraptor")
set(CPACK_PACKAGE_CONTACT "DnCraptor")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Game Boy / Game Boy Color emulator using Linux DRM/KMS and ALSA")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/DnCraptor/pico-gameboy")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_STRIP_FILES ON)
include(CPack)
