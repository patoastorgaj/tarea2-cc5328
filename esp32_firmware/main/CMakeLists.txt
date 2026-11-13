idf_component_register(
    SRCS "main.c" "bme688.c" "processing.c" "protocol.c" "mini_json.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer
)
