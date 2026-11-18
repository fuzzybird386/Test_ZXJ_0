#pragma once
#ifndef AD5941_PORT_H__
#define AD5941_PORT_H__

#include <stdint.h>

/**
 * Initialise the Zephyr port for the AD5940 driver:
 *  - acquires the SPI controller and the CS / RESET / INT GPIOs
 *  - hard-resets the AFE
 *
 * Must be called once before any AD5940 lib API. It is idempotent and is
 * also reachable via `AD5940_MCUResourceInit(NULL)` so the AD5940
 * application examples work unmodified.
 */
int ad5941_port_init(void);
int ad5941_port_error(void);
void ad5941_port_clear_error(void);

#endif /* AD5941_PORT_H__ */
