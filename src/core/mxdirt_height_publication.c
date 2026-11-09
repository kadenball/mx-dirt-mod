#include "mxdirt_height_publication.h"
#include "mxdirt_height_bridge.h"
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

typedef struct { uint64_t serial;uint32_t tile;int used; } NativeEvent;
typedef struct {size_t start,end;} CellInterval;
struct MxbHeightPublication {
 unsigned char *shadow_maybe_nonzero,*echo_maybe_nonzero;
 uint32_t w,h;size_t cells,tiles,target_cap,event_cap,qhead,qcount,event_count;
 uint64_t generation,last_sequence,issued_sequence,next_event,reservation;
 int active,faulted,fault_line;
 int16_t *base;int32_t *issued,*sent,*echo,*shadow,*latest,*targets,*event_data;
 uint64_t *sequences;NativeEvent *events;MxbDelta *scratch;int64_t *remainder;
 size_t sparse_cap,scratch_cap,latest_count,*counts,interval_count,allocated_bytes;
 MxbPubAbsoluteCell *sparse_targets,*latest_sparse;
 CellInterval *intervals;uint32_t *tile_scratch;uint64_t *matches;
 int adopt_foreign;uint64_t foreign_tiles;
 uint32_t adopted_ring[256];uint64_t adopted_n; /* v8.8: adopted tiles, so the session can resend them */
};
/* Exact equality, not a hash or sampled check. Unaligned loads and scalar tail
   stay within the requested span. SSE2 is baseline on the supported x64 host. */
static int bytes_equal(const void *aa,const void *bb,size_t n){
 const unsigned char *a=aa,*b=bb;size_t i=0;
#if defined(__SSE2__) || defined(_M_X64)
 __m128i zero=_mm_setzero_si128();
 for(;n-i>=128;i+=128){
  __m128i d=zero;
  for(size_t j=0;j<128;j+=16)d=_mm_or_si128(d,_mm_xor_si128(_mm_loadu_si128((const __m128i*)(a+i+j)),_mm_loadu_si128((const __m128i*)(b+i+j))));
  if(_mm_movemask_epi8(_mm_cmpeq_epi8(d,zero))!=65535)return 0;
 }
#endif
 for(;i<n;i++)if(a[i]!=b[i])return 0;return 1;
}
/* Metadata describes only this module's private buffers, never native memory.
   A zero flag proves its 64-cell block is zero; a set flag makes no promise.
   Flags only change 0->1 between seeds, so clearing a value cannot invalidate
   the proof. Every native byte is still compared at the original boundaries. */
