add_library(graphics INTERFACE)

target_sources(graphics INTERFACE ${CMAKE_CURRENT_LIST_DIR}/graphics.c ${CMAKE_CURRENT_LIST_DIR}/backplane.c)

#target_link_libraries(graphics INTERFACE
#vga-nextgen
#hdmi
#tft
#)

target_include_directories(graphics INTERFACE ${CMAKE_CURRENT_LIST_DIR})

#pico_generate_pio_header(st7789
#        ${CMAKE_CURRENT_LIST_DIR}/st7789.pio
#)

# Follow the display selection in the top-level CMakeLists.txt.
get_target_property(backplane_platform_defines pico_platform_headers INTERFACE_COMPILE_DEFINITIONS)
if("PICO_RP2350=1" IN_LIST backplane_platform_defines AND NOT TFT AND (HDMI OR NOT TV))
    set(backplane_width 320)
    if(SOFTTV AND NOT HDMI)
        set(backplane_width 384)
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(backplane_dir "${PROJECT_SOURCE_DIR}/sd-card/GB")
    set(backplane_assets
        "${backplane_dir}/gbc-green.gbbp"
        "${backplane_dir}/gbc-purple.gbbp"
        "${backplane_dir}/gbc-green-white.gbbp"
        "${backplane_dir}/gbc-green-white-3x.gbbp"
    )
    set(backplane_generator "${PROJECT_SOURCE_DIR}/tools/embed_gbbp.py")
    set(backplane_header "${CMAKE_CURRENT_BINARY_DIR}/backplane_skins.h")
    add_custom_command(OUTPUT "${backplane_header}"
        COMMAND "${Python3_EXECUTABLE}" "${backplane_generator}" "${backplane_width}"
                "${backplane_header}" ${backplane_assets}
        DEPENDS "${backplane_generator}" ${backplane_assets}
        VERBATIM)
    add_custom_target(gameboy_backplane_skins DEPENDS "${backplane_header}")
    add_dependencies(graphics gameboy_backplane_skins)
    target_include_directories(graphics INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")
endif()
