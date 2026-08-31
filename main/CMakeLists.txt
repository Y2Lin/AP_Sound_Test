idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "demo_sound_meter.c"
         "sound_meter_model.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash
)
