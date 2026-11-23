#pragma once
#include <stdbool.h>

// Pulls signed releases: checks CONFIG_GARAGE_UPDATE_URL/version.txt every
// CONFIG_GARAGE_UPDATE_INTERVAL_S and installs a newer image only inside the night window and
// while idle_fn() holds. Signature, version and rollback checks are the same as for a push.
typedef bool (*update_idle_fn)(void);
void update_start(update_idle_fn idle_fn);
const char *update_last(void);  // outcome of the last check, for /status
