#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#define ARG_UNUSED(x) (void)(x)
uint32_t k_uptime_get_32(void);
