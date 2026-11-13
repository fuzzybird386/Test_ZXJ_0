#ifndef TEST_KERNEL_H
#define TEST_KERNEL_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
extern int64_t test_now;
struct k_thread { int unused; };
#define K_FOREVER -1
#define K_NO_WAIT 0
#define K_MSGQ_DEFINE(name,size,count,align) int name
#define K_SEM_DEFINE(name,initial,limit) int name
#define K_MUTEX_DEFINE(name) int name
#define K_THREAD_STACK_DEFINE(name,size) uint8_t name[size]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
static inline void k_mutex_lock(int *p,int t) { (void)p;(void)t; }
static inline void k_mutex_unlock(int *p) { (void)p; }
static inline int k_msgq_put(int *p,const void *b,int t) { (void)p;(void)b;(void)t;return 0; }
static inline int k_msgq_get(int *p,void *b,int t) { (void)p;(void)b;(void)t;return -1; }
static inline void k_sem_take(int *p,int t) { (void)p;(void)t; }
static inline void k_sem_give(int *p) { (void)p; }
static inline int64_t k_uptime_get(void) { return test_now; }
static inline void k_msleep(int64_t ms) { test_now+=ms; }
static inline void k_thread_create(struct k_thread *p,uint8_t *s,size_t n,void (*fn)(void *,void *,void *),void *a,void *b,void *c,int priority,int options,int delay)
{ (void)p;(void)s;(void)n;(void)fn;(void)a;(void)b;(void)c;(void)priority;(void)options;(void)delay; }
static inline void k_thread_name_set(struct k_thread *p,const char *n) { (void)p;(void)n; }
#endif
