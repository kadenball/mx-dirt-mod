#ifndef MXDIRT_SESSION_H
#define MXDIRT_SESSION_H
#include "mxdirt_contact.h"
#include "mxdirt_height_field.h"
#include "mxdirt_height_publication.h"
#include "mxb_dirt_ghost.h"
#include "mxdirt_remote.h"

/* One track session of the CPU dirt simulation, owned by one thread:
   wheel contacts -> work history (soil evolution) + bump stress -> cut gain ->
   dirt sim (live wheels, live gate lines,
   ghost riders, pre-wear) -> sim heights -> MX Bikes deformation targets ->
   sparse publication. */
typedef struct MxdirtSession MxdirtSession;
typedef struct {
    MxdirtContactConfig contact;
    MxdirtHeightConfig height;
    size_t sparse_capacity;
} MxdirtSessionConfig;
typedef struct {
    uint64_t generated_sequence,accepted_sequence,accepted_source_sequence,outer_sequence;
    size_t cells,sparse_capacity;
    int priming,faulted;
} MxdirtSessionState;
/* Called synchronously with immutable sorted unique changes. Return 1 only if
   the entire target was copied/accepted, 0 for backpressure, -1 for fatal error.
   Omitted cells inherit the previously ACCEPTED target, not a skipped frame. */
typedef int (*MxdirtAcceptTarget)(void *,uint64_t generation,uint64_t previous_sequence,
    uint64_t sequence,const MxbPubAbsoluteCell *,size_t count);

int mxdirt_session_create(MxdirtSession **,const MxdirtSessionConfig *,
    const int16_t *physics_base,const int32_t *initial_dfm,size_t cells);
void mxdirt_session_destroy(MxdirtSession *);
/* Height range 1..1.75 (before the dirt sim starts). Preserves the saved DFM. */
int mxdirt_session_set_depth_response(MxdirtSession *,float response);
/* Sharp ruts: vertex-exact publication (before start_sim; 2048 texels on a 2049-vertex terrain only). */
int mxdirt_session_set_sharp_ruts(MxdirtSession *,int enabled);
/* Starts the dirt simulation (before the first step). depth_percent 10..300
   scales sinkage/roost (erode), firmness_percent 0..100 untouched compaction. */
int mxdirt_session_start_sim(MxdirtSession *,float depth_percent,float firmness_percent);
/* Soil moisture 0 dry .. 35 ideal .. 100 muddy (after start_sim, before the first step). */
int mxdirt_session_set_dirt_moisture(MxdirtSession *,float percent);
/* Packed-line hardness 25..200 % and crust breakdown 0..300 %. */
int mxdirt_session_set_dirt_response(MxdirtSession *,float hardness_percent,float breakdown_percent);
/* Track drying 0..400 % and loose-soil sweeping 0..300 %. */
int mxdirt_session_set_dirt_weather(MxdirtSession *,float drying_percent,float sweep_percent);
/* Rut shape 0..300 % each: capture into grooves, damp-wall cohesion, roost clods. */
int mxdirt_session_set_dirt_shape(MxdirtSession *,float capture_percent,float cohesion_percent,float clods_percent);
/* Braking bumps 0..200 % (0 = no bump stress, no live gate), before the first step. */
int mxdirt_session_set_bumps(MxdirtSession *,float percent);
/* Rolling bumps 0..100 %: bump stress with no braking or drive. Before the first step. */
int mxdirt_session_set_rolling_bumps(MxdirtSession *,float percent);
/* Extra riders 0..15 replaying each real contact on spread lines (test mode). */
int mxdirt_session_set_extra_riders(MxdirtSession *,unsigned riders);
/* Live wear of each pass shared over 1..8 lines riding with the player (1 = one groove). */
int mxdirt_session_set_live_gate(MxdirtSession *,unsigned lines);
int mxdirt_session_set_live_wear(MxdirtSession *,float percent); /* 0 Low .. 100 Medium .. 200 */
int mxdirt_session_dirt_moisture_now(const MxdirtSession *,float *out);
/* [0] net volume error m^3, [1] cut, [2] compacted, [3] roost, [4] dust, [5] avalanche moves. */
int mxdirt_session_dirt_sim_stats(const MxdirtSession *,double out[6]);
/* Read-only per-texel dirt sim state (owner only, valid until the next session call). */
typedef struct {
    const uint8_t *comp;const float *loose;const int8_t *moist;const uint8_t *clods;
    uint32_t size,vertex_exact;float moisture,compaction0;uint64_t steps;
    const uint8_t *print,*print_dir;const int8_t *print_off;
} MxdirtSimView;
int mxdirt_session_sim_view(const MxdirtSession *,MxdirtSimView *out);
/* Soft top layer (display only, mxdirt_soft_layer.h): thickness_m 0 = off, else
   .005...10. After start_sim, before the first step and before pre-wear. */
