#ifndef MXDIRT_HEIGHT_BRIDGE_H
#define MXDIRT_HEIGHT_BRIDGE_H
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include "mxdirt_height_lookup.h"

/* MX Bikes PHYSICS base/height buffers are signed int16; its renderer
   separately uses a biased uint16 grid. Positive signed 16.16 deformation
   excavates. Do not reinterpret raw physics base bits as unsigned when
   computing these bounds. No game memory is owned. */
static inline void mxb_height_fixed_bounds(int16_t physics_base,int32_t *lower,int32_t *upper) {
    int64_t lo=((int64_t)physics_base-INT16_MAX)*65536;
    int64_t hi=((int64_t)physics_base-INT16_MIN+1)*65536-1;
    *lower=(int32_t)(lo<INT32_MIN ? INT32_MIN : lo);
    *upper=(int32_t)(hi>INT32_MAX ? INT32_MAX : hi);
}

/* A complete target reversal can exceed one int32 transaction delta. Return
   one representable chunk; the queue must acknowledge it before recomputing
   the remaining chunk. This function never silently wraps target-acknowledged. */
static inline int32_t mxb_height_delta_chunk(int32_t target,int32_t acknowledged) {
    int64_t difference=(int64_t)target-acknowledged;
    return difference<INT32_MIN ? INT32_MIN : difference>INT32_MAX ? INT32_MAX : (int32_t)difference;
}
#endif