#define ZERO_BLOCK_CELLS 64u
static int bytes_zero(const void *aa,size_t n){
 const unsigned char *a=aa;size_t i=0;
#if defined(__SSE2__) || defined(_M_X64)
 __m128i zero=_mm_setzero_si128();
 for(;n-i>=128;i+=128){
  __m128i d=zero;
  for(size_t j=0;j<128;j+=16)d=_mm_or_si128(d,_mm_loadu_si128((const __m128i*)(a+i+j)));
  if(_mm_movemask_epi8(_mm_cmpeq_epi8(d,zero))!=65535)return 0;
 }
#endif
 for(;i<n;i++)if(a[i])return 0;return 1;
}
static int private_range_exact(const MxbHeightPublication *p,const int32_t *a,const int32_t *b,size_t first,size_t end){
 const unsigned char *flags=b==p->shadow?p->shadow_maybe_nonzero:b==p->echo?p->echo_maybe_nonzero:NULL;
 if(!flags)return bytes_equal(a+first,b+first,(end-first)*4);
 while(first<end){
  size_t block=first/ZERO_BLOCK_CELLS,stop=(block+1)*ZERO_BLOCK_CELLS;if(stop>end)stop=end;
  /* Merge equal metadata states; no native cell is skipped or sampled. */
  while(stop<end&&flags[stop/ZERO_BLOCK_CELLS]==flags[block]){size_t next=stop+ZERO_BLOCK_CELLS;stop=next<end?next:end;}
  if(flags[block]?!bytes_equal(a+first,b+first,(stop-first)*4):!bytes_zero(a+first,(stop-first)*4))return 0;
  first=stop;
 }
 return 1;
}
static int mul_ok(size_t a,size_t b,size_t *v){if(b&&a>SIZE_MAX/b)return 0;*v=a*b;return 1;}
static int view_shape(const MxbHeightPublication *p,const MxbPendingView *v,uint64_t generation){
 return v&&v->width==p->w&&v->height==p->h&&v->generation==generation&&
  v->pending&&v->dirty&&v->pending_count>=p->cells&&v->dirty_count>=p->tiles;
}
static MxbPubResult fault_at(MxbHeightPublication *p,int line){if(!p->faulted&&!p->fault_line)p->fault_line=line;p->faulted=1;return MXB_PUB_FAULT;}
#define fault(p) fault_at((p),__LINE__)
static MxbPubResult ready(MxbHeightPublication *p){
 if(!p)return MXB_PUB_INVALID;if(p->faulted)return MXB_PUB_FAULT;if(!p->active)return MXB_PUB_INACTIVE;return MXB_PUB_OK;
}
static int bounds_ok(int16_t b,int32_t x){int32_t lo,hi;mxb_height_fixed_bounds(b,&lo,&hi);return x>=lo&&x<=hi;}
static int initial_ok(const MxbHeightPublication *p,const int16_t *base,const int32_t *initial,
 size_t cells,const MxbPendingView *v,uint64_t gen){
 if(!base||!initial||cells!=p->cells||!gen||!view_shape(p,v,gen))return 0;
 for(size_t i=0;i<cells;i++)if(v->pending[i]!=0||!bounds_ok(base[i],initial[i]))return 0;
 return 1;
}
static void seed(MxbHeightPublication *p,uint64_t generation,const int16_t *base,const int32_t *initial){
 memset(p->shadow_maybe_nonzero,0,(p->cells+ZERO_BLOCK_CELLS-1)/ZERO_BLOCK_CELLS);
 memset(p->echo_maybe_nonzero,0,(p->cells+ZERO_BLOCK_CELLS-1)/ZERO_BLOCK_CELLS);
 for(size_t k=0;k<p->cells;k++)if(initial[k])p->echo_maybe_nonzero[k/ZERO_BLOCK_CELLS]=1;
 memcpy(p->base,base,p->cells*2);memcpy(p->issued,initial,p->cells*4);memcpy(p->sent,initial,p->cells*4);
 memcpy(p->echo,initial,p->cells*4);memset(p->shadow,0,p->cells*4);if(p->latest)memcpy(p->latest,initial,p->cells*4);
 memset(p->events,0,p->event_cap*sizeof(*p->events));p->generation=generation;
 p->last_sequence=0;p->issued_sequence=0;p->next_event=1;p->reservation=0;p->qhead=0;p->qcount=0;
 p->event_count=0;p->active=0;p->faulted=0;p->latest_count=0;p->interval_count=0;
}
static void *allocate(MxbHeightPublication *p,size_t count,size_t unit){
 size_t bytes;if(!mul_ok(count,unit,&bytes)||bytes>SIZE_MAX-p->allocated_bytes)return NULL;
 void *a=calloc(1,bytes);if(a)p->allocated_bytes+=bytes;return a;
}
static MxbHeightPublication *create(uint32_t w,uint32_t h,uint64_t generation,
 const int16_t *base,const int32_t *initial,size_t cells,const MxbPendingView *pending,
 size_t target_cap,size_t event_cap,size_t sparse_cap){
 size_t n,t,qn,en;
 if(!mxb_dimensions(w,h,&n,&t)||n!=cells||!generation||!target_cap||!event_cap||
    target_cap>1024||event_cap>65536||sparse_cap>cells||
    !mul_ok(sparse_cap?sparse_cap:n,target_cap,&qn)||!mul_ok(event_cap,4096,&en))return NULL;
 MxbHeightPublication *p=calloc(1,sizeof(*p));if(!p)return NULL;
 p->w=w;p->h=h;p->cells=n;p->tiles=t;p->target_cap=target_cap;p->event_cap=event_cap;
 p->sparse_cap=sparse_cap;p->scratch_cap=sparse_cap?sparse_cap:n;p->allocated_bytes=sizeof(*p);
 if(!initial_ok(p,base,initial,n,pending,generation)){free(p);return NULL;}
 p->shadow_maybe_nonzero=allocate(p,(n+ZERO_BLOCK_CELLS-1)/ZERO_BLOCK_CELLS,1);
 p->echo_maybe_nonzero=allocate(p,(n+ZERO_BLOCK_CELLS-1)/ZERO_BLOCK_CELLS,1);
 p->base=allocate(p,n,2);p->issued=allocate(p,n,4);p->sent=allocate(p,n,4);p->echo=allocate(p,n,4);p->shadow=allocate(p,n,4);
 if(sparse_cap){p->sparse_targets=allocate(p,qn,sizeof(*p->sparse_targets));p->latest_sparse=allocate(p,sparse_cap,sizeof(*p->latest_sparse));}
 else{p->latest=allocate(p,n,4);p->targets=allocate(p,qn,4);}
 p->event_data=allocate(p,en,4);p->sequences=allocate(p,target_cap,sizeof(*p->sequences));p->counts=allocate(p,target_cap,sizeof(*p->counts));
 p->events=allocate(p,event_cap,sizeof(*p->events));p->scratch=allocate(p,p->scratch_cap,sizeof(*p->scratch));p->remainder=allocate(p,p->scratch_cap,sizeof(*p->remainder));
 p->intervals=allocate(p,event_cap*64,sizeof(*p->intervals));p->tile_scratch=allocate(p,event_cap,sizeof(*p->tile_scratch));p->matches=allocate(p,event_cap,sizeof(*p->matches));
 if(!p->shadow_maybe_nonzero||!p->echo_maybe_nonzero||!p->base||!p->issued||!p->sent||!p->echo||!p->shadow||
    (sparse_cap?(!p->sparse_targets||!p->latest_sparse):(!p->latest||!p->targets))||!p->event_data||
    !p->sequences||!p->counts||!p->events||!p->scratch||!p->remainder||!p->intervals||!p->tile_scratch||!p->matches){mxb_pub_destroy(p);return NULL;}
 seed(p,generation,base,initial);return p;
}
MxbHeightPublication *mxb_pub_create(uint32_t w,uint32_t h,uint64_t generation,const int16_t *base,const int32_t *initial,size_t cells,
 const MxbPendingView *pending,size_t target_cap,size_t event_cap){return create(w,h,generation,base,initial,cells,pending,target_cap,event_cap,0);}
