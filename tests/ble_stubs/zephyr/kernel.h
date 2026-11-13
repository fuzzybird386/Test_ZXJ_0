#ifndef TEST_BLE_KERNEL_H
#define TEST_BLE_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>
extern int64_t test_now;
#define CONFIG_BT_DEVICE_NAME "CQET"
#define CONFIG_BT_ID_MAX 1
#define CONFIG_ECW_DISCONNECT_POWEROFF 1
#define CONFIG_ECW_POWER_HOLD 1
#define BIT(n) (1U<<(n))
#define IS_ENABLED(x) (x)
#define CLAMP(v,l,h) MIN(MAX(v,l),h)
#define K_FOREVER -1
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define K_SECONDS(x) ((x)*1000)
struct k_thread { int unused; };
struct k_work { int unused; };
struct k_work_sync { int unused; };
struct k_work_delayable { struct k_work work; void (*fn)(struct k_work *); int delay; bool pending; };
struct test_queue { uint8_t *data; unsigned size,capacity,head,count; };
#define K_WORK_DELAYABLE_DEFINE(n,f) struct k_work_delayable n={.fn=f}
#define K_MSGQ_DEFINE(n,s,c,a) uint8_t n##_buffer[(s)*(c)]; struct test_queue n={n##_buffer,s,c,0,0}
#define K_SEM_DEFINE(n,i,l) int n=i
#define K_MUTEX_DEFINE(n) int n
#define K_THREAD_STACK_DEFINE(n,s) uint8_t n[s]
#define K_THREAD_STACK_SIZEOF(n) sizeof(n)
static inline void k_mutex_lock(int *p,int t) { (void)p;(void)t; }
static inline void k_mutex_unlock(int *p) { (void)p; }
static inline int k_msgq_put(struct test_queue *q,const void *p,int t) {
    (void)t;if(q->count==q->capacity)return -ENOMSG;
    memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,p,q->size);++q->count;return 0;
}
static inline int k_msgq_get(struct test_queue *q,void *p,int t) {
    (void)t;if(!q->count)return -ENOMSG;
    memcpy(p,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;--q->count;return 0;
}
static inline void k_sem_take(int *p,int t) { (void)t;*p=0; }
static inline void k_sem_give(int *p) { *p=1; }
static inline int64_t k_uptime_get(void) { return test_now; }
static inline uint32_t k_uptime_get_32(void) { return test_now; }
static inline void k_msleep(int64_t ms) { test_now+=ms; }
static inline int k_work_reschedule(struct k_work_delayable *p,int delay) { p->pending=true;p->delay=delay;return 1; }
static inline int k_work_schedule(struct k_work_delayable *p,int delay) { return k_work_reschedule(p,delay); }
static inline int k_work_cancel_delayable_sync(struct k_work_delayable *p,struct k_work_sync *s) { (void)s;p->pending=false;return 0; }
static inline void k_thread_create(struct k_thread *p,uint8_t *s,size_t n,void (*fn)(void *,void *,void *),void *a,void *b,void *c,int priority,int options,int delay)
{ (void)p;(void)s;(void)n;(void)fn;(void)a;(void)b;(void)c;(void)priority;(void)options;(void)delay; }
static inline void k_thread_name_set(struct k_thread *p,const char *n) { (void)p;(void)n; }
#endif
