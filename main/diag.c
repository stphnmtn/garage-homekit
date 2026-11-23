#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>

#define RING_SIZE 8192

static char ring[RING_SIZE];
static size_t head;   // next write position
static bool wrapped;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t uart_vprintf;
static diag_status_fn status_fn;

static int hook(const char *fmt, va_list ap)
{
    char line[192];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > (int)sizeof(line) - 1) {
        n = sizeof(line) - 1;
    }
    taskENTER_CRITICAL(&lock);
    for (int i = 0; i < n; i++) {
        ring[head++] = line[i];
        if (head == RING_SIZE) {
            head = 0;
            wrapped = true;
        }
    }
    taskEXIT_CRITICAL(&lock);
    return uart_vprintf(fmt, ap);
}

void diag_init(void) { uart_vprintf = esp_log_set_vprintf(hook); }

static esp_err_t log_get(httpd_req_t *req)
{
    char *out = malloc(RING_SIZE);
    if (!out) {
        return httpd_resp_send_500(req);
    }
    size_t len;
    taskENTER_CRITICAL(&lock);
    if (wrapped) {
        memcpy(out, ring + head, RING_SIZE - head);
        memcpy(out + RING_SIZE - head, ring, head);
        len = RING_SIZE;
    } else {
        memcpy(out, ring, head);
        len = head;
    }
    taskEXIT_CRITICAL(&lock);
    httpd_resp_set_type(req, "text/plain");
    esp_err_t r = httpd_resp_send(req, out, len);
    free(out);
    return r;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char out[384];
    status_fn(out, sizeof(out));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, out);
}

void diag_register(httpd_handle_t srv, diag_status_fn status)
{
    if (!srv) {
        return;
    }
    status_fn = status;
    httpd_uri_t l = { .uri = "/log", .method = HTTP_GET, .handler = log_get };
    httpd_uri_t s = { .uri = "/status", .method = HTTP_GET, .handler = status_get };
    httpd_register_uri_handler(srv, &l);
    httpd_register_uri_handler(srv, &s);
}
