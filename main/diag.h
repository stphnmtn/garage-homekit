#pragma once
#include <stddef.h>
#include <esp_http_server.h>

// Diagnostics kept in RAM only (never flash, to spare it): the last log lines and a status
// line, readable on the LAN at GET :8032/log and GET :8032/status. Lost on reboot.
typedef int (*diag_status_fn)(char *buf, size_t len);

void diag_init(void);  // call first: mirrors every ESP_LOG line into the RAM ring
void diag_register(httpd_handle_t srv, diag_status_fn status);
