idf_component_register(
    SRCS
        "app_main.c"
        "wifi.c"
        "sdcard.c"
        "http_server.c"
        "media_stream.c"
        "content_dir.c"
        "content_index.c"
        "dlna_ssdp.c"
        "dlna_upnp.c"
        "dlna_didl.c"
        "webdav.c"
        "webui.c"
        "config.c"
        "auth.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        fatfs
        sdmmc
        driver
        mdns
        lwip
        mbedtls
)

# Path-building snprintf() calls intentionally accept truncation for pathologically
# deep paths (never an overflow). Keep the warning but don't fail the build on it.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)

