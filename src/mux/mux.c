/* No external mux exists on this PCB. This API only validates WE0.
 * AFE shutdown is performed explicitly by the measurement backend. */
#include "mux.h"
#include <errno.h>
int mux_init(void) { return 0; }
int mux_select_we(uint8_t index) { return (index == 0 || index == 255) ? 0 : -ENOTSUP; }
void mux_disable(void) {}
