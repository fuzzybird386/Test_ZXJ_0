#pragma once
#ifndef MUX_H__
#define MUX_H__

#include <stdint.h>

/* Compatibility API: accepts WE0 only. No physical mux or isolation switch. */
int  mux_init(void);

int  mux_select_we(uint8_t we_index);

void mux_disable(void);

#endif /* MUX_H__ */
