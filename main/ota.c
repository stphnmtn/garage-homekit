#include <sdkconfig.h>
#include "ota.h"

#include <stdlib.h>
#include <string.h>
#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "ota";

static void reboot_task(void *arg)
{
#if CONFIG_GARAGE_QEMU
    // QEMU does not reset peripherals on a software reset (stale timer IRQ at boot): the
    // test harness power-cycles the emulator within this window instead.
    vTaskDelay(pdMS_TO_TICKS(30000));
#else
    vTaskDelay(pdMS_TO_TICKS(1000));
#endif
    esp_restart();
}

void ota_reboot(void)
{
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *msg, esp_ota_handle_t h)
{
    if (h) {
        esp_ota_abort(h);
    }
    ESP_LOGE(TAG, "%s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next || req->content_len == 0 || req->content_len > next->size) {
        return fail(req, "400 Bad Request", "bad size\n", 0);
    }
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(next, req->content_len, &h) != ESP_OK) {
        return fail(req, "500 Internal Server Error", "ota begin failed\n", 0);
    }
    static char buf[4096];
    size_t left = req->content_len;
    while (left > 0) {
        int n = httpd_req_recv(req, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return fail(req, "400 Bad Request", "receive failed\n", h);
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            return fail(req, "500 Internal Server Error", "write failed\n", h);
        }
        left -= n;
    }
    // Checks the image and, with CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT, its signature.
    if (esp_ota_end(h) != ESP_OK) {
        return fail(req, "403 Forbidden", "image invalid or not signed with our key\n", 0);
    }
    esp_app_desc_t incoming;
    if (esp_ota_get_partition_description(next, &incoming) != ESP_OK) {
        return fail(req, "400 Bad Request", "no app description\n", 0);
    }
    long running = strtol(esp_app_get_description()->version, NULL, 10);
    long candidate = strtol(incoming.version, NULL, 10);
    if (candidate < running) {
        return fail(req, "409 Conflict", "older version refused\n", 0);
    }
    if (esp_ota_set_boot_partition(next) != ESP_OK) {
        return fail(req, "500 Internal Server Error", "set boot failed\n", 0);
    }
    ESP_LOGI(TAG, "version %s accepted, rebooting", incoming.version);
    httpd_resp_sendstr(req, "ok, rebooting\n");
    ota_reboot();
    return ESP_OK;
}

static esp_err_t version_get(httpd_req_t *req)
{
    char out[64];
    snprintf(out, sizeof(out), "%s\n", esp_app_get_description()->version);
    return httpd_resp_sendstr(req, out);
}

httpd_handle_t ota_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8032;
    cfg.ctrl_port = 32770;  // HAP's own server uses the default control port
    cfg.max_open_sockets = 2;
    cfg.max_uri_handlers = 4;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "server start failed");
        return NULL;
    }
    httpd_uri_t post = { .uri = "/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_uri_t ver = { .uri = "/version", .method = HTTP_GET, .handler = version_get };
    httpd_register_uri_handler(srv, &post);
    httpd_register_uri_handler(srv, &ver);
    return srv;
}
