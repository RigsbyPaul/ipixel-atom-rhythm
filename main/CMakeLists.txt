idf_component_register(SRCS "main.c" "ipixel.c" "audio.c" "bands.c" "settings.c" "ui.c" "draw.c" "live.c" "fx.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_hw_support esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_timer)
