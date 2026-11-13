#ifndef TEST_ATOMIC_H
#define TEST_ATOMIC_H
#include <stdint.h>
typedef int32_t atomic_t;
typedef int32_t atomic_val_t;
static inline int atomic_get(const atomic_t *p) { return *p; }
static inline int atomic_set(atomic_t *p,int n) { int old=*p;*p=n;return old; }
static inline int atomic_clear(atomic_t *p) { return atomic_set(p,0); }
static inline int atomic_inc(atomic_t *p) { return (*p)++; }
static inline int atomic_cas(atomic_t *p,int old,int value) { if(*p!=old)return 0;*p=value;return 1; }
static inline int atomic_or(atomic_t *p,int value) { int old=*p;*p|=value;return old; }
#endif
