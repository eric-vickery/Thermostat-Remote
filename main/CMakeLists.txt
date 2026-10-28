idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_now_remote esp_wifi nvs_flash esp_driver_gpio esp_timer)
