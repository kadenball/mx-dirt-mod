#ifndef MXDIRT_HEIGHT_FIELD_H
#define MXDIRT_HEIGHT_FIELD_H
#include <stdint.h>
#include <stddef.h>

/* Converts the dirt simulation's square height texture (encoded 0..1) into
   MX Bikes' own deformation grid (DFM, signed 16.16 per terrain vertex). */
typedef struct MxdirtHeightField MxdirtHeightField;
typedef struct {
    uint32_t source_width,source_height,source_stride,destination_width,destination_height;
    float native_span_x,native_span_z,native_height_scale,reference_encoded;
    double native_metres,mxb_vertical_scale;
    uint64_t generation;
} MxdirtHeightConfig;
typedef struct {size_t sampled,retained_edge,clipped,changed_from_initial;} MxdirtHeightStats;

/* Copies the signed physics base and initial deformation into owned memory.
   Both grids are centred and span the same physical X/Z extent. Destination
   grid X/Z increase with world X/Z; source texture rows run with -Z. The
   caller must establish this geometry before construction. Source storage is
   limited to 2^24 samples so every binary32 linear index is exact. This module
   does not attach to the game, infer a track, or publish a frame. */
int mxdirt_height_field_create(MxdirtHeightField **out,const MxdirtHeightConfig *,
    const int16_t *physics_base,const int32_t *initial_dfm,size_t count);
void mxdirt_height_field_destroy(MxdirtHeightField *);
/* Height range response 1..1.75 (before any deformation). Preserves the initial
   DFM and additionally caps targets to the encoded [0,1] range of the default
   response. Rejects an initial DFM outside that range. 1 = no cap. */
int mxdirt_height_field_set_depth_response(MxdirtHeightField *,float response);
float mxdirt_height_field_depth_response(const MxdirtHeightField *);
/* The physics base (track shape, int16, destination grid) this adapter owns. */
const int16_t *mxdirt_height_field_base(const MxdirtHeightField *);
/* Optional vertex-exact publication (no 2x2 blend); before deformation only.
   Destination vertex (x,z) then reads source texel (x-1, 2047-z) on a
   2048-texel/2049-vertex terrain, clamped at the edges. */
int mxdirt_height_field_set_vertex_exact(MxdirtHeightField *,int enabled);
/* Output is an absolute desired grid, not an additive transaction. Compare with
   acknowledged state to create publication deltas. Buffers must not alias.
   Prevalidates the source and stages the complete conversion; invalid inputs or a
   stale generation leave output/stats unchanged. Edge vertices outside the
   lookup keep the initial deformation. Calls are serialized by the owner.
   Dirt-sim heights are published 1:1 (no depth-resistance curve). */
int mxdirt_height_field_convert(MxdirtHeightField *,uint64_t generation,
    const float *source,size_t source_count,int32_t *target,size_t target_count,
    MxdirtHeightStats *stats);
/* Incremental conversion requires one successful full conversion first.
   tiles is a row-major 0/1 map of source 128x32-texel tiles. It MUST cover every
   source change since the last successful conversion. A missed tile cannot be
   detected here. Only sampled texels are validated; failure leaves cached
   output/target/stats unchanged so the same complete tile union can be retried.
   Output is still the complete absolute desired grid. Stats count the
   recomputed subset. Serialized owner only. */
int mxdirt_height_field_convert_changed_tiles(MxdirtHeightField *,uint64_t generation,
    const float *source,size_t source_count,const unsigned char *tiles,size_t tile_count,
    int32_t *target,size_t target_count,MxdirtHeightStats *stats);
/* Conservative destination intervals recomputed by the last SUCCESSFUL call.
   Borrowed arrays contain one [begin,end) interval per row; empty rows have
   begin=UINT32_MAX,end=0. Full conversion reports the whole grid. Failed calls
   preserve these intervals. Valid only until the next successful conversion;
   callers that defer work must union them before then. Serialized owner only. */
int mxdirt_height_field_changed_rows(const MxdirtHeightField *,uint64_t generation,
    const uint32_t **begin,const uint32_t **end,size_t *rows);
#endif
