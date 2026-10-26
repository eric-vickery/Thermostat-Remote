idf_component_register(SRCS "esp_now_remote.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif esp_event esp_timer)
