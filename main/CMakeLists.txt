idf_component_register(
    SRCS
        "main.c"
        "audio_driver.c"
        "wake_word.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        edge-impulse-wake
        driver
        esp_timer
        esp_system
        heap
        log
        nvs_flash
        freertos
)

# The Edge Impulse DSP headers are third-party and trip ESP-IDF's -Werror=all
# in the TUs that include them. Downgrade their warnings to avoid touching the
# vendored SDK sources.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=uninitialized
    -Wno-error=maybe-uninitialized
    -Wno-error=deprecated-declarations
    -Wno-error=missing-field-initializers
)