MxbHeightPublication *mxb_pub_create_sparse(uint32_t w,uint32_t h,uint64_t generation,const int16_t *base,const int32_t *initial,size_t cells,
 const MxbPendingView *pending,size_t target_cap,size_t event_cap,size_t max_changed){
 if(!max_changed)return NULL;return create(w,h,generation,base,initial,cells,pending,target_cap,event_cap,max_changed);
}
void mxb_pub_destroy(MxbHeightPublication *p){
 if(!p)return;free(p->shadow_maybe_nonzero);free(p->echo_maybe_nonzero);
 free(p->base);free(p->issued);free(p->sent);free(p->echo);free(p->shadow);free(p->latest);
 free(p->targets);free(p->event_data);free(p->sequences);free(p->events);free(p->scratch);free(p->remainder);
 free(p->counts);free(p->sparse_targets);free(p->latest_sparse);free(p->intervals);free(p->tile_scratch);free(p->matches);free(p);
}
int mxb_pub_matches_initial(const MxbHeightPublication *p,const int16_t *base,const int32_t *initial,size_t cells){
 return p&&base&&initial&&cells==p->cells&&!p->active&&!p->faulted&&!p->last_sequence&&
  !p->qcount&&!p->event_count&&!p->reservation&&bytes_equal(base,p->base,cells*2)&&
  bytes_equal(initial,p->echo,cells*4);
}
MxbPubResult mxb_pub_activate(MxbHeightPublication *p,const MxbPendingView *v,const int32_t *native,size_t cells,int exclusive){
 if(!p)return MXB_PUB_INVALID;if(p->faulted)return MXB_PUB_FAULT;
 if(!native||cells!=p->cells||!view_shape(p,v,p->generation))return MXB_PUB_INVALID;
 if(!exclusive)return p->active?fault(p):MXB_PUB_INVALID;
 if(!bytes_equal(v->pending,p->shadow,cells*4)||!bytes_equal(native,p->echo,cells*4))return fault(p);
 p->active=1;return MXB_PUB_OK;
}
MxbPubResult mxb_pub_enqueue(MxbHeightPublication *p,uint64_t generation,uint64_t sequence,
 const int32_t *absolute,size_t cells){
 MxbPubResult r=ready(p);if(r!=MXB_PUB_OK)return r;
 if(generation!=p->generation)return generation<p->generation?MXB_PUB_STALE:MXB_PUB_INVALID;
 if(!absolute||cells!=p->cells||!sequence||p->sparse_cap)return MXB_PUB_INVALID;
 for(size_t i=0;i<cells;i++)if(!bounds_ok(p->base[i],absolute[i]))return MXB_PUB_INVALID;
 if(sequence==p->last_sequence)return !bytes_equal(absolute,p->latest,cells*4)?fault(p):MXB_PUB_DUPLICATE;
 if(sequence<p->last_sequence)return MXB_PUB_STALE;
 if(p->last_sequence==UINT64_MAX||sequence!=p->last_sequence+1)return MXB_PUB_OUT_OF_ORDER;
 if(p->qcount==p->target_cap)return MXB_PUB_QUEUE_FULL;
 size_t slot=(p->qhead+p->qcount)%p->target_cap;
 memcpy(p->targets+slot*cells,absolute,cells*4);memcpy(p->latest,absolute,cells*4);
 p->sequences[slot]=sequence;p->qcount++;p->last_sequence=sequence;return MXB_PUB_OK;
}
MxbPubResult mxb_pub_enqueue_sparse(MxbHeightPublication *p,uint64_t generation,uint64_t previous,uint64_t sequence,
 const MxbPubAbsoluteCell *a,size_t count){
 MxbPubResult r=ready(p);if(r!=MXB_PUB_OK)return r;
 if(generation!=p->generation)return generation<p->generation?MXB_PUB_STALE:MXB_PUB_INVALID;
 if(!p->sparse_cap||count>p->sparse_cap||(count&&!a)||!sequence||previous!=sequence-1)return MXB_PUB_INVALID;
 for(size_t i=0;i<count;i++)if(a[i].index>=p->cells||(i&&a[i-1].index>=a[i].index)||!bounds_ok(p->base[a[i].index],a[i].absolute))return MXB_PUB_INVALID;
 if(sequence==p->last_sequence)return count==p->latest_count&&bytes_equal(a,p->latest_sparse,count*sizeof(*a))?MXB_PUB_DUPLICATE:fault(p);
 if(sequence<p->last_sequence)return MXB_PUB_STALE;
 if(p->last_sequence==UINT64_MAX||previous!=p->last_sequence)return MXB_PUB_OUT_OF_ORDER;
 if(p->qcount==p->target_cap)return MXB_PUB_QUEUE_FULL;
 size_t slot=(p->qhead+p->qcount)%p->target_cap;
 if(count){memcpy(p->sparse_targets+slot*p->sparse_cap,a,count*sizeof(*a));memcpy(p->latest_sparse,a,count*sizeof(*a));}
 p->counts[slot]=count;p->sequences[slot]=sequence;p->latest_count=count;p->qcount++;p->last_sequence=sequence;return MXB_PUB_OK;
}
static int pending_exact(const MxbHeightPublication *p,const MxbPendingView *v){
 return view_shape(p,v,p->generation)&&private_range_exact(p,v->pending,p->shadow,0,p->cells);
}
MxbPubResult mxb_pub_issue(MxbHeightPublication *p,MxbPendingView *v,int exclusive,MxbPubIssueReceipt *receipt){
 MxbPubResult r=ready(p);if(r!=MXB_PUB_OK)return r;
 if(!receipt||!view_shape(p,v,p->generation))return MXB_PUB_INVALID;
 if(!exclusive)return fault(p);if(p->reservation)return MXB_PUB_BUSY;
 if(!pending_exact(p,v))return fault(p);
 MxbPubIssueReceipt out;memset(&out,0,sizeof(out));out.generation=p->generation;out.issued_sequence=p->issued_sequence;
 if(!p->qcount){*receipt=out;return MXB_PUB_NO_CHANGE;}
 const int32_t *target=p->sparse_cap?NULL:p->targets+p->qhead*p->cells;size_t count=0;
 const MxbPubAbsoluteCell *sparse=p->sparse_cap?p->sparse_targets+p->qhead*p->sparse_cap:NULL;
 out.target_sequence=p->sequences[p->qhead];
 size_t scan=p->sparse_cap?p->counts[p->qhead]:p->cells;
 for(size_t i=0;i<scan;i++){
  uint32_t k=p->sparse_cap?sparse[i].index:(uint32_t)i;int32_t desired=p->sparse_cap?sparse[i].absolute:target[k];
  if(desired!=p->issued[k]){p->scratch[count].index=k;p->scratch[count].delta=mxb_height_delta_chunk(desired,p->issued[k]);
   count++;}
 }
 MxbDeltaTransaction tx={p->w,p->h,p->generation,out.target_sequence,p->scratch,count};
 /* All data, bounds, indices, spans and immutable queue state validated before
    the helper writes. No allocation or fallible operation follows that write. */
 if(!mxb_commit_delta(&tx,v,p->remainder,p->scratch_cap,&out.pending_write))return fault(p);
 size_t sparse_pos=0;
 for(size_t i=0;i<count;i++){
  size_t k=p->scratch[i].index;int64_t applied=(int64_t)p->scratch[i].delta-p->remainder[i];
  p->issued[k]=(int32_t)((int64_t)p->issued[k]+applied);p->shadow[k]=v->pending[k];
  if(p->shadow[k])p->shadow_maybe_nonzero[k/ZERO_BLOCK_CELLS]=1;
  if(sparse)while(sparse[sparse_pos].index<k)sparse_pos++;
  int32_t desired=sparse?sparse[sparse_pos].absolute:target[k];
  if(desired!=p->issued[k]){
   int64_t d=(int64_t)desired-p->issued[k];out.remaining_cells++;out.remaining_magnitude+=(uint64_t)(d<0?-d:d);
  }
 }
 if(!out.remaining_cells){p->issued_sequence=out.target_sequence;p->qhead=(p->qhead+1)%p->target_cap;p->qcount--;out.issued_sequence=p->issued_sequence;}
 *receipt=out;
 return !out.remaining_cells?MXB_PUB_TARGET_ISSUED:out.pending_write.changed?MXB_PUB_PROGRESS:MXB_PUB_BLOCKED;
}
static void tile_shape(const MxbHeightPublication *p,uint32_t tile,uint32_t *x,uint32_t *z,uint32_t *w,uint32_t *h){
 uint32_t tx=(p->w+63)/64;*x=(tile%tx)*64;*z=(tile/tx)*64;
 *w=p->w-*x<64?p->w-*x:64;*h=p->h-*z<64?p->h-*z:64;
}
static int tile_set_valid(const MxbHeightPublication *p,const uint32_t *tiles,size_t count){
 if(!tiles||!count||count>p->event_cap)return 0;
 for(size_t i=0;i<count;i++)if(tiles[i]>=p->tiles||(i&&tiles[i-1]>=tiles[i]))return 0;return 1;
}
/* Expand a sorted tile set into sorted non-overlapping cell intervals, merging
   adjacency. The complement plus these intervals partitions the ENTIRE grid. */
