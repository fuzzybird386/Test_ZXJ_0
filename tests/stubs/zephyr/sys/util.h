#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#define ARG_UNUSED(x) (void)(x)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
