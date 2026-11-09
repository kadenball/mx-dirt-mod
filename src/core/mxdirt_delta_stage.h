/* Source-only transaction adapter for the dirt simulation's height output.
   This code owns no game pointers and installs no hooks. Caller must validate
   memory lifetime and commit exclusively at the native pending consumer boundary.
   Positive MXB fixed-point values excavate; negative values deposit. */
#ifndef MXB_DELTA_STAGE_H
#define MXB_DELTA_STAGE_H
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <math.h>

typedef struct { uint32_t index; int32_t delta; } MxbDelta;
typedef struct {
 uint32_t width,height; uint64_t generation,sequence;
 const MxbDelta *cells; size_t count;
} MxbDeltaTransaction;
typedef struct {
 uint32_t width,height; uint64_t generation;
 int32_t *pending; size_t pending_count;
 unsigned char *dirty; size_t dirty_count;
} MxbPendingView;
typedef struct {
 size_t changed,saturated; int64_t requested,applied,remainder;
} MxbDeltaReceipt;

static int mxb_dimensions(uint32_t w,uint32_t h,size_t *cells,size_t *tiles){
 if(w<2||h<2||w>8193||h>8193||(uint64_t)w*h>34000000)return 0;
 *cells=(size_t)w*h;*tiles=(size_t)((w+63)/64)*((h+63)/64);return 1;
}
/* Convert a vertical world-height change, in metres, into one signed MXB
   deformation delta. A rise produces a negative delta. Reject unrepresentable
   output rather than silently wrapping. Caller retains quantization residuals. */
static int mxb_height_delta(double before,double after,double terrain_scale,int32_t *out){
 if(!out||!isfinite(before)||!isfinite(after)||!isfinite(terrain_scale)||terrain_scale<=0)return 0;
 double q=(before-after)*(65535.0*65536.0)/terrain_scale;
 if(!isfinite(q))return 0;
 q=q>=0?floor(q+.5):ceil(q-.5);
 if(q<(double)INT32_MIN||q>(double)INT32_MAX)return 0;
 *out=(int32_t)q;return 1;
}
/* Transaction indices must be strictly ascending, making duplicates invalid.
   Validation is complete before any write. Inputs and output buffers must not
   alias, and must remain immutable/live throughout this synchronous call.
   optional per-cell remainder[] enables exact retry after saturation.
   Sequence deduplication/acknowledgment belongs to the owning queue: never call
   twice for the same generation+sequence without processing its receipt. */
static int mxb_commit_delta(const MxbDeltaTransaction *tx,MxbPendingView *v,
                           int64_t *remainder,size_t remainder_count,MxbDeltaReceipt *receipt){
 size_t cells,tiles;
 if(!tx||!v||!receipt||!mxb_dimensions(v->width,v->height,&cells,&tiles)||
    tx->width!=v->width||tx->height!=v->height||tx->generation!=v->generation||
    !v->pending||!v->dirty||v->pending_count<cells||v->dirty_count<tiles||
    tx->count>cells||(tx->count&&!tx->cells)||(remainder&&remainder_count<tx->count))return 0;
 for(size_t i=0;i<tx->count;i++)
  if(tx->cells[i].index>=cells||(i&&tx->cells[i-1].index>=tx->cells[i].index))return 0;
 MxbDeltaReceipt r={0};size_t tiles_x=(v->width+63)/64;
 for(size_t i=0;i<tx->count;i++){
  uint32_t k=tx->cells[i].index;int64_t old=v->pending[k];
  int64_t requested=tx->cells[i].delta,sum=old+requested;
  int64_t bounded=sum<INT32_MIN?INT32_MIN:sum>INT32_MAX?INT32_MAX:sum;
  int64_t applied=bounded-old,left=requested-applied;
  if(remainder)remainder[i]=left;
  r.requested+=requested;r.applied+=applied;r.remainder+=left;
  if(left)r.saturated++;
  if(applied){
   v->pending[k]=(int32_t)bounded;
   size_t x=k%v->width,z=k/v->width;
   v->dirty[(z/64)*tiles_x+x/64]=1;r.changed++;
  }
 }
 *receipt=r;return 1;
}
#endif
