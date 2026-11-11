#ifndef MXDIRT_FRAME_H
#define MXDIRT_FRAME_H
#include <stdint.h>
/* Read-only export from the core plugin (mx_dirt.dlo) to the render plugin
   (mx_dirt_render.dlo), both loaded by MX Bikes in the same process.

   The render plugin finds the core with GetModuleHandleW(L"mx_dirt.dlo") and
   GetProcAddress(module,"MxDirtBorrowFrame"). Both are optional: no core, an
   ABI mismatch or a 0 return simply means "draw nothing this frame".

   Borrow rules: call on the game's render/owner thread (owner_thread), never
   from inside a core callback. Pointers stay valid until the core's next
   owner-thread call (its next simulation step or event). Never write through them.

   Dirt sim texels: sim_size x sim_size, texel (q,r), rows run with -z.
   When sim_vertex_exact, texel (q,r) sits on terrain vertex (q+1, sim_size-1-r);
   otherwise on (q*(N-1)/(sim_size-1)) blended, where N = grid_width.
     sim_comp      compaction: 0 untouched, else 1..255 -> 0..1
     sim_loose     loose soil depth, metres
     sim_moist     moisture offset, 1/255 on sim_moisture
     sim_clods     roost clods (~48 per clod)
     sim_print     tyre prints: bit7 rear, bits0..6 strength
     sim_print_dir travel angle mod pi, 1/256 of pi
     sim_print_off line offset from the texel centre along the travel normal, cell/100
   Terrain identity (lets the renderer check it is drawing over the same terrain):
     grid_width/grid_height  the native height grid (DFM) size, e.g. 2049x2049
     span_x_m/span_z_m       terrain extent in metres
     origin_m[3]             world position of grid vertex (0,0), metres
   Soft top layer (ABI 2, display only; soft_tile NULL = layer off). Texel
   (a,b) covers x in [a,a+1)*soft_cell_m, z in [b,b+1)*soft_cell_m from grid
   vertex (0,0), b with +z. soft_tile[tz*soft_tiles_x+tx] -> soft_tile_side^2
   texels, rows with +z, 2 bytes each {128 + height/soft_unit_um relative to the
   untouched layer top, bit7 crisp tread | bits 6..5 freshness (3 just made .. 0
   settled) | bits 4..0 travel angle mod pi in 1/32 of pi} (ABI 3);
   a NULL tile is untouched {128,0}. soft_tile_serial[t] changes when tile t does. */
enum { MXDIRT_FRAME_ABI = 3 };
typedef struct {
    uint32_t abi, bytes, owner_thread, reserved;
    uint64_t generation;          /* changes on every track load */
    uint32_t grid_width, grid_height;
    float span_x_m, span_z_m, origin_m[3];
    uint32_t sim_size, sim_vertex_exact;
    float sim_moisture, sim_compaction0;
    uint64_t sim_steps;
    const uint8_t *sim_comp; const float *sim_loose; const int8_t *sim_moist; const uint8_t *sim_clods;
    const uint8_t *sim_print, *sim_print_dir; const int8_t *sim_print_off;
    float soft_cell_m, soft_thickness_m;
    uint32_t soft_tiles_x, soft_tiles_z, soft_tile_side, soft_unit_um; /* height step, micrometres (1000) */
    const uint8_t *const *soft_tile; const uint32_t *soft_tile_serial;
} MxDirtFrame;
/* Returns 1 and fills *out, or 0 (core not live / wrong thread / abi or bytes mismatch). */
typedef int (*MxDirtBorrowFrameFn)(uint32_t abi, uint32_t bytes, MxDirtFrame *out);
#endif
