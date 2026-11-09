#ifndef MXDIRT_DIRT_GL_H
#define MXDIRT_DIRT_GL_H
#include "mxdirt_dirt.h"
#include "mxdirt_camera.h"
typedef struct MxDirtGl MxDirtGl;
typedef struct {float strength,base_loose,fade_start,fade_end;} MxDirtLook;
/* Dirt Material GL owner. GL 4.x compatibility context only, bound to the
   creating context/thread; generation fixed until destroy. Private objects:
   one static 257x257 grid mesh (VBO+IBO+VAO), RG16 window heights, RGBA8
   generated clods with mips, RGBA8 sim state + tyre prints, three sampler
   objects, and an RGBA8 native-colour snapshot reallocated at viewport changes. */
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
} MxDirtSoftInput;
int mxdirt_gl_draw(MxDirtGl *,const MxDirtCameraScope *,const MxDirtWindow *,const MxDirtLook *,const MxDirtState *state,const MxDirtSoftInput *soft);
/* Returns 0 (keeping everything) when not on the owner context/thread or when
   one of our objects is currently bound. */
int mxdirt_gl_destroy(MxDirtGl *);
/* Stage of the last failed create (0 = last create succeeded), with the context's GL major version and profile mask. */
void mxdirt_gl_last_failure(int *stage,int *major,int *profile);
#endif
