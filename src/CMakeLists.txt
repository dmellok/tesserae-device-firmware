# Touch-v3 icon font (pinned Phosphor bold 2.1.2, MIT). Embedding it under
# PlatformIO needs BOTH halves, and they must be enabled together per env:
#   platformio.ini `board_build.embed_files`      GENERATES the assembly stub
#   this EMBED_FILES entry                        LINKS it
# With only the first the symbols come out undefined; with only the second the
# build dies looking for a .S nothing produced -- which is why this is gated
# rather than unconditional: boards that do not render icons neither generate
# the stub nor should pay the ~484 KB for it. The E1003 env turns it on via
# `board_build.cmake_extra_args = ... -D TESSERAE_ICON_FONT=1`.
set(TESSERAE_EMBED_FILES "")
if(TESSERAE_ICON_FONT)
    list(APPEND TESSERAE_EMBED_FILES "fonts/Phosphor-Bold.ttf")
endif()

idf_component_register(
    SRCS
        "main.c"
        "epd_driver.c"
        "panel/registry.c"
        "panel/drivers/spectra6_spi_dual.c"
        "panel/drivers/spectra6_t133a01_dual.c"
        "panel/drivers/spectra6_spi_single.c"
        "panel/drivers/mono_spi.c"
        "panel/drivers/it8951_gray.c"
        "battery.c"
        "pmic.c"
        "sht4x.c"
        "shtc3.c"
        "i2c_bus.c"
        "touch_coords.c"
        "touch_gt911.c"
        "touch_queue.c"
        "touch_wakestub.c"
        "wifi_manager.c"
        "provisioning.c"
        "button_report.c"
        "net_rest.c"
        "relay.c"
        "relay_crypto.c"
        "relay_wire.c"
        "rest_config.c"
        "image_fetcher.c"
        "image_decoder.c"
        "ota_verify.c"
        "ota_manifest.c"
        "ota_install.c"
        "ota_boot.c"
        "ota_report.c"
        "overlay.c"
        "overlay_run.c"
        "proto2.c"
        "proto2_run.c"
        "touch3.c"
        "touch3_run.c"
        "touch3_icons.c"
        "sse_client.c"
        "deck.c"
        "frame_collection.c"
        "deck_digest.c"
        "deck_cache.c"
        "collection_cache.c"
        "deck_run.c"
        "collection_run.c"
        "sdcard.c"
        "splash.c"
        "vendor/qrcodegen.c"
    INCLUDE_DIRS
        "."
        "panel"
        "../boards"
        "../include"
    EMBED_FILES
        ${TESSERAE_EMBED_FILES}
    REQUIRES
        driver
        app_update
        esp_adc
        esp_driver_i2c
        esp_driver_usb_serial_jtag
        esp_event
        esp_http_client
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        esp-tls
        fatfs
        json
        nvs_flash
        mbedtls
        lwip
        mdns
)
