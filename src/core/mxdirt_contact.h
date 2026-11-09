#ifndef MXDIRT_CONTACT_H
#define MXDIRT_CONTACT_H
#include <stdint.h>
#include "mxdirt_wheel_kinematics.h"

/* Per-wheel contact adapter: MX Bikes wheel samples (one per 2 ms physics
   step) -> one dirt-sim contact per outer frame. Pure copied data; never reads
   the process. Coordinates are MX Bikes world metres in, native units (feet,
   relative to the terrain centre) out. One serialized owner.

   Load: a first-order low-pass of the raw MX Bikes normal force (newtons),
   updated every physics sample: active samples pull it toward the measured
   force, airborne samples toward 0, with time constant MXDIRT_LOAD_TAU_S.
   Contact flag: the wheel's heightfield contact in the last physics sample of
   this outer frame (no hold-over frame). */
#define MXDIRT_LOAD_TAU_S .1f

typedef struct {
    float origin_metres[3], native_length_per_metre;
    uint64_t generation;
} MxdirtContactConfig;

/* One MX Bikes wheel-force sample (mxdirt_wheel_sample). */
typedef struct {
    MxbWheelKinematics body;
    float point_metres[3], normal[3];
    float radius_metres, normal_force_newtons, physics_dt;
    int active; /* fresh current normal contact */
    uint64_t generation, sequence;
} MxdirtPhysicsSample;

/* Post-integration wheel state at the end of one outer frame
   (mxdirt_frame_snapshot_controller_values) plus the host's contact policy. */
typedef struct {
    float controller_center_metres[3], center_velocity_metres[3];
    float angular_local_x, dt;
    float terrain_span_native;
    int qualified_contact;   /* last physics sample: current contact on the terrain heightfield */
    int deformation_enabled; /* surface may deform (material / in-game Deformation slider) */
    uint64_t generation, sequence, physics_sequence;
} MxdirtControllerFrame;

/* What the dirt session consumes for one wheel and one outer frame. */
typedef struct {
    float point[3];           /* contact point, native units, terrain-centred */
    float forward[3];         /* unit longitudinal axis, signed along travel */
    float lateral[3];         /* unit lateral axis */
    float center_velocity[3]; /* wheel centre velocity, native units/s */
    float slip[3];            /* contact-point velocity, native units/s: [0] lateral,
                                 [1] normal, [2] along the unsigned longitudinal axis */
    float direction;          /* +1/-1: forward = longitudinal*direction */
    float load_n;             /* filtered normal load, newtons */
    float frame_mean_n;       /* mean raw load of this frame's active samples (0 if none) */
    float frame_slow_n;       /* ~0.6 s running mean of the raw load */
    uint32_t frame_count;     /* active samples this frame */
    float dt;                 /* outer frame seconds */
    int contact;              /* in contact this frame */
    int deformation;          /* deformation allowed for this contact */
} MxdirtContact;

typedef struct {
    MxdirtContactConfig config;
    float load_n;
    float radius_vector_metres[3], longitudinal[3], lateral[3], contact_velocity_native[3];
    float load_sum_n, load_slow_n;
    uint32_t load_count;
    uint64_t physics_sequence, controller_sequence;
    int initialized, wheel, has_physics, has_geometry, has_controller;
} MxdirtContactBridge;

int mxdirt_contact_init(MxdirtContactBridge *, const MxdirtContactConfig *, int wheel); /* 0 front, 1 rear */
/* Once per physics sample. Repeats, stale generations, nonfinite/degenerate
   inputs are rejected without changing state. Active geometry places the
   contact at normalize(point-center)*radius. Inactive samples keep the last
   contact geometry, zero the point velocity and decay the load. */
int mxdirt_contact_physics(MxdirtContactBridge *, const MxdirtPhysicsSample *);
/* Once per outer frame, after its physics samples (physics_sequence must be the
   last accepted one). Output and state are unchanged on failure. Restarts the
   per-frame raw load accumulation. */
int mxdirt_contact_frame(MxdirtContactBridge *, const MxdirtControllerFrame *, MxdirtContact *);
#endif
