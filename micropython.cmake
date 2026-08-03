# Create an INTERFACE library for our C module.

add_library(usermod_lcd_bus INTERFACE)

if(ESP_PLATFORM)
    set(INCLUDES
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/esp32_include
    )

    set(SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/modlcd_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/lcd_types.c
        ${CMAKE_CURRENT_LIST_DIR}/esp32_src/i2c_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/esp32_src/spi_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/esp32_src/i80_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/esp32_src/rgb_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/esp32_src/dsi_bus.c
    )

    # gets esp_lcd include paths
    idf_component_get_property(esp_lcd_includes esp_lcd INCLUDE_DIRS)
    idf_component_get_property(esp_lcd_dir esp_lcd COMPONENT_DIR)

    # sets the include paths into INCLUDES variable
    if(esp_lcd_includes)
        list(TRANSFORM esp_lcd_includes PREPEND ${esp_lcd_dir}/)
        list(APPEND INCLUDES ${esp_lcd_includes})
    endif()

    # esp_cache.h lives in the esp_mm component. esp_lcd depends on esp_mm
    # privately (PRIV_REQUIRES), so its include dirs are NOT propagated to us.
    # dsi_bus.c needs esp_cache.h for esp_cache_msync() (flush, ESP32-P4).
    idf_component_get_property(esp_mm_includes esp_mm INCLUDE_DIRS)
    idf_component_get_property(esp_mm_dir esp_mm COMPONENT_DIR)
    if(esp_mm_includes)
        list(TRANSFORM esp_mm_includes PREPEND ${esp_mm_dir}/)
        list(APPEND INCLUDES ${esp_mm_includes})
    endif()

else()
    set(INCLUDES
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/common_include
    )

    set(SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/lcd_types.c
        ${CMAKE_CURRENT_LIST_DIR}/modlcd_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/common_src/i2c_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/common_src/spi_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/common_src/i80_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/common_src/rgb_bus.c
        ${CMAKE_CURRENT_LIST_DIR}/common_src/dsi_bus.c
    )

endif(ESP_PLATFORM)

# NOTE: esp32_src/dsi_bus.c is compiled on every ESP32 target, but the real MIPI
# DSI implementation inside it is guarded by `#if SOC_MIPI_DSI_SUPPORTED` (only
# defined on ESP32-P4). On chips without DSI (e.g. ESP32-S3) it compiles down to
# a stub type that raises NotImplementedError, so no manual build-system routing
# is needed per chip.

# NOTE: DSI DPI panels need high PSRAM bandwidth (200MHz + L2 cache) to avoid
# underrun. When building with mp_Make-Tools, the required sdkconfig keys for
# each chip are declared in sdkconfig.require.json (next to this file) and
# injected automatically by the build tool — no manual sdkconfig editing needed.


# Add our source files to the lib
target_sources(usermod_lcd_bus INTERFACE ${SOURCES})

# Add include directories.
target_include_directories(usermod_lcd_bus INTERFACE ${INCLUDES})

# Link our INTERFACE library to the usermod target.
target_link_libraries(usermod INTERFACE usermod_lcd_bus)
