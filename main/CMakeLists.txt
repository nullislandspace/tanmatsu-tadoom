# Collect PrBoom core sources
file(GLOB PRBOOM_SOURCES "prboom/*.c")

# Remove files we don't want (GL, server, mmap - shouldn't be there but just in case)
list(FILTER PRBOOM_SOURCES EXCLUDE REGEX "gl_main\\.c$")
list(FILTER PRBOOM_SOURCES EXCLUDE REGEX "gl_texture\\.c$")
list(FILTER PRBOOM_SOURCES EXCLUDE REGEX "d_server\\.c$")
list(FILTER PRBOOM_SOURCES EXCLUDE REGEX "w_mmap\\.c$")

# Tanmatsu HAL sources
set(TANMATSU_SOURCES
    main.c
    i_system_tanmatsu.c
    i_video_tanmatsu.c
    i_sound_tanmatsu.c
    i_joy_tanmatsu.c
    i_network_tanmatsu.c
    midi_player.c
    sdcard.c
    fastopen.c
)

idf_component_register(
    SRCS
        ${TANMATSU_SOURCES}
        ${PRBOOM_SOURCES}
    PRIV_REQUIRES
        esp_lcd
        fatfs
        nvs_flash
        badge-bsp
        esp_timer
        driver
        sdmmc
        esp_driver_sdmmc
        vfs
    INCLUDE_DIRS
        "."
        "prboom"
        "lib"
)

# Suppress warnings on PrBoom sources (old C89 code)
set_source_files_properties(${PRBOOM_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wno-error;-Wno-all;-Wno-implicit-function-declaration;-Wno-int-conversion;-Wno-pointer-sign;-Wno-unused-variable;-Wno-format;-Wno-missing-braces;-Wno-incompatible-pointer-types;-Wno-unused-but-set-variable;-Wno-shift-negative-value"
)

# Force all prboom sources to include config.h (some don't check HAVE_CONFIG_H)
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H=1)
target_compile_options(${COMPONENT_LIB} PRIVATE "-include" "config.h")

# Global build options
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-variable" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=unused-const-variable" APPEND)
