#include "sdcard.h"

#include <dirent.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

static const char *TAG = "sdcard";

/* SPI host used for the SD card. SPI2 (a.k.a. FSPI) is the general-purpose host on the C3. */
#define SD_SPI_HOST SPI2_HOST

static sdmmc_card_t *s_card;

esp_err_t sdcard_mount(void)
{
    esp_err_t ret;

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_SD_PIN_MOSI,
        .miso_io_num = CONFIG_SD_PIN_MISO,
        .sclk_io_num = CONFIG_SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = CONFIG_SD_SPI_FREQ_KHZ;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = CONFIG_SD_PIN_CS;
    slot_cfg.host_id = SD_SPI_HOST;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* never wipe the user's card */
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(TAG, "Mounting SD (MOSI=%d MISO=%d CLK=%d CS=%d @ %d kHz)",
             CONFIG_SD_PIN_MOSI, CONFIG_SD_PIN_MISO, CONFIG_SD_PIN_CLK,
             CONFIG_SD_PIN_CS, CONFIG_SD_SPI_FREQ_KHZ);

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount FAT filesystem. Is the card formatted FAT32?");
        } else {
            ESP_LOGE(TAG, "SD init failed: %s. Check wiring and pull-ups.", esp_err_to_name(ret));
        }
        spi_bus_free(SD_SPI_HOST);
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);

    /* Log the root directory so the exact filenames/case are visible for building
     * /media?path=... URLs. */
    DIR *d = opendir(SD_MOUNT_POINT);
    if (d) {
        ESP_LOGI(TAG, "--- %s contents ---", SD_MOUNT_POINT);
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            ESP_LOGI(TAG, "  %s%s", de->d_name, (de->d_type == DT_DIR) ? "/" : "");
        }
        ESP_LOGI(TAG, "--- end ---");
        closedir(d);
    }
    return ESP_OK;
}
