#ifndef MXDIRT_TERRAIN_SCOPE_H
#define MXDIRT_TERRAIN_SCOPE_H
/* Passive x64 adapter for MX Bikes' terrain draw (one exact mxbikes.exe, see
   SUPPORTED_EXE.txt). Reads the native terrain descriptor passed to the outer
   terrain draw call and plans the 5-byte CALL patch for that call site. No GL
   calls, allocation, image writes, terrain writes or hook installation here. */
#include <stddef.h>
#include <stdint.h>

#define MXDIRT_TS_SCOPES 32u
#define MXDIRT_TS_DEPTH 8u
#define MXDIRT_TS_NONE UINT32_MAX

/* Must copy exactly n bytes or fail; never expose an unchecked target pointer.
   Must not reenter the collector. */
typedef int (*MxDirtTsRead)(void *user, uintptr_t source, void *dest, size_t n);
typedef struct {
    MxDirtTsRead read;
    void *read_user;
    uint64_t generation;             /* event identity bound by the host */
    uint32_t owner_thread;           /* actual render thread, supplied by host */
} MxDirtTsConfig;

/* Native terrain descriptor ("h") as read at the outer terrain call. */
typedef struct {
    uintptr_t h, model, view_context, base_grid, render_grid;
    uint32_t width, height, flags;
    float extent_x, extent_z, height_scale;
    float origin[3];                 /* world position of grid vertex (0,0) */
    float view[16];
} MxDirtTsTerrain;

typedef struct { uint64_t generation, serial; uint32_t depth; } MxDirtTsToken;
typedef struct { MxDirtTsToken token; uint32_t scope; } MxDirtTsFrame;
typedef struct { uint64_t rejected_entries, dropped_scopes, unbalanced, overflow; } MxDirtTsStats;
typedef struct {
    MxDirtTsConfig config;
    uint64_t serial;
    uint32_t depth, poisoned, scope_count;
    MxDirtTsStats stats;
    MxDirtTsFrame stack[MXDIRT_TS_DEPTH];
    MxDirtTsTerrain scopes[MXDIRT_TS_SCOPES];
} MxDirtTerrainScope;

/* init: fresh storage, under quiescence. */
int mxdirt_ts_init(MxDirtTerrainScope *, const MxDirtTsConfig *);
/* Discards a fully unwound prior capture, keeping event identity and the
   monotonically increasing token serial. Owner thread, no active depth. */
int mxdirt_ts_next_capture(MxDirtTerrainScope *, uint32_t actual_thread);
/* Call enter even for an invalid target: the returned token blocks inheritance
   from an outer valid scope. Always leave every nonzero token in LIFO order. */
MxDirtTsToken mxdirt_ts_enter_terrain(MxDirtTerrainScope *, uint64_t generation, uint32_t thread,
    uint32_t source_call_rva, uintptr_t h, uintptr_t model, uintptr_t view_context,
    uintptr_t view_matrix, uint32_t flags);
int mxdirt_ts_leave(MxDirtTerrainScope *, uint32_t thread, MxDirtTsToken);

/* Native terrain draw function at the verified CALL site (Microsoft x64 ABI).
   The wrapper calls the original exactly once with every argument unchanged. */
typedef int (*MxDirtTsTerrainFn)(void *, void *, void *, const float *, uint32_t);

/* Byte plan for the complete five-byte CALL at the outer terrain call site,
   never a function-entry detour. executable_identity_verified must mean the
   externally checked exact EXE hash; the surrounding code bytes are checked
   again against the mapped image here. Planning is not installation. */
typedef struct {
    uintptr_t image; size_t image_bytes; int executable_identity_verified;
    MxDirtTsRead read; void *read_user;
} MxDirtTsImage;
typedef struct {
    uintptr_t site, target, resume, wrapper, relay;
    unsigned char original[5], patch[5], relay_bytes[14];
    uint32_t relay_size;
} MxDirtTsCallPlan;
#define MXDIRT_TS_TERRAIN_CALL 0x24f613u
int mxdirt_ts_plan_terrain_call(const MxDirtTsImage *, uintptr_t wrapper, uintptr_t optional_near_relay, MxDirtTsCallPlan *);
#endif
