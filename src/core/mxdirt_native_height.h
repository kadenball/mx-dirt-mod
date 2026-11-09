#ifndef MXB_NATIVE_HEIGHT_H
#define MXB_NATIVE_HEIGHT_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
 uint32_t width,height;const int16_t *samples;size_t sample_count;
 float extent_x,extent_z,height_scale,origin[3];
} MxbNativeHeightGrid;
typedef struct {float height,normal[3];} MxbNativeHeightPoint;
/* MX Bikes height lookup (mxbikes.exe+1f1720) arithmetic, using an explicit immutable snapshot.
   Returns1 in bounds,0 for invalid input/outside; failure leaves output unchanged.
   Inclusive x/z bounds, signed16 heights, bilinear height, clamped neighbor
   tangent interpolation. Normal is native cross(normalized tangentZ,tangentX),
   NOT normalized afterward and not necessarily unit length. No live memory/I/O.
   Safe domain:2..8193 dimensions, finite positive extents, finite scale/origin,
   enough samples. Overflow/nonfinite arithmetic is rejected. Caller owns sample
   lifetime and coherence; pointer/span validity is not inferred by this API.
   For native bit parity compile without fast-math/FMA contraction and use the
   default round-to-nearest FP environment (as in the private original oracle). */
int mxb_native_height_query(const MxbNativeHeightGrid *,float world_x,float world_z,MxbNativeHeightPoint *);
#endif
