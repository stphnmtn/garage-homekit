#pragma once

// LAN OTA: `POST /ota` with a signed app image on port 8032. ESP-IDF rejects images not
// signed with our key; this module also rejects images whose version is lower than ours.
#include <esp_http_server.h>

httpd_handle_t ota_start(void);  // NULL on failure; diag.c adds its pages to it
