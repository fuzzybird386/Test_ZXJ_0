#include "bluetooth.h"
struct bt_conn;
struct bt_gatt_attr {
    uint16_t handle;
    const void *uuid;
    uint8_t properties,perm;
    ssize_t (*read)(struct bt_conn *,const struct bt_gatt_attr *,void *,uint16_t,uint16_t);
    ssize_t (*write)(struct bt_conn *,const struct bt_gatt_attr *,const void *,uint16_t,uint16_t,uint8_t);
};
#define BT_GATT_CHRC_READ 2
#define BT_GATT_CHRC_WRITE_WITHOUT_RESP 4
#define BT_GATT_CHRC_WRITE 8
#define BT_GATT_CHRC_NOTIFY 16
#define BT_GATT_PERM_READ 1
#define BT_GATT_PERM_WRITE 2
#define BT_GATT_CCC_NOTIFY 1
#define BT_GATT_WRITE_FLAG_PREPARE 1
#define BT_ATT_ERR_INVALID_OFFSET 7
#define BT_ATT_ERR_INVALID_ATTRIBUTE_LEN 13
#define BT_ATT_ERR_INSUFFICIENT_RESOURCES 17
#define BT_GATT_ERR(x) (-(x))
#define BT_GATT_PRIMARY_SERVICE(u) {.uuid=u}
#define BT_GATT_CHARACTERISTIC(u,p,m,r,w,d) {0}, {.uuid=u,.properties=p,.perm=m,.read=r,.write=w}
#define BT_GATT_CCC(fn,m) {0}
#define BT_GATT_SERVICE_DEFINE(n,...) struct bt_gatt_attr n##_attrs[]={__VA_ARGS__}; struct {struct bt_gatt_attr *attrs;} n={n##_attrs}
extern bool test_ctrl_sub,test_data_sub;
extern const struct bt_gatt_attr *test_ctrl_attr;
static inline bool bt_gatt_is_subscribed(struct bt_conn *c,const struct bt_gatt_attr *a,uint16_t value) { (void)c;(void)value;return a==test_ctrl_attr?test_ctrl_sub:test_data_sub; }
static inline uint16_t bt_gatt_get_mtu(struct bt_conn *c) { (void)c;return 23; }
static inline int bt_gatt_notify(struct bt_conn *c,const struct bt_gatt_attr *a,const void *p,uint16_t n) { (void)c;(void)a;(void)p;(void)n;return 0; }
static inline ssize_t bt_gatt_attr_read(struct bt_conn *c,const struct bt_gatt_attr *a,void *b,uint16_t n,uint16_t o,const void *p,uint16_t s) {
    (void)c;(void)a;if(o>s)return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    unsigned len=n<s-o?n:s-o;if(len)memcpy(b,(const uint8_t *)p+o,len);return len;
}
