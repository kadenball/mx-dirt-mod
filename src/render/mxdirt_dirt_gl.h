#ifndef MXDIRT_DIRT_GL_H
#define MXDIRT_DIRT_GL_H
#include "mxdirt_dirt.h"
#include "mxdirt_camera.h"
typedef struct MxDirtGl MxDirtGl;
typedef struct {float strength,base_loose,fade_start,fade_end,relief;} MxDirtLook; /* relief: depth shading 0..2 (0 = off) */
/* Dirt Material GL owner. GL 4.x compatibility context only, bound to the
   creating context/thread; generation fixed until destroy. Private objects:
   one static (MXDIRT_CELLS+1)^2 grid mesh (VBO+IBO+VAO), RG16 window heights, RGBA8
   generated clods with mips, RGBA8 sim state, RG8 soft layer window and its
   64x64 R8 tile occupancy, two programs (colour, tyre sink), four sampler
   objects, and an RGBA8 native-colour snapshot reallocated at viewport changes
   (plus one FBO around that snapshot when the game draws into a multisampled FBO). */
int mxdirt_gl_create(MxDirtGl **,uint64_t generation);
/* Draws the pass over the native ground just drawn with scope's camera.
   Returns 1 issued, 0 transient scope/query/camera/framebuffer rejection, -1
   resource or ownership error. Saves and restores: program, texture/sampler
   bindings of units 0..5 and the active unit, unpack PBO and pixel-store,
   VAO and ARRAY binding, draw-buffer-0 blend/equation/colour mask, depth
   test/func/mask, cull/alpha/stencil/logic-op/polygon-offset/sample-alpha,
   polygon mode, sRGB. Native alpha and depth are never written.
   state may be NULL or invalid: shading then uses the height delta only. */
/* soft: the core's soft top layer (mxdirt_frame.h, ABI 2), or NULL = off. The
   pass keeps a 2048^2 RG8 window of it (51 m, toroidal, ahead of the camera)
   and uploads only tiles that entered the window or changed. */
typedef struct {
 const uint8_t *const *tile;const uint32_t *serial;uint32_t tiles_x,tiles_z,side;
 float cell,thickness,unit,origin_x,origin_z; /* unit: m per height step; origin: world x,z of grid vertex (0,0) */
 int solid,sink; /* solid: the ground texture follows the relief; sink: also write the raised layer's depth (the bike sinks into it) */
} MxDirtSoftInput;
int mxdirt_gl_draw(MxDirtGl *,const MxDirtCameraScope *,const MxDirtWindow *,const MxDirtLook *,const MxDirtState *state,const MxDirtSoftInput *soft);
/* The draw target seen at the last full state check (FBO name, its sample count)
   and why the last frame was not drawn: 0 drawn, 1 camera changed, 2 transform
   feedback / rasterizer discard, 3 an open query, 4 unusable target or viewport,
   5 a multisampled target that could not be resolved. */
void mxdirt_gl_target(const MxDirtGl *,int *draw_fbo,int *samples,int *skip);
/* Measurement only: 1 with the GPU time (ms) of one recent pass when a new sample is there (about one frame in
   30 is measured), else 0; ms NULL only asks for width/height, the viewport of the last full state check. */
int mxdirt_gl_gpu(MxDirtGl *,double *ms,int *width,int *height);
/* Returns 0 (keeping everything) when not on the owner context/thread or when
   one of our objects is currently bound. */
int mxdirt_gl_destroy(MxDirtGl *);
/* Stage of the last failed create (0 = last create succeeded), with the context's GL major version and profile mask. */
void mxdirt_gl_last_failure(int *stage,int *major,int *profile);
#endif
