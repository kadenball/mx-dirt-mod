#include "mxdirt_publication_hooks.h"
#include <stdlib.h>
#include <intrin.h>
#if defined(__clang__) && !__has_builtin(_AddressOfReturnAddress)
#error Build this Win64 boundary source with -fms-extensions so _AddressOfReturnAddress is an intrinsic.
#endif
#pragma intrinsic(_AddressOfReturnAddress)
#define MXB_NATIVE_RETURN_SLOT _AddressOfReturnAddress()

struct MxbPublicationHooks {
 MxbPublicationHookConfig c;
 size_t tiles; unsigned char *before,*zero,*echo_seen,*chosen;
 uint32_t *selected,*echo_tiles; size_t selected_count,echo_count,cursor;
 const int32_t *receive_full;
 uint64_t reservation; int dirty_seen,retired;
 volatile LONG faulted,in_publisher,failure_result;int fail_line;
 MxbPublicationHookState stats;
};
static MxbPublicationHooks *volatile bound;
static const unsigned char publisher_call[5]={0xe8,0x66,0xf6,0xff,0xff};
static const unsigned char receive_setup[28]={0x48,0x8d,0x84,0x24,0xd8,2,0,0,0x8b,8,0x48,0x8b,0x50,8,0x44,0x8b,0x40,0x10,0xe8,0x1e,0x8d,9,0,0xe9,0x8c,0x4f,0,0};
static const unsigned char receive_thunk[38]={0xff,0xc9,0x83,0xf9,2,0x77,0x19,0x48,0x63,0xc9,0x48,0x8d,5,0x67,0xc6,0x3a,0,0x48,0x69,0xc9,0x60,0x61,0,0,0x48,3,0xc8,0xe9,0xf0,0xa0,3,0,0xb8,1,0,0,0,0xc3};
static int receive_signature(const MxbPacingContext *c){
 if(!mxb_pace_context(c)||!mxb_pace_span(c,c->exe+0x1bbe10,38,0)||
    memcmp(c->exe+0x1bbe10,receive_thunk,38)||!mxb_pace_span(c,c->exe+0x1f5f20,401,0))return 0;
 uint64_t hash=UINT64_C(0xcbf29ce484222325);
 for(size_t i=0;i<401;i++)hash=(hash^c->exe[0x1f5f20+i])*UINT64_C(0x100000001b3);
 return hash==UINT64_C(0x06934feaf1d087a4);
}
static int publisher_signature(const MxbPacingContext *c,int normalize_selector){
 if(!mxb_pace_context(c)||!mxb_pace_span(c,c->exe+0x2a2e60,1254,0))return 0;
 uint64_t h=UINT64_C(0xcbf29ce484222325);
 for(size_t i=0;i<1254;i++){
  unsigned char b=c->exe[0x2a2e60+i];
  if(normalize_selector&&i>=0xc0&&i<0xc6)b=mxb_pace_sig[0][i-0xc0];
  h=(h^b)*UINT64_C(0x100000001b3);
 }return h==UINT64_C(0x290f9dafec7ae1d0);
}
int mxb_publication_call_plan(const MxbPacingContext *c,uintptr_t replacement,MxbPublicationCallPlan *out){
 if(!out||!replacement||!publisher_signature(c,0)||!mxb_pace_span(c,c->exe+0x2a37f5,5,0)||
    memcmp(c->exe+0x2a37f5,publisher_call,5))return 0;
 uintptr_t next=(uintptr_t)c->exe+0x2a37fa;int64_t displacement;
 if(replacement>=next){if(replacement-next>INT32_MAX)return 0;displacement=(int64_t)(replacement-next);}
 else {if(next-replacement>(uint64_t)INT32_MAX+1)return 0;displacement=-(int64_t)(next-replacement);}
 MxbPublicationCallPlan p={(uintptr_t)c->exe+0x2a37f5,(uintptr_t)c->exe+0x2a2e60,replacement,{0},{0}};
 memcpy(p.original,publisher_call,5);p.patch[0]=0xe8;int32_t d=(int32_t)displacement;memcpy(p.patch+1,&d,4);*out=p;return 1;
}
int mxb_publication_receive_plan(const MxbPacingContext *c,uintptr_t replacement,MxbPublicationCallPlan *out){
 if(!out||!replacement||!receive_signature(c)||!mxb_pace_span(c,c->exe+0x1230db,28,0)||
    memcmp(c->exe+0x1230db,receive_setup,28))return 0;
 uintptr_t next=(uintptr_t)c->exe+0x1230f2;int64_t displacement;
 if(replacement>=next){if(replacement-next>INT32_MAX)return 0;displacement=(int64_t)(replacement-next);}
 else {if(next-replacement>(uint64_t)INT32_MAX+1)return 0;displacement=-(int64_t)(next-replacement);}
 MxbPublicationCallPlan p={(uintptr_t)c->exe+0x1230ed,(uintptr_t)c->exe+0x1bbe10,replacement,{0},{0}};
 memcpy(p.original,receive_setup+18,5);p.patch[0]=0xe8;int32_t d=(int32_t)displacement;memcpy(p.patch+1,&d,4);*out=p;return 1;
}
static int binding(MxbPublicationHooks *h,int require_ours){
 MxbPacingContext *c=&h->c.pacing;uint32_t tiles;
 if(!mxb_pace_binding(c,0,&tiles)||tiles!=h->tiles||!publisher_signature(c,1)||!receive_signature(c))return 0;
 unsigned char *g=c->exe+0x568488;
 if(mxb_pace_ptr(g+0x7c8)!=(uintptr_t)h->c.pending.pending||mxb_pace_ptr(g+0x7d0)!=(uintptr_t)h->c.pending.dirty||
    mxb_pace_ptr(g+0x7a8)!=(uintptr_t)h->c.native_published||
    mxb_pace_ptr(c->exe+0x5a21c8+0x28)!=(uintptr_t)h->receive_full||
    !mxb_pace_span(c,h->c.native_published,h->c.pending.pending_count*4,0)||
    !mxb_pace_span(c,(void*)mxb_pace_ptr(c->exe+0x5a21c8+0x28),h->c.pending.pending_count*4,0))return 0;
 MxbPubState s;if(mxb_pub_state(h->c.ledger,&s)!=MXB_PUB_OK||s.generation!=c->generation||s.cells!=h->c.pending.pending_count)return 0;
 if(require_ours&&(mxb_pace_ptr(c->exe+0x5a2210)!=(uintptr_t)c->wrapper[0]||
    memcmp(c->exe+0x2a37f5,h->c.call_plan.patch,5)||memcmp(c->exe+0x1230ed,h->c.receive_plan.patch,5)))return 0;
 return 1;
}
static void fail_at(MxbPublicationHooks *h,MxbPubResult r,int line){
 if(!h->faulted&&!h->fail_line)h->fail_line=line;
 InterlockedExchange(&h->failure_result,(LONG)r);InterlockedExchange(&h->faulted,1);
 /* Keep suppression wrappers installed. Disabling pacing alone does not allow
    native terrain sends: the dirty wrapper continues returning zero flags. */
 InterlockedExchange(&h->c.pacing.gate->bits,0);
}
#define fail(h,r) fail_at((h),(r),__LINE__)
static int current_owner(const MxbPublicationHooks *h,DWORD thread){
 return thread==h->c.pacing.gate->owner_thread&&h->c.pacing.generation==h->c.pacing.gate->generation;
}
MxbPublicationHooks *mxb_publication_hooks_create(const MxbPublicationHookConfig *c){
 size_t cells,tiles;
 if(!c||!c->ledger||!c->original_publisher||!c->original_dirty||!c->original_replace||!c->max_batch_tiles||
    !mxb_dimensions(c->pending.width,c->pending.height,&cells,&tiles)||c->pending.pending_count!=cells||
    c->pending.dirty_count<tiles||c->max_batch_tiles>tiles||c->pending.generation!=c->pacing.generation)return NULL;
 MxbPublicationCallPlan plan;
 if(!mxb_publication_call_plan(&c->pacing,c->call_plan.replacement,&plan)||
    plan.site!=c->call_plan.site||plan.original_target!=c->call_plan.original_target||
    memcmp(plan.original,c->call_plan.original,5)||memcmp(plan.patch,c->call_plan.patch,5))return NULL;
 if(!mxb_publication_receive_plan(&c->pacing,c->receive_plan.replacement,&plan)||
    plan.site!=c->receive_plan.site||plan.original_target!=c->receive_plan.original_target||
    memcmp(plan.original,c->receive_plan.original,5)||memcmp(plan.patch,c->receive_plan.patch,5))return NULL;
 MxbPublicationHooks *h=calloc(1,sizeof(*h));if(!h)return NULL;h->c=*c;h->tiles=tiles;
 if(!mxb_pace_binding(&h->c.pacing,0,NULL)){free(h);return NULL;}
 h->receive_full=(const int32_t*)mxb_pace_ptr(h->c.pacing.exe+0x5a21c8+0x28);
 h->before=calloc(tiles,1);h->zero=calloc(tiles,1);h->echo_seen=calloc(tiles,1);h->chosen=calloc(tiles,1);
 h->selected=calloc(tiles,sizeof(*h->selected));h->echo_tiles=calloc(tiles,sizeof(*h->echo_tiles));
 if(!h->before||!h->zero||!h->echo_seen||!h->chosen||!h->selected||!h->echo_tiles||!binding(h,0)){
  h->retired=1;mxb_publication_hooks_destroy(h);return NULL;
 }return h;
}
int mxb_publication_hooks_bind(MxbPublicationHooks *h){
 return h&&!h->stats.enabled&&!h->in_publisher&&InterlockedCompareExchangePointer((void*volatile*)&bound,h,NULL)==NULL;
}
int mxb_publication_hooks_unbind(MxbPublicationHooks *h){
 if(!h||h->stats.enabled||h->in_publisher||
    !mxb_pace_span(&h->c.pacing,h->c.pacing.exe+0x5a2210,8,0)||
    !mxb_pace_span(&h->c.pacing,h->c.pacing.exe+0x2a37f5,5,0)||
    !mxb_pace_span(&h->c.pacing,h->c.pacing.exe+0x1230ed,5,0)||
    mxb_pace_ptr(h->c.pacing.exe+0x5a2210)==(uintptr_t)h->c.pacing.wrapper[0]||
    !memcmp(h->c.pacing.exe+0x2a37f5,h->c.call_plan.patch,5)||
    !memcmp(h->c.pacing.exe+0x1230ed,h->c.receive_plan.patch,5))return 0;
 return InterlockedCompareExchangePointer((void*volatile*)&bound,NULL,h)==h;
}
int mxb_publication_hooks_enable(MxbPublicationHooks *h,DWORD thread){
 if(!h||h->retired||h->faulted||h->stats.enabled||h->in_publisher||!current_owner(h,thread)||!binding(h,1)||
    InterlockedCompareExchange(&h->c.pacing.gate->bits,0,0)!=MXB_PACE_REQUIRED)return 0;
 MxbPubState s;mxb_pub_state(h->c.ledger,&s);
 if(!s.active||s.faulted||s.reserved)return 0;h->stats.enabled=1;return 1;
}
/* Self-heal companion of mxb_pub_resync: clear the hook fault and per-call
   bookkeeping. Only while enabled, outside the native publisher, on the owner,
   and only if every native pointer is still bound to this transport. */
