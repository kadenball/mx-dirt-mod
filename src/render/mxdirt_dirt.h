#ifndef MXDIRT_DIRT_H
#define MXDIRT_DIRT_H
#include <stddef.h>
#include <stdint.h>
/* Dirt Material, CPU side: a camera-local window of MX Bikes' own height grid.
   Reads CURRENT (render_grid) and BASE (base_grid) uint16 heights for a fixed
   256-cell square plus a 2-cell margin, ahead of the camera, and packs them as
   interleaved current/base texels for one small GPU texture. The window keeps
   its position until the desired centre drifts more than MXDIRT_RECENTER
   cells, so BASE rows are re-read only on a recentre. CURRENT rows are read
   every update as ONE contiguous band (SIDE rows at the grid pitch, ~1 MB for
   a 2049-wide grid); nothing reads a whole grid.
   Display only: no physics, terrain, native texture or game memory writes. */
enum { MXDIRT_CELLS=256, MXDIRT_MARGIN=2, MXDIRT_SIDE=MXDIRT_CELLS+1+2*MXDIRT_MARGIN, MXDIRT_RECENTER=16, MXDIRT_AHEAD=48 };
typedef int (*MxDirtRead)(void *user,uintptr_t address,void *out,size_t bytes);
typedef struct {
 float modelview[16];            /* native column-major world->eye */
 uintptr_t current_grid,base_grid;uint32_t width,height;
 float extent_x,extent_z,height_scale,origin[3];
 MxDirtRead read;void *read_user;
} MxDirtInput;
typedef struct {
 int32_t first_x,first_z;        /* grid index of texel(0,0), margin included */
 float spacing_x,spacing_z,height_scale;
 float texel0[3];                /* world x/z of texel(0,0), y = grid origin */
 float bounds[4];                /* world xmin,zmin,xmax,zmax of the 256 cells */
 float camera[3];
 const uint16_t *texels;         /* SIDE*SIDE*2 interleaved current,base */
 uint32_t changed,recentered;
} MxDirtWindow;
enum { MXDIRT_OK=0,MXDIRT_INPUT,MXDIRT_CAMERA,MXDIRT_READ };
typedef struct {uint32_t stage,read_calls,read_bytes,recentered,changed;} MxDirtStats;
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
/* Dirt sim state per height-window texel (same SIDE*SIDE layout as the
   heights): RGBA8 {packed, loose, wetness, roost clods}. Built from the core's
   borrowed sim arrays (rows run with -z, see mxdirt_frame.h); rebuilt only
   when the sim stepped or the window moved. print/print_dir/print_off may be
   NULL (tyre prints off): the prints texture is then neutral. */
typedef struct {
 const uint8_t *comp;const float *loose;const int8_t *moist;const uint8_t *clods;
 uint32_t size,vertex_exact;float moisture,compaction0;uint64_t steps;
 const uint8_t *print,*print_dir;const int8_t *print_off;
} MxDirtSim;
/* prints: RGBA8 {tag (bit7 rear, bits0..6 strength), travel angle mod pi (1/256
   of pi), line offset from THIS window texel centre along the travel normal
   (cell/100, +128), 0}. */
typedef struct {int valid,changed;const unsigned char *rgba,*prints;} MxDirtState;
int mxdirt_state(MxDirt *,const MxDirtWindow *,uint32_t grid_width,uint32_t grid_height,const MxDirtSim *,MxDirtState *out);
#endif