static void build_intervals(MxbHeightPublication *p,const uint32_t *tiles,size_t count){
 uint32_t tx=(p->w+63)/64;p->interval_count=0;
 for(size_t begin=0;begin<count;){
  size_t end=begin+1;uint32_t rowgroup=tiles[begin]/tx;
  while(end<count&&tiles[end]/tx==rowgroup)end++;
  uint32_t z=rowgroup*64,h=p->h-z<64?p->h-z:64;
  for(uint32_t row=0;row<h;row++)for(size_t j=begin;j<end;j++){
   uint32_t x=tiles[j]%tx*64,w=p->w-x<64?p->w-x:64;
   size_t start=(size_t)(z+row)*p->w+x,stop=start+w;
   if(p->interval_count&&p->intervals[p->interval_count-1].end==start)p->intervals[p->interval_count-1].end=stop;
   else p->intervals[p->interval_count++]=(CellInterval){start,stop};
  }
  begin=end;
 }
}
static int outside_exact(const MxbHeightPublication *p,const int32_t *a,const int32_t *b){
 size_t cursor=0;
 for(size_t i=0;i<p->interval_count;i++){
  size_t next=p->intervals[i].start;
  if(!private_range_exact(p,a,b,cursor,next))return 0;cursor=p->intervals[i].end;
 }
 return private_range_exact(p,a,b,cursor,p->cells);
}
static MxbPubResult prepare_check(MxbHeightPublication *p,const uint32_t *tiles,size_t count,const MxbPendingView *v){
 MxbPubResult r=ready(p);if(r!=MXB_PUB_OK)return r;
 if(!tile_set_valid(p,tiles,count)||!view_shape(p,v,p->generation))return MXB_PUB_INVALID;
 if(p->reservation)return MXB_PUB_BUSY;
 if(count>p->event_cap-p->event_count)return MXB_PUB_QUEUE_FULL;
 if(p->next_event>UINT64_MAX-count)return MXB_PUB_NEEDS_DRAIN;
 return MXB_PUB_OK;
}
/* Infallible after prepare_check and exact pending validation, with no external
   calls/reentrancy between them. Inputs remain immutable by the API contract. */
