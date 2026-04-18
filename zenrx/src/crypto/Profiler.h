#pragma once

#ifndef FORCE_INLINE
#if defined(_MSC_VER)
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__)
#define FORCE_INLINE __attribute__((always_inline)) inline
#elif defined(__clang__)
#define FORCE_INLINE __inline__
#else
#define FORCE_INLINE
#endif
#endif

#define PROFILE_SCOPE(x)

#include "crypto/randomx/blake2/blake2.h"

struct rx_blake2b_wrapper
{
    FORCE_INLINE static void run(void* out, size_t outlen, const void* in, size_t inlen)
    {
        PROFILE_SCOPE(RandomX_Blake2b);
        rx_blake2b(out, outlen, in, inlen);
    }
};
