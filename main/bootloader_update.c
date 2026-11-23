#include "bootloader_update.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <esp_flash_partitions.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static const char *TAG = "bootloader";

// This build's own bootloader (main/CMakeLists.txt), covered by the app's signature.
extern const uint8_t bl_start[] asm("_binary_bootloader_bin_start");
extern const uint8_t bl_end[] asm("_binary_bootloader_bin_end");

// The Shelly kept the bootloader of its first serial flash, which ignores the rollback
// states in otadata. Replace it with ours. The classic ESP32 has no recovery bootloader:
// power lost during the erase+write (well under a second) leaves a unit that needs a cable.
bool bootloader_update(void)
{
    size_t len = bl_end - bl_start;
    if (len < 24 || len > ESP_BOOTLOADER_SIZE || bl_start[0] != 0xE9) {
        ESP_LOGE(TAG, "embedded image unusable (%u bytes)", (unsigned)len);
        return true;  // nothing a retry would fix
    }
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_BOOTLOADER,
                                                        ESP_PARTITION_SUBTYPE_BOOTLOADER_PRIMARY, NULL);
    if (!p && esp_partition_register_external(NULL, ESP_PRIMARY_BOOTLOADER_OFFSET, ESP_BOOTLOADER_SIZE,
                                              "PrimaryBTLDR", ESP_PARTITION_TYPE_BOOTLOADER,
                                              ESP_PARTITION_SUBTYPE_BOOTLOADER_PRIMARY, &p) != ESP_OK) {
        ESP_LOGE(TAG, "cannot register the bootloader region");
        return true;
    }
    uint8_t *cur = malloc(len);
    if (!cur) {
        return false;
    }
    if (esp_partition_read(p, 0, cur, len) == ESP_OK && !memcmp(cur, bl_start, len)) {
        ESP_LOGI(TAG, "bootloader current");
        free(cur);
        return true;
    }
    ESP_LOGW(TAG, "replacing the bootloader (%u bytes)", (unsigned)len);
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(p, len, &h);
    if (err == ESP_OK) {
        err = esp_ota_write(h, bl_start, len);
        esp_err_t end = esp_ota_end(h);  // verifies the written image
        if (err == ESP_OK) {
            err = end;
        }
    }
    bool same = esp_partition_read(p, 0, cur, len) == ESP_OK && !memcmp(cur, bl_start, len);
    free(cur);
    if (err == ESP_OK && same) {
        ESP_LOGW(TAG, "bootloader replaced");
        return true;
    }
    ESP_LOGE(TAG, "bootloader write failed (%s, match %d)", esp_err_to_name(err), same);
    return false;
}