static void reserve_batch(MxbHeightPublication *p,const uint32_t *tiles,size_t count,uint64_t *token){
 build_intervals(p,tiles,count);p->reservation=p->next_event;size_t slot=0;
 for(size_t j=0;j<count;j++){
  while(p->events[slot].used)slot++;
  uint32_t x,z,w,h;tile_shape(p,tiles[j],&x,&z,&w,&h);
  for(uint32_t row=0;row<h;row++)memcpy(p->event_data+slot*4096+(size_t)row*w,p->issued+(size_t)(z+row)*p->w+x,(size_t)w*4);
  p->events[slot]=(NativeEvent){p->next_event++,tiles[j],1};p->event_count++;slot++;
 }*token=p->reservation;
}
MxbPubResult mxb_pub_prepare_native_clear_batch(MxbHeightPublication *p,const uint32_t *tiles,size_t count,const MxbPendingView *v,uint64_t *token){
 if(!token)return MXB_PUB_INVALID;MxbPubResult r=prepare_check(p,tiles,count,v);if(r!=MXB_PUB_OK)return r;
 if(!pending_exact(p,v))return fault(p);reserve_batch(p,tiles,count,token);return MXB_PUB_OK;
}
MxbPubResult mxb_pub_prepare_native_clear(MxbHeightPublication *p,uint32_t tile,const MxbPendingView *v,uint64_t *token){
 return mxb_pub_prepare_native_clear_batch(p,&tile,1,v,token);
}
MxbPubResult mxb_pub_issue_prepare_batch(MxbHeightPublication *p,MxbPendingView *v,int exclusive,
 const uint32_t *tiles,size_t count,MxbPubIssueReceipt *receipt,uint64_t *token){
 if(!receipt||!token)return MXB_PUB_INVALID;MxbPubResult r=prepare_check(p,tiles,count,v);if(r!=MXB_PUB_OK)return r;
 r=mxb_pub_issue(p,v,exclusive,receipt);
 if(r!=MXB_PUB_PROGRESS&&r!=MXB_PUB_TARGET_ISSUED&&r!=MXB_PUB_BLOCKED&&r!=MXB_PUB_NO_CHANGE)return r;
 /* issue validated every pending cell, then only the owning module wrote its
    known changes. Rechecking the same memory here adds no new observation. */
 reserve_batch(p,tiles,count,token);return r;
}
MxbPubResult mxb_pub_finish_native_clear(MxbHeightPublication *p,uint64_t token,const MxbPendingView *v){
 MxbPubResult r=ready(p);if(r!=MXB_PUB_OK)return r;
 if(!token||token!=p->reservation||!view_shape(p,v,p->generation))return MXB_PUB_INVALID;
 if(!outside_exact(p,v->pending,p->shadow))return fault(p);
 for(size_t i=0;i<p->interval_count;i++)for(size_t k=p->intervals[i].start;k<p->intervals[i].end;k++)if(v->pending[k])return fault(p);
 for(size_t slot=0;slot<p->event_cap;slot++)if(p->events[slot].used==1){
  uint32_t x,z,w,h;tile_shape(p,p->events[slot].tile,&x,&z,&w,&h);
  for(uint32_t row=0;row<h;row++){
   size_t k=(size_t)(z+row)*p->w+x;memset(p->shadow+k,0,(size_t)w*4);memcpy(p->sent+k,p->event_data+slot*4096+(size_t)row*w,(size_t)w*4);
  }p->events[slot].used=2;
 }
 p->reservation=0;return MXB_PUB_OK;
}
MxbPubResult mxb_pub_cancel_native_clear(MxbHeightPublication *p,uint64_t token,const MxbPendingView *v){
 MxbPubResult r=ready(p);if(r!=MXB_PUB_OK)return r;
 if(!token||token!=p->reservation)return MXB_PUB_INVALID;if(!pending_exact(p,v))return fault(p);
 for(size_t slot=0;slot<p->event_cap;slot++)if(p->events[slot].used==1){p->events[slot].used=0;p->event_count--;}
 p->reservation=0;return MXB_PUB_OK;
}
static int tile_matches(const MxbHeightPublication *p,uint32_t tile,const int32_t *native,const int32_t *other,int packed){
 uint32_t x,z,w,h;tile_shape(p,tile,&x,&z,&w,&h);
 for(uint32_t row=0;row<h;row++){
  size_t k=(size_t)(z+row)*p->w+x;
  if(!bytes_equal(native+k,other+(packed?(size_t)row*w:k),(size_t)w*4))return 0;
 }return 1;
}
/* Tolerant mode: a tile whose native heights match neither our echo nor any
   sent checkpoint was written by another native terrain writer. Adopt it for
   that tile only (issued = native + still-pending shadow, sent = echo =
   native, its in-flight events superseded) instead of faulting the ledger. */
