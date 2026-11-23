#include "setup.h"

#include <stdint.h>
#include <string.h>
#include <esp_log.h>
#include <nvs.h>

static const char *TAG = "setup";

// Where the SDK reads setup info with CONFIG_HAP_PLATFORM_DEF_NVS_FACTORY_PARTITION="nvs"
// (esp_hap_database.c). hap_reset_pairings leaves this namespace alone.
#define NS "hap_setup"

#if __has_include("secrets.h")
#include "secrets.h"  // local builds only (tools/build.sh); CI and releases carry no secret
static void provision(void)
{
    static const uint8_t salt[16] = SETUP_SALT;
    static const uint8_t verifier[384] = SETUP_VERIFIER;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open " NS);
        return;
    }
    uint8_t cur[384];
    size_t len = sizeof(cur);
    // Rewrite only on change: flash wear, and the salt differs on every generation.
    if (nvs_get_blob(h, "setup_verifier", cur, &len) != ESP_OK || len != sizeof(verifier)
        || memcmp(cur, verifier, len)) {
        nvs_set_blob(h, "setup_id", SETUP_ID, strlen(SETUP_ID) + 1);
        nvs_set_blob(h, "setup_salt", salt, sizeof(salt));
        nvs_set_blob(h, "setup_verifier", verifier, sizeof(verifier));
        nvs_commit(h);
        ESP_LOGW(TAG, "setup info written");
    }
    nvs_close(h);
}
#else
static void provision(void) {}
#endif

bool setup_init(void)
{
    provision();
    nvs_handle_t h;
    size_t len = 0;
    bool ok = nvs_open(NS, NVS_READONLY, &h) == ESP_OK
              && nvs_get_blob(h, "setup_verifier", NULL, &len) == ESP_OK && len == 384;
    if (ok) {
        nvs_close(h);
    }
    if (!ok) {
        ESP_LOGE(TAG, "no setup info: HomeKit cannot start (provision with a local build)");
    }
    return ok;
}
