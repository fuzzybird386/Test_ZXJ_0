/* Actual GPIO/LED state machine against logical (active-low aware) pin stubs. */
#include "../src/board_io.c"
volatile unsigned checks;
int64_t test_now,test_power_cut_at;
int test_pins[3];
#define CHECK(x) do { ++checks; if (!(x)) return __LINE__; } while (0)
static void tick(int64_t ms) { test_now=ms;led_update(&led_work.work); }
int run_tests(void) {
    CHECK(board_io_init()==0 && test_pins[0]==1);
    tick(0);CHECK(test_pins[2]==1 && !test_pins[1]);
    tick(500);CHECK(!test_pins[2] && !test_pins[1]);
    tick(1000);CHECK(test_pins[2]==1);
    board_led_connected(true);
    tick(1500);CHECK(test_pins[2]==1);
    tick(2000);CHECK(test_pins[2]==1);
    board_led_error(true);tick(2250);CHECK(test_pins[2]==1 && test_pins[1]);
    board_led_connected(false);board_led_shutdown();
    tick(2250);CHECK(!test_pins[2] && test_pins[1] && test_pins[0]);
    tick(2500);CHECK(!test_pins[2] && !test_pins[1] && test_pins[0]);
    tick(2750);CHECK(!test_pins[2] && test_pins[1] && test_pins[0]);
    tick(5000);CHECK(!test_pins[2] && !test_pins[1] && test_pins[0]);
    CHECK(board_power_off()==0);
    CHECK(test_power_cut_at==5250 && !test_pins[0] && !test_pins[1] && !test_pins[2] && !led_work.pending);
    /* Slow cleanup already consumed the warning interval: do not wait again. */
    test_now=10000;CHECK(board_io_init()==0);board_led_shutdown();
    tick(14000);CHECK(board_power_off()==0 && test_power_cut_at==14000);
    return 0;
}
