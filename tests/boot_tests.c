/* Run the actual entry point with failures at board, BLE and AFE startup. */
#define main firmware_main
#include "../src/main.c"
#undef main
volatile unsigned checks;
int64_t test_now;
static int board_rc, ble_rc, afe_rc, order, ble_order, afe_order, violation;
static bool red;
#define CHECK(x) do { ++checks; if (!(x)) return __LINE__; } while (0)
int board_io_init(void) { ++order; return board_rc; }
void methods_init(void) { ++order; }
int ble_link_init(void) { ble_order=++order; return ble_rc; }
int afe_init(void) {
    afe_order=++order;
    /* A phone can already connect, but START/RESET must stay guarded. */
    if (afe_boot_finished()) violation=1;
    return afe_rc;
}
uint16_t afe_chip_id(void) { return 0x5502; }
void board_led_error(bool error) { red=error; }
void ble_log(uint8_t level,uint16_t code,const char *message) {
    (void)level;(void)code;(void)message;
    if (!afe_boot_finished()) violation=2;
}
static void reset(void) {
    board_rc=ble_rc=afe_rc=order=ble_order=afe_order=violation=0;
    red=false;atomic_clear(&boot_finished);
}
int run_tests(void) {
    reset(); CHECK(firmware_main()==0);
    CHECK(ble_order && afe_order>ble_order && afe_boot_finished() && !violation && !red);
    reset();afe_rc=-ENODEV;CHECK(firmware_main()==0);
    CHECK(ble_order && afe_order>ble_order && afe_boot_finished() && !violation && red);
    reset();ble_rc=-EIO;CHECK(firmware_main()==-EIO);
    CHECK(ble_order && !afe_order && !afe_boot_finished() && red);
    reset();board_rc=-ENODEV;CHECK(firmware_main()==-ENODEV);
    CHECK(!ble_order && !afe_order && !afe_boot_finished());
    return 0;
}
