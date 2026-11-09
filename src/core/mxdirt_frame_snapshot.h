#ifndef MXB_FRAME_SNAPSHOT_H
#define MXB_FRAME_SNAPSHOT_H
#include <stdint.h>
#include <stddef.h>
#include "mxdirt_contact.h"

/* Read-only adapter for MX Bikes' outer frame scheduler (supported mxbikes.exe).
   A reader copies exactly bytes bytes, returning 1 on success, 0 on failure.
   Addresses are integer addresses in the supplied image/address space. */
typedef int (*MxbFrameRead)(void *user, uint64_t address, void *out, size_t bytes);

typedef enum {
    MXB_FRAME_OK=1, MXB_FRAME_NO_NEW_STEP=2, MXB_FRAME_WAITING=3,
    MXB_FRAME_PAUSED=4,
    MXB_FRAME_INVALID=-1, MXB_FRAME_BUILD=-2, MXB_FRAME_READ=-3,
    MXB_FRAME_REENTRY=-4, MXB_FRAME_IDENTITY=-5, MXB_FRAME_BODY_RESET=-6,
    MXB_FRAME_SEQUENCE=-7, MXB_FRAME_SCHEDULER=-8, MXB_FRAME_DATA=-9,
    MXB_FRAME_REBIND_REQUIRED=-10
} MxbFrameResult;

typedef struct {
    uint64_t image_base, image_bytes, callsite, original_target, return_address;
    unsigned char original_call[5];
} MxbFrameCallsitePlan;

/* Validate BEFORE a host patches this call. This function never writes memory.
   verified_file_sha256 must be the host-verified on-disk executable digest;
   passing a literal instead of checking the file does not verify its identity.
   Signature at callsite: int scheduler(int elapsed_ms), Windows x64 ECX/EAX.
   A host wrapper must always call original once, including failed begin(). */
int mxb_frame_callsite_plan(MxbFrameRead, void *user, uint64_t image_base,
    uint64_t image_bytes, const char *verified_file_sha256, MxbFrameCallsitePlan *);

typedef struct {
    uint64_t generation, bike, thread;
    int testing, track_allowed, running;
} MxbFrameGate;

typedef struct {
    MxbFrameRead read;
    void *user;
    MxbFrameCallsitePlan plan;
    uint64_t generation, bike, owner_thread;
    /* 0 means1 on first attachment. Rebinding a surviving session must use
       session.outer_sequence+1; reprime preserves that session counter. */
    uint64_t first_outer_sequence;
    /* Explicit host rejection policy, not an MX Bikes dt clamp.
       1..100 ms, matching the receiving contact bridge's maximum .1 seconds. */
    unsigned max_elapsed_ms;
} MxbFrameSnapshotConfig;

typedef struct {
    uint64_t generation, bike, thread, sequence, body_address[2];
    unsigned new_steps;
    int elapsed_ms, pending_physics_ms, history_restarted;
    float dt, angular_local[2][3];
    MxdirtPhysicsSample last_preintegration[2];
    MxbWheelKinematics postintegration[2];
} MxbFrameSnapshot;

/* One serialized owner. The thread stamp is a validity check, NOT a mutex.
   The host must serialize callbacks and exclude asynchronous body mutation
   while finish() copies. Repeat reads detect observed drift, not arbitrary
   ABA races. The structure must not alias arguments or reader output storage. */
typedef struct {
    MxbFrameSnapshotConfig config;
    uint64_t body_address[2], next_sequence;
    MxdirtPhysicsSample latest[2];
    MxbFrameSnapshot published;
    unsigned new_steps;
    int initialized, in_call, busy, fault, needs_rebind;
    int elapsed_ms, next_wheel, has_pair, has_published, paused, restarted;
} MxbFrameSnapshotState;

/* init/rebind only at a quiescent boundary. Generation must match the receiving
   session. For a same-generation reset, discard all old samples and reprime
   the receiving session BEFORE fresh physics; retain a monotonic outer
   sequence. A generation change requires a matching session change.
   This function does not install a hook. */
int mxb_frame_snapshot_init(MxbFrameSnapshotState *, const MxbFrameSnapshotConfig *);
/* Immediately BEFORE the original RVA135d70 call. Only OK arms capture.
   PAUSED includes non-Testing/disallowed-track gates; it discards history.
   Identity/sequence/read failures require an explicit quiescent rebind.
   Begin reentry poisons the enclosing call; the original still must execute. */
int mxb_frame_snapshot_begin(MxbFrameSnapshotState *, const MxbFrameGate *, int elapsed_ms);
/* Only after successful begin, before original scheduler/physics, for the
   session's delayed-publication gate. Returns0 if no healthy call is armed. */
uint64_t mxb_frame_snapshot_upcoming_sequence(const MxbFrameSnapshotState *);
/* After each selected-bike wheel-force call, after its sample was successfully
   fed to mxdirt_contact_physics. Native wheel order is front0 then rear1.
   Sequence is per wheel, strictly increasing; numeric equality between wheels
   is not required. Rejected/omitted physics-filter samples cannot be hidden.
   Body address is the pointer from that same captured force sample. */
int mxb_frame_snapshot_note(MxbFrameSnapshotState *, const MxbFrameGate *, int wheel,
    uint64_t body_address, const MxdirtPhysicsSample *accepted_physics_sample);
/* Immediately AFTER the original scheduler returns. OK/NO_NEW_STEP both
   publish output; all other results leave output unchanged. NO_NEW_STEP may
   still drive an outer controller update using the last matching physics
   sequence. WAITING means no complete pair has ever been captured.
   No interpolation, wall-clock substitute, force refilter, or native write. */
int mxb_frame_snapshot_finish(MxbFrameSnapshotState *, const MxbFrameGate *, MxbFrameSnapshot *);
/* Copy only center/center-velocity/spin/dt/generation/frame/physics sequences.
   Preserve the caller's explicit track, material and qualification settings. */
int mxb_frame_snapshot_controller_values(const MxbFrameSnapshot *, int wheel,
    MxdirtControllerFrame *);
#endif
