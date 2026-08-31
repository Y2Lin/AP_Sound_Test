idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_sound_meter.c"
         "sound_meter_model.c"
         "fap_screenshot.c"
    INCLUDE_DIRS "."
    REQUIRES bsp driver esp_timer nvs_flash
)
