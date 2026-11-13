#ifndef TEST_BT_H
#define TEST_BT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
struct bt_uuid { uint8_t type; };
struct bt_uuid_16 { struct bt_uuid uuid; uint16_t val; };
#define BT_UUID_INIT_16(v) {{16},v}
struct bt_data { uint8_t type,data_len; const uint8_t *data; };
#define BT_DATA(t,p,n) {t,n,(const uint8_t *)(p)}
#define BT_DATA_BYTES(t,...) BT_DATA(t,((const uint8_t[]){__VA_ARGS__}),sizeof((const uint8_t[]){__VA_ARGS__}))
#define BT_DATA_FLAGS 1
#define BT_DATA_UUID16_ALL 3
#define BT_DATA_NAME_COMPLETE 9
#define BT_LE_AD_GENERAL 2
#define BT_LE_AD_NO_BREDR 4
#define BT_LE_ADV_CONN_FAST_1 ((void *)1)
typedef struct { uint8_t data[7]; } bt_addr_le_t;
#define BT_ADDR_LE_STR_LEN 30
int bt_enable(void *);
int bt_disable(void);
int bt_le_adv_start(const void *,const struct bt_data *,size_t,const struct bt_data *,size_t);
static inline void bt_id_get(bt_addr_le_t *p,size_t *n) { (void)p;*n=0; }
static inline int bt_addr_le_to_str(const bt_addr_le_t *p,char *s,size_t n) { (void)p;(void)s;(void)n;return 0; }
#endif
