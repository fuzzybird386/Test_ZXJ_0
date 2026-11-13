#include "bluetooth.h"
struct bt_conn { int refs; };
struct bt_conn_cb { void (*connected)(struct bt_conn *,uint8_t); void (*disconnected)(struct bt_conn *,uint8_t); void (*recycled)(void); };
#define BT_CONN_CB_DEFINE(n) struct bt_conn_cb n
static inline struct bt_conn *bt_conn_ref(struct bt_conn *c) { ++c->refs;return c; }
static inline void bt_conn_unref(struct bt_conn *c) { --c->refs; }
