#pragma once
#include <stdbool.h>

// Writes the bootloader embedded in this image over flash 0x1000 when they differ.
// false: the region may now be corrupt; the caller retries before anything reboots.
bool bootloader_update(void);