int mxdirt_session_set_soft_layer(MxdirtSession *,float thickness_m);
typedef struct {
    const uint8_t *const *tile;const uint32_t *serial;
    uint32_t tiles_x,tiles_z,tile_side,allocated,refused;float cell,thickness;uint64_t stamps;int loaded;
} MxdirtSoftView;
int mxdirt_session_soft_view(const MxdirtSession *,MxdirtSoftView *out);
/* Save / restore the layer (a saved track keeps its marks). load: before any pass. */
int mxdirt_session_soft_save(const MxdirtSession *,const char *path);
int mxdirt_session_soft_load(MxdirtSession *,const char *path);
/* dirt-sim soil state (packing, loose, moisture, clods, prints) saved with the track; wear = mxdirt_session_saved_wear(s,.015) */
int mxdirt_session_soil_save(const MxdirtSession *,const char *path,uint64_t wear);
int mxdirt_session_soil_load(MxdirtSession *,const char *path,uint64_t wear);
/* Ghost riders: recorded laps replayed by the dirt sim, staggered around the lap.
   Laps stay owned by the caller and must outlive the session (or a later count 0). */
#define MXDIRT_SESSION_MAX_GHOSTS 12
int mxdirt_session_set_ghosts(MxdirtSession *,const MxbDirtGhostLap *laps,unsigned count);
uint64_t mxdirt_session_ghost_passes(const MxdirtSession *);
/* The real tyre contacts used by the last completed step (t unset), for recording. */
int mxdirt_session_live_contacts(const MxdirtSession *,MxbDirtGhostSample out[2],int ok[2]);
/* Online riders (EXPERIMENTAL, mxdirt_remote.h): the other bikes the game reports wear the
   track as soil of `material`, once their positions are calibrated against the local bike.
   set: before or between steps. feed: one RaceTrackPosition batch, now_s monotonic seconds. */
int mxdirt_session_set_remote(MxdirtSession *,int on,int material);
int mxdirt_session_remote_feed(MxdirtSession *,const MxdRemotePos *,int count,double now_s);
typedef struct {int calibrated,map,self_num,riders;uint64_t passes,batches,locks;float ref_off,y_off;} MxdirtRemoteState;
int mxdirt_session_remote_state(const MxdirtSession *,double now_s,MxdirtRemoteState *);
/* Pre-wear: `riders` virtual riders ride the recorded laps once, synchronously,
   before the first step (spread/clip in metres). Published with the next step. */
int mxdirt_session_prewear(MxdirtSession *,const MxbDirtGhostLap *laps,unsigned count,unsigned riders,
    float spread,float clip,uint32_t seed,uint64_t *passes);
/* Vertices of the loaded (saved) deformation beyond min_m metres (0 on a fresh track). */
uint64_t mxdirt_session_saved_wear(const MxdirtSession *,double min_m);
/* One wheel-force sample. */
int mxdirt_session_physics(MxdirtSession *,int wheel,const MxdirtPhysicsSample *);
/* One outer frame for both wheels (same sequence/dt). material: MX Bikes surface
   ids as the sim should see them. 1 completed, 0 rejected/fatal. */
int mxdirt_session_step(MxdirtSession *,const MxdirtControllerFrame frames[2],
    const int material[2],MxdirtHeightStats *);
/* Before a LATER outer frame, offer the latest completed height target.
   1 accepted/no work, 0 deferred, -1 invalid/faulted. Unions larger than the
   sparse capacity are sent in capacity-sized pieces over the next frames. */
int mxdirt_session_publish(MxdirtSession *,uint64_t next_outer_sequence,MxdirtAcceptTarget,void *);
/* Resend these destination cells on the next publish (tiles adopted by the transport). */
int mxdirt_session_resend_rect(MxdirtSession *,uint32_t x,uint32_t z,uint32_t w,uint32_t h);
/* After pause/reset: keep terrain, reset wheel inputs; primes on the next frame. */
int mxdirt_session_reprime(MxdirtSession *);
int mxdirt_session_state(const MxdirtSession *,MxdirtSessionState *);
/* Cumulative step timing: [0] contacts/bumps, [1] dirt sim, [2] height conversion. */
int mxdirt_session_step_timing(const MxdirtSession *,uint64_t *frames,double total_ms[3],double max_ms[3]);
const int32_t *mxdirt_session_target(const MxdirtSession *);
#endif
