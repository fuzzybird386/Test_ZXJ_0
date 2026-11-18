#pragma once

#include <stdint.h>

typedef struct {
    uint8_t level_pct; /* Battery level 0–100 % */
} battery_measurement_t;

int battery_init(void);
int battery_refresh(battery_measurement_t *out);