static void adopt_tile(MxbHeightPublication *p,uint32_t tile,const int32_t *native){
 uint32_t x,z,w,h;tile_shape(p,tile,&x,&z,&w,&h);
 for(uint32_t row=0;row<h;row++){
  size_t k=(size_t)(z+row)*p->w+x;
  for(uint32_t c=0;c<w;c++){int64_t s=(int64_t)native[k+c]+p->shadow[k+c];p->issued[k+c]=(int32_t)(s<INT32_MIN?INT32_MIN:s>INT32_MAX?INT32_MAX:s);}
  memcpy(p->sent+k,native+k,(size_t)w*4);memcpy(p->echo+k,native+k,(size_t)w*4);
  for(size_t j=k/ZERO_BLOCK_CELLS;j<=(k+w-1)/ZERO_BLOCK_CELLS;j++)p->echo_maybe_nonzero[j]=1;
 }
 for(size_t slot=0;slot<p->event_cap;slot++)if(p->events[slot].used==2&&p->events[slot].tile==tile){p->events[slot].used=0;p->event_count--;}
 p->foreign_tiles++;p->adopted_ring[p->adopted_n%256]=tile;p->adopted_n++;
}
/* v8.8: rects (x,z,w,h) of tiles adopted since *cursor; returns the count, or SIZE_MAX when
   more were adopted than the ring holds (caller resends everything). Advances *cursor. */
