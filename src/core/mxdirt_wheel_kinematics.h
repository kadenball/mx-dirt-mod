#ifndef MXB_WHEEL_KINEMATICS_H
#define MXB_WHEEL_KINEMATICS_H

#include <stddef.h>
#include <string.h>

/* MX Bikes body layout of the one supported mxbikes.exe (SHA-256 pinned by the plugin).
   B is bike; W = B + 0x60f8 + index * 0xe310 (front=0, rear=1).
   The caller must capture the pointed-to body, not merely this pointer.
   These functions never read another process or modify game state. */
enum {
    MXB_WHEEL_BODY_POINTER = 0xdfb8,
    MXB_BODY_CAPTURE_OFFSET = 0xc0,
    MXB_BODY_CAPTURE_BYTES = 0x6c
};

typedef struct MxbWheelKinematics {
    float center[3];              /* Body+c0, world position. */
    float rotation[12];           /* Body+d0, row-major 3x4 rotation. */
    float linear_world[3];        /* Body+110, center velocity. */
    float angular_world[3];       /* Body+120, radians/second. */
} MxbWheelKinematics;

/* Input is one coherent byte snapshot of Body+c0 through Body+12b.
   Rejects short snapshots; pointer validity/synchronization are caller duties. */
static inline int mxb_wheel_decode_body(MxbWheelKinematics *out,
    const void *body_c0_snapshot, size_t bytes)
{
    const unsigned char *p = (const unsigned char *)body_c0_snapshot;
    if (!out || !p || bytes < MXB_BODY_CAPTURE_BYTES) return 0;
    memcpy(out->center, p, 12);
    memcpy(out->rotation, p + 0x10, 48);
    memcpy(out->linear_world, p + 0x50, 12);
    memcpy(out->angular_world, p + 0x60, 12);
    return 1;
}

/* Same scalar ordering as MX Bikes' point-velocity helper (mxbikes.exe RVA 2ace60):
   v(point) = v(center) + omega_world x (point_world - center_world).
   No subtraction of surface motion; this is rigid-body point velocity.
   Compile without fast-math/FMA contraction for scalar instruction parity. */
static inline void mxb_wheel_point_velocity(float out[3],
    const MxbWheelKinematics *body, const float point_world[3])
{
    const float x = point_world[0] - body->center[0];
    const float y = point_world[1] - body->center[1];
    const float z = point_world[2] - body->center[2];
    const float wx = body->angular_world[0];
    const float wy = body->angular_world[1];
    const float wz = body->angular_world[2];
    const float vx = (wy*z - wz*y) + body->linear_world[0];
    const float vy = (wz*x - wx*z) + body->linear_world[1];
    const float vz = (wx*y - wy*x) + body->linear_world[2];
    out[0] = vx; out[1] = vy; out[2] = vz;
}

/* As MX Bikes (mxbikes.exe RVA 266eb0, applied to Body+120 at 1afdb1..1afde3).
   R transpose multiplies world angular velocity; no translation applies.
   X is promoted to double and stored at W+c110 by native code.
   Preserve asymmetric addition order used by the X component. */
static inline void mxb_wheel_angular_local(float out[3],
    const MxbWheelKinematics *body)
{
    const float *r = body->rotation, *w = body->angular_world;
    const float x = (w[1]*r[4] + w[0]*r[0]) + w[2]*r[8];
    const float y = (w[0]*r[1] + w[1]*r[5]) + w[2]*r[9];
    const float z = (w[0]*r[2] + w[1]*r[6]) + w[2]*r[10];
    out[0] = x; out[1] = y; out[2] = z;
}

#endif
