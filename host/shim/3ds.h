// Minimal stand-ins for the libctru calls used by 3ds/source/http_3ds.cpp, so
// the console's network code can be tested on a desktop.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t Result;
#define R_FAILED(r) ((r) < 0)
#define R_SUCCEEDED(r) ((r) >= 0)
static inline Result PS_GenerateRandomBytes(void* out, size_t len) { arc4random_buf(out, len); return 0; }
static inline u64 osGetTime(void) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (u64)tv.tv_sec * 1000 + tv.tv_usec / 1000 + 2208988800000ULL;
}
static inline int closesocket(int fd) { return close(fd); }