size_t mxb_pub_adopted_rects(const MxbHeightPublication *p,uint64_t *cursor,uint32_t *rects,size_t max){
 if(!p||!cursor)return 0;uint64_t n=p->adopted_n;if(*cursor>n)*cursor=n;
 if(n-*cursor>256||n-*cursor>max){*cursor=n;return (size_t)-1;}
 size_t k=0;for(uint64_t i=*cursor;i<n;i++,k++){uint32_t t=p->adopted_ring[i%256];tile_shape(p,t,rects+4*k,rects+4*k+1,rects+4*k+2,rects+4*k+3);}
 *cursor=n;return k;
}
void mxb_pub_set_adopt_foreign(MxbHeightPublication *p,int enabled){if(p)p->adopt_foreign=enabled!=0;}
uint64_t mxb_pub_foreign_tiles(const MxbHeightPublication *p){return p?p->foreign_tiles:0;}
MxbPubResult mxb_pub_observe_native_echo_batch(MxbHeightPublication *p,const uint32_t *tiles,size_t count,
 const int32_t *native,size_t cells,MxbPubBatchEchoReceipt *receipt){
 MxbPubResult r=ready(p);if(r!=MXB_PUB_OK)return r;
 if(!native||!receipt||cells!=p->cells||!tile_set_valid(p,tiles,count))return MXB_PUB_INVALID;
 if(p->reservation)return MXB_PUB_BUSY;
 build_intervals(p,tiles,count);if(!outside_exact(p,native,p->echo))return fault(p);
 for(size_t j=0;j<count;j++){
  uint64_t serial=UINT64_MAX;
  for(size_t slot=0;slot<p->event_cap;slot++)if(p->events[slot].used==2&&p->events[slot].tile==tiles[j]&&p->events[slot].serial<serial&&
     tile_matches(p,tiles[j],native,p->event_data+slot*4096,1))serial=p->events[slot].serial;
  if(serial==UINT64_MAX&&!tile_matches(p,tiles[j],native,p->echo,0)){
   if(!p->adopt_foreign)return fault(p);
   adopt_tile(p,tiles[j],native);
  }
  p->matches[j]=serial; /* Every matched checkpoint was signed-bound validated. */
 }
 MxbPubBatchEchoReceipt out={count,0,0,0};
 for(size_t j=0;j<count;j++)if(p->matches[j]!=UINT64_MAX){
  uint64_t serial=p->matches[j];uint32_t x,z,w,h;tile_shape(p,tiles[j],&x,&z,&w,&h);
  for(uint32_t row=0;row<h;row++){
   size_t k=(size_t)(z+row)*p->w+x;memcpy(p->echo+k,native+k,(size_t)w*4);
   for(size_t j=k/ZERO_BLOCK_CELLS;j<=(k+w-1)/ZERO_BLOCK_CELLS;j++)p->echo_maybe_nonzero[j]=1;
  }
  out.matched_tiles++;if(serial>out.last_event_serial)out.last_event_serial=serial;
  for(size_t slot=0;slot<p->event_cap;slot++)if(p->events[slot].used==2&&p->events[slot].tile==tiles[j]&&p->events[slot].serial<=serial){
   p->events[slot].used=0;p->event_count--;out.coalesced_events++;
  }
 }
 *receipt=out;return out.matched_tiles?MXB_PUB_OK:MXB_PUB_NO_CHANGE;
}
MxbPubResult mxb_pub_observe_native_echo(MxbHeightPublication *p,uint32_t tile,const int32_t *native,size_t cells,MxbPubEchoReceipt *receipt){
 if(!receipt)return MXB_PUB_INVALID;MxbPubBatchEchoReceipt out;
 MxbPubResult r=mxb_pub_observe_native_echo_batch(p,&tile,1,native,cells,&out);
 if(r==MXB_PUB_OK||r==MXB_PUB_NO_CHANGE)*receipt=(MxbPubEchoReceipt){out.last_event_serial,tile,out.coalesced_events};return r;
}
MxbPubResult mxb_pub_state(const MxbHeightPublication *p,MxbPubState *s){
 if(!p||!s)return MXB_PUB_INVALID;
 *s=(MxbPubState){p->generation,p->last_sequence,p->issued_sequence,p->qcount,p->event_count,
  p->active,p->faulted,p->reservation!=0,p->issued,p->sent,p->echo,p->shadow,p->cells,p->allocated_bytes,p->fault_line};return MXB_PUB_OK;
}
/* Self-heal after a fault: adopt the CURRENT native published grid and native
   pending buffer as the truth. Invariant restored: issued = native + pending
   (int32-saturating like the native merge), sent = echo = native, shadow =
   pending. Outstanding reservation/events are dropped (their effects, if any,
   are already in native memory). Queued targets and sequence continuity are
   kept, so the next issue drives every queued cell to its target again. */
