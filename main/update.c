#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ota.h"

static const char *TAG = "update";

#define URL CONFIG_GARAGE_UPDATE_URL
static update_idle_fn idle;
static char last[48] = "none yet";

static void note(const char *msg)
{
    snprintf(last, sizeof(last), "%s", msg);
}

const char *update_last(void) { return last; }

// esp_http_client_get_header reads request headers; response headers come through events.
static char date[40];

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Date")) {
        snprintf(date, sizeof(date), "%s", e->header_value);
    }
    return ESP_OK;
}

static esp_http_client_config_t http_cfg(const char *url)
{
    return (esp_http_client_config_t){
        .url = url,
        .event_handler = on_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        // GitHub answers with a redirect to a signed URL of several hundred bytes.
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .timeout_ms = 20000,
        .keep_alive_enable = false,
    };
}

// The server's Date header ("Sun, 23 Nov 2026 02:15:00 GMT") stands in for a clock: the
// device keeps none, and certificate expiry is not checked (CONFIG_MBEDTLS_HAVE_TIME_DATE
// off); the image signature is what is trusted, not the transport.
static int hour_from_date(const char *d)
{
    if (!d || strlen(d) < 20 || d[19] != ':') {
        return -1;
    }
    return atoi(d + 17);
}

// Latest released version, or -1. *hour gets the UTC hour of the answer, or -1.
static long fetch_version(int *hour)
{
    esp_http_client_config_t cfg = http_cfg(URL "/version.txt");
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    long v = -1;
    *hour = -1;
    date[0] = 0;
    char body[16] = { 0 };
    if (esp_http_client_open(c, 0) != ESP_OK) {
        goto out;
    }
    for (int redirects = 0;; redirects++) {
        if (esp_http_client_fetch_headers(c) < 0) {
            goto out;
        }
        int st = esp_http_client_get_status_code(c);
        if ((st == 301 || st == 302 || st == 307 || st == 308) && redirects < 5) {
            esp_http_client_flush_response(c, NULL);
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            if (esp_http_client_open(c, 0) != ESP_OK) {
                goto out;
            }
            continue;
        }
        if (st != 200) {
            ESP_LOGW(TAG, "version.txt: HTTP %d", st);
            goto out;
        }
        break;
    }
    *hour = hour_from_date(date);
    if (esp_http_client_read(c, body, sizeof(body) - 1) > 0) {
        char *end;
        v = strtol(body, &end, 10);
        if (end == body) {
            v = -1;
        }
    }
out:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return v;
}

static bool in_window(int hour)
{
    return hour >= CONFIG_GARAGE_UPDATE_HOUR_FROM && hour < CONFIG_GARAGE_UPDATE_HOUR_TO;
}

// Downloads into the passive slot. esp_https_ota_finish -> esp_ota_end checks the signature
// (CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) before the slot is made bootable.
static bool install(long running)
{
    esp_http_client_config_t http = http_cfg(URL "/garage-homekit-signed.bin");
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;
    if (esp_https_ota_begin(&cfg, &h) != ESP_OK) {
        note("download failed");
        return false;
    }
    esp_app_desc_t d;
    if (esp_https_ota_get_img_desc(h, &d) != ESP_OK || strtol(d.version, NULL, 10) <= running) {
        // version.txt and the image can disagree for a moment while a release is uploaded.
        esp_https_ota_abort(h);
        note("image not newer");
        return false;
    }
    esp_err_t err;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        note("download failed");
        return false;
    }
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "v%s refused: %s", d.version, esp_err_to_name(err));
        note("image refused (signature)");
        return false;
    }
    ESP_LOGW(TAG, "v%s installed", d.version);
    snprintf(last, sizeof(last), "v%s installed", d.version);
    return true;
}

static void task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(120 * 1000));  // let a fresh image confirm itself first
    for (;; vTaskDelay(pdMS_TO_TICKS(CONFIG_GARAGE_UPDATE_INTERVAL_S * 1000))) {
        long running = strtol(esp_app_get_description()->version, NULL, 10);
        int hour;
        long latest = fetch_version(&hour);
        if (latest < 0) {
            note("check failed");
            continue;
        }
        if (latest <= running) {
            note("up to date");
            continue;
        }
        if (!in_window(hour)) {
            snprintf(last, sizeof(last), "v%ld waiting for the night", latest);
            continue;
        }
        if (!idle()) {
            snprintf(last, sizeof(last), "v%ld waiting for the door", latest);
            continue;
        }
        ESP_LOGW(TAG, "v%ld available, installing", latest);
        if (!install(running)) {
            continue;
        }
        // The new image is staged; reboot only into a quiet garage.
        while (!idle()) {
            vTaskDelay(pdMS_TO_TICKS(10 * 1000));
        }
        ota_reboot();
        vTaskDelete(NULL);
    }
}

void update_start(update_idle_fn idle_fn)
{
    idle = idle_fn;
    xTaskCreate(task, "update", 8192, NULL, 3, NULL);
}
