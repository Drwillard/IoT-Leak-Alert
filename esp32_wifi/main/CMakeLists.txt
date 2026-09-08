idf_component_register(SRCS "wifi_setup.c" "email_test.c" "water_alarm.c" "water_logic.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash json driver efuse bootloader_support mbedtls esp_timer esp-tls esp_http_client lwip esp_adc)