MxbPubResult mxb_pub_resync(MxbHeightPublication *p,const MxbPendingView *v,const int32_t *native,size_t cells){
 if(!p||!native||cells!=p->cells||!view_shape(p,v,p->generation))return MXB_PUB_INVALID;
 if(!p->active)return MXB_PUB_INACTIVE;
 for(size_t k=0;k<cells;k++){int64_t s=(int64_t)native[k]+v->pending[k];p->issued[k]=(int32_t)(s<INT32_MIN?INT32_MIN:s>INT32_MAX?INT32_MAX:s);}
 memcpy(p->sent,native,cells*4);memcpy(p->echo,native,cells*4);memcpy(p->shadow,v->pending,cells*4);
 /* A set flag makes no promise, so marking every block keeps checks exact. */
 memset(p->shadow_maybe_nonzero,1,(p->cells+ZERO_BLOCK_CELLS-1)/ZERO_BLOCK_CELLS);
 memset(p->echo_maybe_nonzero,1,(p->cells+ZERO_BLOCK_CELLS-1)/ZERO_BLOCK_CELLS);
 memset(p->events,0,p->event_cap*sizeof(*p->events));p->event_count=0;p->reservation=0;p->interval_count=0;
 p->faulted=0;p->fault_line=0;return MXB_PUB_OK;
}
static int internally_drained(const MxbHeightPublication *p){
 if(p->qcount||p->reservation)return 0;
 for(size_t i=0;i<p->cells;i++)if(p->shadow[i]||p->issued[i]!=p->sent[i]||p->issued[i]!=p->echo[i])return 0;
 return 1;
}
MxbPubResult mxb_pub_deactivate_drained(MxbHeightPublication *p,const MxbPendingView *v,const int32_t *native,size_t cells,int quiescent){
 if(!p||!native||cells!=p->cells||!view_shape(p,v,p->generation))return MXB_PUB_INVALID;
 if(p->faulted)return MXB_PUB_FAULT;if(!pending_exact(p,v)||!bytes_equal(native,p->echo,cells*4))return fault(p);
 if(!quiescent||!internally_drained(p))return MXB_PUB_NEEDS_DRAIN;
 memset(p->events,0,p->event_cap*sizeof(*p->events));p->event_count=0;p->active=0;return MXB_PUB_OK;
}
MxbPubResult mxb_pub_reset(MxbHeightPublication *p,uint64_t generation,const int16_t *base,const int32_t *initial,
 size_t cells,const MxbPendingView *v,int quiescent,int retired){
 if(!p||generation<=p->generation||!initial_ok(p,base,initial,cells,v,generation))return MXB_PUB_INVALID;
 if(!quiescent)return MXB_PUB_NEEDS_DRAIN;
 if(!retired&&(p->faulted||p->active||p->event_count||!internally_drained(p)))return MXB_PUB_NEEDS_DRAIN;
 seed(p,generation,base,initial);return MXB_PUB_OK;
}
