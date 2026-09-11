idf_component_register(
    SRCS "broadcast_sched.c" "cache_mgr.c" "caterpillar_mqtt.c"
    INCLUDE_DIRS "include"
    REQUIRES json mqtt
    PRIV_REQUIRES esp_http_client spiffs
)
