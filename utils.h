#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <pthread.h>

#define now_ms() \
({ \
	struct timeval tv; \
	gettimeofday(&tv, NULL); \
	(tv.tv_sec * 1000 + tv.tv_usec / 1000); \
})

#define now_us() \
({ \
	struct timeval tv; \
	gettimeofday(&tv, NULL); \
	(tv.tv_sec * 1000000 + tv.tv_usec); \
})

#ifdef __DEBUG_LOG__
#define LOG_DEBUG(fmt, ...) \
do { \
	printf("[time:%ld, tid:%u] " fmt "\n", now_ms(), gettid(), ##__VA_ARGS__); \
} while (0)
#else
#define LOG_DEBUG(fmt, ...)
#endif

#define LOG(fmt, ...) \
do { \
	printf("[time:%ld, tid:%u] " fmt "\n", now_ms(), gettid(), ##__VA_ARGS__); \
} while (0)

inline pid_t gettid()
{
	return syscall(SYS_gettid);
}

#endif
