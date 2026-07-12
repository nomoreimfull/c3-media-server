#pragma once

#include "esp_err.h"

/* Absolute VFS mount point for the SD card's FAT filesystem. */
#define SD_MOUNT_POINT "/sdcard"

/*
 * Bring up the SPI bus and mount the SD card's FAT filesystem at SD_MOUNT_POINT.
 * Pins and SPI clock come from Kconfig (CONFIG_SD_PIN_*, CONFIG_SD_SPI_FREQ_KHZ).
 * The ESP32-C3 has no SDMMC host, so this always uses SDSPI.
 *
 * Returns ESP_OK on success. On failure the filesystem is not mounted.
 */
esp_err_t sdcard_mount(void);