int mxb_publication_hooks_recover(MxbPublicationHooks *h,DWORD thread){
 if(!h||h->retired||!h->stats.enabled||h->in_publisher||!current_owner(h,thread))return 0;
 h->reservation=0;h->echo_count=0;h->dirty_seen=0;h->selected_count=0;memset(h->echo_seen,0,h->tiles);
 InterlockedExchange(&h->failure_result,0);h->fail_line=0;InterlockedExchange(&h->faulted,0);
 if(!binding(h,1)){fail(h,MXB_PUB_FAULT);return 0;}return 1;
}
int mxb_publication_hooks_disable_drained(MxbPublicationHooks *h,DWORD thread){
 if(!h||h->in_publisher||h->faulted||!current_owner(h,thread))return 0;
 MxbPubState s;if(mxb_pub_state(h->c.ledger,&s)!=MXB_PUB_OK||s.active||s.queued_targets||s.reserved||s.outstanding_native_events)return 0;
 h->stats.enabled=0;InterlockedExchange(&h->c.pacing.gate->bits,0);return 1;
}
int mxb_publication_hooks_retire(MxbPublicationHooks *h,int quiescent){
 if(!h||!quiescent||h->in_publisher)return 0;h->stats.enabled=0;h->retired=1;InterlockedExchange(&h->c.pacing.gate->bits,0);return 1;
}
int mxb_publication_hooks_destroy(MxbPublicationHooks *h){
 if(!h)return 1;if(h==bound||h->stats.enabled||h->in_publisher||(!h->retired&&(h->faulted||h->reservation)))return 0;
 free(h->before);free(h->zero);free(h->echo_seen);free(h->chosen);free(h->selected);free(h->echo_tiles);free(h);return 1;
}
int mxb_publication_hooks_state(const MxbPublicationHooks *h,MxbPublicationHookState *s){
 if(!h||!s)return 0;*s=h->stats;s->faulted=h->faulted;s->in_publisher=h->in_publisher;s->deferred_echo_tiles=h->echo_count;
 if(h->faulted)s->last_ledger_result=(MxbPubResult)h->failure_result;s->fail_line=h->fail_line;return 1;
}
static int valid_return(const MxbPacingContext *c,const void *slot,uintptr_t lo,uintptr_t hi,uintptr_t expected){
 uintptr_t a=(uintptr_t)slot;return a>=lo&&a<=UINTPTR_MAX-8&&a+8<=hi&&mxb_pace_span(c,slot,8,0)&&mxb_pace_ptr(slot)==expected;
}
static int suppress(MxbPublicationHooks *h,unsigned char **out,DWORD error){
 if(mxb_pace_span(&h->c.pacing,out,sizeof(*out),1))*out=h->zero;
 SetLastError(error);return 1; /* native publisher ignores callback's EAX */
}
int mxb_publication_dirty(MxbPublicationHooks *h,int id,unsigned mode,unsigned char **out,const int32_t *range,
 const void *slot,uintptr_t lo,uintptr_t hi,DWORD thread){
 if(!h)return 1;
 if(!h->stats.enabled||id!=1||mode!=0x104)return h->c.original_dirty(id,mode,out,range);
 DWORD incoming=GetLastError();
 if(h->faulted)return suppress(h,out,incoming);
 if(!current_owner(h,thread)||!h->in_publisher||h->dirty_seen++||!binding(h,1)||
    !mxb_pace_span(&h->c.pacing,out,sizeof(*out),1)||
    !mxb_pace_widen(&h->c.pacing,0,id,mode,range,slot,lo,hi,thread)){
  fail(h,MXB_PUB_FAULT);return suppress(h,out,incoming);
 }
 h->stats.dirty_calls++;
 MxbPubIssueReceipt issue;MxbPubResult r=mxb_pub_issue(h->c.ledger,&h->c.pending,1,&issue);
 h->stats.last_ledger_result=r;
 if(r!=MXB_PUB_TARGET_ISSUED&&r!=MXB_PUB_PROGRESS&&r!=MXB_PUB_BLOCKED&&r!=MXB_PUB_NO_CHANGE){
  fail(h,r);return suppress(h,out,incoming);
 }
 /* Snapshot the complete post-issue source flags before the native callback
    clears them. Native has not sent anything yet. This small copy is also the
    exact rollback for all capacity-rejected/unselected work. */
 memcpy(h->before,h->c.pending.dirty,h->tiles);
 SetLastError(incoming);int native_result=h->c.original_dirty(id,mode,out,range);DWORD native_error=GetLastError();
 unsigned char *snapshot=(unsigned char*)mxb_pace_ptr(h->c.pacing.exe+0x568488+0x7d8);
 if(*out!=snapshot||memcmp(snapshot,h->before,h->tiles)||memcmp(h->c.pending.dirty,h->zero,h->tiles)){
  /* Known canonical spans remain owned. Never hand a foreign callback pointer
     to the native loop. Keep every pre-callback dirty flag for retirement. */
  memcpy(h->c.pending.dirty,h->before,h->tiles);fail(h,MXB_PUB_FAULT);suppress(h,out,native_error);return native_result;
 }
 memset(h->chosen,0,h->tiles);size_t selected=0,last=h->cursor;
 for(size_t step=0;step<h->tiles&&selected<h->c.max_batch_tiles;step++){
  size_t tile=(h->cursor+step)%h->tiles;
  if(snapshot[tile]){h->chosen[tile]=1;selected++;last=(tile+1)%h->tiles;}
 }
 h->cursor=last;selected=0;
 for(size_t tile=0;tile<h->tiles;tile++)if(h->chosen[tile])h->selected[selected++]=(uint32_t)tile;
 r=MXB_PUB_NO_CHANGE;
 while(selected){
  r=mxb_pub_prepare_native_clear_batch(h->c.ledger,h->selected,selected,&h->c.pending,&h->reservation);
  if(r!=MXB_PUB_QUEUE_FULL)break;selected--;
 }
 if(selected&&r!=MXB_PUB_OK){selected=0;fail(h,r);}
 memset(h->chosen,0,h->tiles);
 for(size_t j=0;j<selected;j++)h->chosen[h->selected[j]]=1;
 size_t deferred=0;
 for(size_t tile=0;tile<h->tiles;tile++)if(!h->chosen[tile]){
  h->c.pending.dirty[tile]=h->before[tile];snapshot[tile]=0;deferred+=h->before[tile]!=0;
 }
 h->selected_count=selected;h->stats.last_selected_tiles=selected;h->stats.last_ledger_result=r;
 if(deferred)h->stats.backpressure_batches++;
 SetLastError(native_error);return native_result;
}
MxbPubResult mxb_publication_after_native_replace(MxbPublicationHooks *h,uint64_t generation,uint32_t tile,DWORD thread){
 DWORD error=GetLastError();MxbPubResult r=MXB_PUB_INACTIVE;
 if(!h||!h->stats.enabled)goto done;
 if(h->faulted){r=MXB_PUB_FAULT;goto done;}
 if(!current_owner(h,thread)||generation!=h->c.pacing.generation||tile>=h->tiles||!binding(h,1)){
  r=MXB_PUB_FAULT;fail(h,r);goto done;
 }
 if(h->in_publisher){
  if(!h->echo_seen[tile]){h->echo_seen[tile]=1;h->echo_count++;}r=MXB_PUB_BUSY;goto done;
 }
 MxbPubEchoReceipt receipt;r=mxb_pub_observe_native_echo(h->c.ledger,tile,h->c.native_published,h->c.pending.pending_count,&receipt);
 h->stats.last_ledger_result=r;
 if(r==MXB_PUB_OK||r==MXB_PUB_NO_CHANGE)h->stats.echo_batches++;else fail(h,r);
 done:SetLastError(error);return r;
}
int mxb_publication_run(MxbPublicationHooks *h,const void *slot,uintptr_t lo,uintptr_t hi,DWORD thread){
 if(!h)return 0;if(!h->stats.enabled)return h->c.original_publisher();
 DWORD incoming=GetLastError();
 if(!current_owner(h,thread)||!valid_return(&h->c.pacing,slot,lo,hi,(uintptr_t)h->c.pacing.exe+0x2a37fa)||
    InterlockedCompareExchange(&h->in_publisher,1,0)!=0){fail(h,MXB_PUB_FAULT);SetLastError(incoming);return 0;}
 h->stats.publisher_calls++;h->dirty_seen=0;h->selected_count=0;h->echo_count=0;memset(h->echo_seen,0,h->tiles);
 uint64_t prior_reservation=h->reservation;
 if(prior_reservation&&!h->faulted)fail(h,MXB_PUB_FAULT);
 if(!h->faulted&&!binding(h,1))fail(h,MXB_PUB_FAULT);
 SetLastError(incoming);int result=h->c.original_publisher();DWORD native_error=GetLastError();
 /* Terrain is stream4, the final stream. Normal native return follows every
    selected row clear. An exception/longjmp is NOT a normal completion. */
 int still_bound=binding(h,1);if(!still_bound)fail(h,MXB_PUB_FAULT);
 if(h->reservation&&h->reservation!=prior_reservation&&still_bound){
  MxbPubResult r=mxb_pub_finish_native_clear(h->c.ledger,h->reservation,&h->c.pending);h->stats.last_ledger_result=r;
  if(r==MXB_PUB_OK){h->stats.sent_batches++;h->reservation=0;}else fail(h,r);
 }
 if(!h->faulted&&h->echo_count){
  size_t count=0;for(size_t tile=0;tile<h->tiles;tile++)if(h->echo_seen[tile])h->echo_tiles[count++]=(uint32_t)tile;
  MxbPubBatchEchoReceipt receipt;MxbPubResult r=mxb_pub_observe_native_echo_batch(h->c.ledger,h->echo_tiles,count,
   h->c.native_published,h->c.pending.pending_count,&receipt);h->stats.last_ledger_result=r;
  if(r==MXB_PUB_OK||r==MXB_PUB_NO_CHANGE)h->stats.echo_batches++;else fail(h,r);
 }
 h->echo_count=0;InterlockedExchange(&h->in_publisher,0);SetLastError(native_error);return result;
}
int mxb_publication_receive(MxbPublicationHooks *h,int id,const int32_t *full,int tile,const void *slot,
 uintptr_t lo,uintptr_t hi,DWORD thread){
 if(!h)return 1;if(!h->stats.enabled||id!=1)return h->c.original_replace(id,full,tile);
 DWORD incoming=GetLastError();
 if(h->faulted){SetLastError(incoming);return 1;}
 if(!current_owner(h,thread)||!valid_return(&h->c.pacing,slot,lo,hi,(uintptr_t)h->c.pacing.exe+0x1230f2)||
    tile<0||(size_t)tile>=h->tiles||!binding(h,1)||
    (uintptr_t)full!=mxb_pace_ptr(h->c.pacing.exe+0x5a21c8+0x28)){
  fail(h,MXB_PUB_FAULT);SetLastError(incoming);return 1;
 }
 SetLastError(incoming);int result=h->c.original_replace(id,full,tile);DWORD native_error=GetLastError();
 if(result==0){
  MxbPubResult r=mxb_publication_after_native_replace(h,h->c.pacing.generation,(uint32_t)tile,thread);
  h->stats.last_ledger_result=r;
 }
 SetLastError(native_error);return result;
}
int mxb_publication_publisher_entry(void){
 MxbPublicationHooks *h=bound;NT_TIB *t=(NT_TIB*)NtCurrentTeb();
 return mxb_publication_run(h,MXB_NATIVE_RETURN_SLOT,(uintptr_t)t->StackLimit,(uintptr_t)t->StackBase,GetCurrentThreadId());
}
int mxb_publication_dirty_entry(int id,unsigned mode,unsigned char **out,const int32_t *range){
 MxbPublicationHooks *h=bound;NT_TIB *t=(NT_TIB*)NtCurrentTeb();
 return mxb_publication_dirty(h,id,mode,out,range,MXB_NATIVE_RETURN_SLOT,(uintptr_t)t->StackLimit,(uintptr_t)t->StackBase,GetCurrentThreadId());
}
int mxb_publication_receive_entry(int id,const int32_t *full,int tile){
 MxbPublicationHooks *h=bound;NT_TIB *t=(NT_TIB*)NtCurrentTeb();
 return mxb_publication_receive(h,id,full,tile,MXB_NATIVE_RETURN_SLOT,(uintptr_t)t->StackLimit,(uintptr_t)t->StackBase,GetCurrentThreadId());
}
