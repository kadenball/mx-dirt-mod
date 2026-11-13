#ifndef MXDIRT_DIRT_H
#define MXDIRT_DIRT_H
#include <stddef.h>
#include <stdint.h>
/* Dirt Material, CPU side: a camera-local window of MX Bikes' own height grid.
   Keeps CURRENT (render_grid) and BASE (base_grid) uint16 heights for a fixed
   MXDIRT_CELLS-cell square plus a margin, ahead of the camera, as interleaved
   current/base texels for one GPU texture.
   The window SCROLLS: storage is toroidal (window texel (c,r) lives at storage
   ((c+shift_x)%SIDE,(r+shift_z)%SIDE)), so when the desired centre has drifted
   more than MXDIRT_RECENTER cells only the rows and columns that entered are
   read; everything else stays where it is. CURRENT is compared on every update
   in a box of MXDIRT_NEAR cells around the camera (where a bike can dig) and in
   one of MXDIRT_SLICES row slices of the rest, so a change anywhere else in the
   window shows at most MXDIRT_SLICES updates late (the sim state: MXDIRT_STATE_SLICES rebuilds). What changed is handed on
   as a short list of storage rectangles (MxDirtDirty), kept until the GL side
   has uploaded it. A grid smaller than the window is shown whole (texels
   beyond it repeat its edge; bounds stop at the grid).
   Display only: no physics, terrain, native texture or game memory writes. */
#ifndef MXDIRT_CELLS
#define MXDIRT_CELLS 512
#endif
#ifndef MXDIRT_AHEAD
#define MXDIRT_AHEAD 112
#endif
#ifndef MXDIRT_MARGIN
#define MXDIRT_MARGIN 4
#endif
#define MXDIRT_SIDE (MXDIRT_CELLS+1+2*MXDIRT_MARGIN)
/* RECENTER: on a track with 10 cm cells (supercross) 16 cells recentred the window ~7 times a second. */
enum { MXDIRT_RECENTER=48, MXDIRT_NEAR=64, MXDIRT_SLICES=8, MXDIRT_STATE_SLICES=16, MXDIRT_RECTS=24 };
/* Storage texels that changed and are not uploaded yet. n > MXDIRT_RECTS: everything. The uploader sets n=0. */
typedef struct {uint16_t x,y,w,h;} MxDirtRect;
typedef struct {uint32_t n;MxDirtRect r[MXDIRT_RECTS];} MxDirtDirty;
typedef int (*MxDirtRead)(void *user,uintptr_t address,void *out,size_t bytes);
/* Optional: a pointer through which [address,address+bytes) may be read directly
   until the update returns, or NULL to have it copied through read instead. */
typedef const void *(*MxDirtView)(void *user,uintptr_t address,size_t bytes);
typedef struct {
 float modelview[16];            /* native column-major world->eye */
 uintptr_t current_grid,base_grid;uint32_t width,height;
 float extent_x,extent_z,height_scale,origin[3];
 MxDirtRead read;void *read_user;
 MxDirtView view;                /* may be NULL; same read_user */
} MxDirtInput;
typedef struct {
 int32_t first_x,first_z;        /* grid index of window texel(0,0), margin included (may lie outside a small grid) */
 uint32_t shift_x,shift_z;       /* window texel (c,r) is stored at ((c+shift_x)%SIDE,(r+shift_z)%SIDE) */
 float spacing_x,spacing_z,height_scale;
 float texel0[3];                /* world x/z of window texel(0,0), y = grid origin */
 float bounds[4];                /* world xmin,zmin,xmax,zmax of the window's cells that lie on the grid */
 float camera[3];
 const uint16_t *texels;         /* SIDE*SIDE*2 interleaved current,base, in storage order */
 MxDirtDirty *dirty;             /* texels to upload; the uploader clears it */
 uint32_t changed,recentered;
} MxDirtWindow;
enum { MXDIRT_OK=0,MXDIRT_INPUT,MXDIRT_CAMERA,MXDIRT_READ };
typedef struct {uint32_t stage,read_calls,read_bytes,recentered,changed;} MxDirtStats;
/* The window's reach ahead of the camera shrinks by up to RECENTER cells between recentres. */
typedef struct MxDirt MxDirt;
MxDirt *mxdirt_create(void);
void mxdirt_destroy(MxDirt *);
/* Returns 1 with a complete window, 0 on invalid input/camera or any failed
   read. A failed read forgets the window so the next success re-reads BASE.
   Output texels live until the next update/destroy. */
int mxdirt_update(MxDirt *,const MxDirtInput *,MxDirtWindow *,MxDirtStats *);
/* Deterministic tileable clod/crumb relief, RGBA8 side*side: R,G encoded
   slope, B height, A grain mask. Generated here, no imported textures. */
enum { MXDIRT_CLOD_SIDE=256 };
void mxdirt_clods(unsigned char *rgba);
/* Dirt sim state per height-window texel (same SIDE*SIDE storage as the
   heights): RGBA8 {packed, loose, wetness, roost clods}. Built from the core's
   borrowed sim arrays (rows run with -z, see mxdirt_frame.h). Follows the
   window's scroll (entered rows/columns only); when the sim stepped, the box
   around the camera and one row slice of the rest are rebuilt. */
typedef struct {
 const uint8_t *comp;const float *loose;const int8_t *moist;const uint8_t *clods;
 uint32_t size,vertex_exact;float moisture,compaction0;uint64_t steps;
} MxDirtSim;
typedef struct {int valid,changed;const unsigned char *rgba;MxDirtDirty *dirty;} MxDirtState;
int mxdirt_state(MxDirt *,const MxDirtWindow *,uint32_t grid_width,uint32_t grid_height,const MxDirtSim *,MxDirtState *out);
#endif
