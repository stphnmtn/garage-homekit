#pragma once
#include <stdbool.h>

// HomeKit setup info (salt + verifier, never the code) lives in NVS, so firmware images hold
// no secret. A local build carrying secrets.h writes it; every build then reads it.
// Returns false when it is absent.
bool setup_init(void);
