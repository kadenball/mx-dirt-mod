#include "mxdirt_transport.h"
#include <stdlib.h>
#include <intrin.h>
#if defined(__clang__) && !__has_builtin(_AddressOfReturnAddress)
#error Build with -fms-extensions for the real native return-slot intrinsic.
#endif
#pragma intrinsic(_AddressOfReturnAddress)
typedef struct {void *address;size_t bytes;int kind;unsigned char original[640],next[640];} TransportPatch;
struct MxdirtTransport {
 MxbTransportConfig c;MxbPacingGate pace;MxbPacingContext context;MxbExcavationGate excavation;
 MxbHeightPublication *ledger;MxbPublicationHooks *hooks;MxbPendingView pending;
 const int16_t *base;const int32_t *published;size_t cells,tiles;
 MxbPublicationCallPlan publisher_plan,receive_plan;MxbExcavationCallPlan excavation_plan;MxbPacingPlan pacing_plan[2];
 TransportPatch patches[MXB_TRANSPORT_PATCHES];unsigned owned;
 volatile LONG phase,faulted,inflight;int installed;int fault_line,fault_why;unsigned fault_detail;unsigned recoveries;
 MxbPubResult last;unsigned char *zero;MxbTransportTrace *trace;size_t trace_count,trace_dropped;
 uint64_t calls;MxbNativePublisher original_publisher;MxbNativeReplace original_replace;MxbPacingDirty original_dirty;
};
static MxdirtTransport *volatile transport_bound;
static const char transport_sha[]="9e792d7aed1dc40df11eea51d1a6d0519daec7be2b65609905df8c9de804a291";
static LONG atomic_read(volatile LONG *v){return InterlockedCompareExchange(v,0,0);}
static LONGLONG ticks(void){LARGE_INTEGER q;QueryPerformanceCounter(&q);return q.QuadPart;}
static int owner(const MxdirtTransport *t,DWORD thread){return t&&thread==t->pace.owner_thread&&thread==GetCurrentThreadId();}
static int facts(const MxdirtTransport *t,const MxbTransportFacts *f,int stopped){
 return t&&f&&owner(t,f->owner_thread)&&f->generation==t->pace.generation&&f->offline_testing&&f->exact_track_profile&&f->pinned_lifetime&&
  (!stopped||(f->engine_quiescent&&!atomic_read((volatile LONG*)&t->inflight)));
}
static void fault_at(MxdirtTransport *t,MxbPubResult why,int line,unsigned detail){
 if(!t)return;if(!atomic_read(&t->faulted)&&!t->fault_line){t->fault_line=line;t->fault_why=(int)why;t->fault_detail=detail;}
 InterlockedExchange(&t->faulted,1);InterlockedExchange(&t->pace.bits,0);
 InterlockedExchange(&t->phase,MXB_TRANSPORT_FAULTED);InterlockedExchange(&t->excavation.faulted,1);
 /* Never release the other terrain writer on a fault or a pause. */
 if(owner(t,GetCurrentThreadId()))t->last=why;
}
#define fault(t,why) fault_at((t),(why),__LINE__,0u)
static void record(MxdirtTransport *t,int kind,int result,uint32_t tile,LONGLONG begin){
 if(!t||!owner(t,GetCurrentThreadId()))return;
 if(t->trace_count==t->c.trace_capacity){t->trace_dropped++;return;}
 MxbPubState s;MxbPublicationHookState h;memset(&s,0,sizeof(s));memset(&h,0,sizeof(h));
 if(t->ledger)mxb_pub_state(t->ledger,&s);if(t->hooks)mxb_publication_hooks_state(t->hooks,&h);
 MxbTransportTrace *r=&t->trace[t->trace_count++];
 *r=(MxbTransportTrace){t->trace_count,t->pace.generation,begin,ticks(),GetCurrentThreadId(),kind,result,t->phase,tile,s.queued_targets,s.outstanding_native_events,h.last_selected_tiles};
}
static int owned_all(const MxdirtTransport *t){
 if(!t||t->owned!=255||!t->installed)return 0;
 for(unsigned k=0;k<8;k++){
  if(!mxb_pace_span(&t->context,t->patches[k].address,t->patches[k].bytes,0)||
   memcmp(t->patches[k].address,t->patches[k].next,t->patches[k].bytes))return 0;
 }return 1;
}
static void health(MxdirtTransport *t,DWORD thread){
 if(!owner(t,thread)){fault(t,MXB_PUB_FAULT);return;}
 MxbPublicationHookState h;MxbPubState s;mxb_publication_hooks_state(t->hooks,&h);mxb_pub_state(t->ledger,&s);
 LONG bits=atomic_read(&t->pace.bits);int pace_bad=(t->phase==MXB_TRANSPORT_ACTIVE||t->phase==MXB_TRANSPORT_PAUSED)&&bits!=31;
 if(h.faulted||s.faulted||t->excavation.faulted||pace_bad)
  fault_at(t,MXB_PUB_FAULT,__LINE__,(h.faulted?1u:0u)|(s.faulted?2u:0u)|(t->excavation.faulted?4u:0u)|(pace_bad?8u:0u)|((unsigned)bits<<8));
}
int mxb_transport_patch_windows(void *user,void *address,const void *before,const void *after,size_t n,int kind){
 (void)user;if(!address||!before||!after||!n)return 0;
 if(kind){MEMORY_BASIC_INFORMATION m;
  if(n!=8||((uintptr_t)address&7)||!VirtualQuery(address,&m,sizeof(m))||m.State!=MEM_COMMIT||
   (m.Protect&255)!=PAGE_READWRITE||(m.Protect&PAGE_GUARD))return 0;
  void *old,*next;memcpy(&old,before,8);memcpy(&next,after,8);
  return InterlockedCompareExchangePointer((void*volatile*)address,next,old)==old;}
 if(memcmp(address,before,n))return 0;DWORD old,ignored;if(!VirtualProtect(address,n,PAGE_EXECUTE_READWRITE,&old))return 0;
 memcpy(address,after,n);int flushed=FlushInstructionCache(GetCurrentProcess(),address,n)!=0;
 int protected_again=VirtualProtect(address,n,old,&ignored)!=0;return flushed&&protected_again;
}
static void patch_plan(TransportPatch *p,void *address,const void *before,const void *after,size_t n,int kind){
 p->address=address;p->bytes=n;p->kind=kind;memcpy(p->original,before,n);memcpy(p->next,after,n);
}
static void relay(unsigned char *p,const void *fn){const unsigned char j[6]={0xff,0x25,0,0,0,0};memcpy(p,j,6);memcpy(p+6,&fn,8);}
static int all_zero(const void *p,size_t n){const unsigned char *v=p;for(size_t i=0;i<n;i++)if(v[i])return 0;return 1;}
static int create_reject(MxbTransportCreateReport *r,const char *stage,int side){
 if(r)*r=(MxbTransportCreateReport){stage,side};return 0;
}
static int clean_binding_report(MxdirtTransport *t,MxbTransportCreateReport *r){
 for(int side=0;side<2;side++)if(!mxb_pace_binding(&t->context,side,NULL))return create_reject(r,"stream_binding",side);
 unsigned char *g=t->c.exe+0x568488;uint32_t w=mxb_pace_u32(g+0x740),h=mxb_pace_u32(g+0x744);
 if(w<64||h<64||w>8193||h>8193||(uint64_t)w*h>34000000)return create_reject(r,"dimensions",0);
 if(t->ledger&&(w!=t->pending.width||h!=t->pending.height))return create_reject(r,"changed_dimensions",0);
 t->cells=(size_t)w*h;t->tiles=(size_t)((w+63)/64)*((h+63)/64);
 if(t->ledger&&(mxb_pace_ptr(g+0x748)!=(uintptr_t)t->base||mxb_pace_ptr(g+0x7a8)!=(uintptr_t)t->published||
  mxb_pace_ptr(g+0x7c8)!=(uintptr_t)t->pending.pending||mxb_pace_ptr(g+0x7d0)!=(uintptr_t)t->pending.dirty))return create_reject(r,"changed_grid_pointers",0);
 t->base=(const int16_t*)mxb_pace_ptr(g+0x748);const int16_t *height=(const int16_t*)mxb_pace_ptr(g+0x750);
 t->published=(const int32_t*)mxb_pace_ptr(g+0x7a8);
 t->pending=(MxbPendingView){w,h,t->pace.generation,(int32_t*)mxb_pace_ptr(g+0x7c8),t->cells,(unsigned char*)mxb_pace_ptr(g+0x7d0),t->tiles};
 if(!mxb_pace_span(&t->context,t->base,t->cells*2,0)||!mxb_pace_span(&t->context,height,t->cells*2,0)||
  !mxb_pace_span(&t->context,t->published,t->cells*4,0))return create_reject(r,"height_spans",0);
 if(t->ledger&&!mxb_pub_matches_initial(t->ledger,t->base,t->published,t->cells))return create_reject(r,"initial_seed_changed",0);
 for(int side=0;side<2;side++){
  unsigned char *grid=g+side*0x6160,*record=t->c.exe+mxb_pace_record[side];
  if(mxb_pace_u32(grid+0x740)!=w||mxb_pace_u32(grid+0x744)!=h)return create_reject(r,"peer_dimensions",side);
  const void *source=(void*)mxb_pace_ptr(record),*last=(void*)mxb_pace_ptr(record+0x28);
  unsigned cb=side?0x58:0x48,fo=side?0x7b0:0x7d0;
  uintptr_t fn=mxb_pace_ptr(record+cb);
  if(fn!=(uintptr_t)(t->installed?(void*)t->context.wrapper[side]:(void*)(t->c.exe+0x1bc140)))return create_reject(r,"dirty_callback",side);
  if(!mxb_pace_span(&t->context,last,t->cells*4,0))return create_reject(r,"stream_last_span",side);
  /* The client sends increments; the host sends full DFM. A stopped, fully
     converged nonzero DFM is a valid initial surface, not unfinished work. */
  if(side?memcmp(source,t->published,t->cells*4):!all_zero(source,t->cells*4))return create_reject(r,side?"host_source_mismatch":"pending_not_zero",side);
  if(memcmp(last,t->published,t->cells*4))return create_reject(r,"stream_last_mismatch",side);
  const int16_t *peer_base=(const int16_t*)mxb_pace_ptr(grid+0x748),*peer_height=(const int16_t*)mxb_pace_ptr(grid+0x750);
  if(!mxb_pace_span(&t->context,peer_base,t->cells*2,0)||!mxb_pace_span(&t->context,peer_height,t->cells*2,0))return create_reject(r,"peer_height_spans",side);
  if(memcmp(peer_base,t->base,t->cells*2))return create_reject(r,"peer_base_mismatch",side);
  for(size_t k=0;k<t->cells;k++){
   int32_t expected=(int32_t)peer_base[k]-(t->published[k]>>16);
   if(expected<INT16_MIN||expected>INT16_MAX||peer_height[k]!=expected)return create_reject(r,"height_dfm_mismatch",side);
  }
  if(!all_zero((void*)mxb_pace_ptr(grid+fo),t->tiles))return create_reject(r,"dirty_flags_not_zero",side);
  /* grid+fo+8 is the dirty callback's prior output, not its pending queue.
     Binding validates its span/distinctness. Never clear or adopt its bytes. */
 }return 1;
}
static int clean_binding(MxdirtTransport *t){return clean_binding_report(t,NULL);}
MxdirtTransport *mxb_transport_create_report(const MxbTransportConfig *c,const MxbTransportFacts *f,MxbTransportCreateReport *r){
 create_reject(r,"config_or_lifecycle",-1);
 if(!c||!f||!f->generation||!f->owner_thread||f->owner_thread!=GetCurrentThreadId()||!f->offline_testing||!f->exact_track_profile||
  !f->pinned_lifetime||!f->engine_quiescent||!f->transport_quiescent||!c->verified_executable_sha256||strcmp(c->verified_executable_sha256,transport_sha)||
  !c->exe||!c->span||!c->near_code||c->near_code_bytes<640||!c->target_capacity||!c->native_event_capacity||!c->max_changed_cells||
  !c->max_batch_tiles||c->max_batch_tiles>c->native_event_capacity||c->trace_capacity>8192)return NULL;
#ifndef MXB_TRANSPORT_PRIVATE_FIXTURE
 if(c->fixture_publisher||c->fixture_replace||c->fixture_dirty||c->fixture_excavation){create_reject(r,"fixture_override",-1);return NULL;}
#endif
 create_reject(r,"transport_allocation",-1);
 MxdirtTransport *t=calloc(1,sizeof(*t));if(!t)return NULL;t->c=*c;
 if(!t->c.patch)t->c.patch=mxb_transport_patch_windows;t->pace=(MxbPacingGate){0,f->owner_thread,f->generation};
 t->context=(MxbPacingContext){c->exe,c->image_bytes,1,&t->pace,f->generation,c->span,c->span_user,{mxb_transport_client_dirty_entry,mxb_transport_host_dirty_entry}};
 MEMORY_BASIC_INFORMATION code_region;
 create_reject(r,"relay_protection",-1);
 if(!VirtualQuery(c->near_code,&code_region,sizeof(code_region))||!(code_region.Protect&(PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))goto bad;
 create_reject(r,"pacing_context",-1);if(!mxb_pace_context(&t->context))goto bad;
 create_reject(r,"relay_span",-1);if(!mxb_pace_span(&t->context,c->near_code,640,0))goto bad;
 create_reject(r,"relay_not_zero",-1);if(!all_zero(c->near_code,640))goto bad;
 if(!clean_binding_report(t,r))goto bad;
 create_reject(r,"publisher_plan",-1);if(!mxb_publication_call_plan(&t->context,(uintptr_t)c->near_code,&t->publisher_plan))goto bad;
 create_reject(r,"receive_plan",-1);if(!mxb_publication_receive_plan(&t->context,(uintptr_t)c->near_code+128,&t->receive_plan))goto bad;
 create_reject(r,"excavation_plan",-1);
 if(!mxb_pace_span(&t->context,c->exe+0x1a4d8f,5,0)||!mxb_pace_span(&t->context,c->exe+0x1bbda0,12,0)||
  !mxb_excavation_call_plan((uintptr_t)c->exe,(uintptr_t)c->near_code+256,1,c->exe+0x1a4d8f,c->exe+0x1bbda0,&t->excavation_plan))goto bad;
 for(int side=0;side<2;side++){create_reject(r,"pacing_plan",side);if(!mxb_pace_build(&t->context,side,(uintptr_t)c->near_code+384+128*side,&t->pacing_plan[side]))goto bad;}
 t->original_publisher=c->fixture_publisher?c->fixture_publisher:(MxbNativePublisher)t->publisher_plan.original_target;
 t->original_replace=c->fixture_replace?c->fixture_replace:(MxbNativeReplace)t->receive_plan.original_target;
 t->original_dirty=c->fixture_dirty?c->fixture_dirty:(MxbPacingDirty)(c->exe+0x1bc140);
 create_reject(r,"excavation_gate",-1);if(!mxb_excavation_gate_init(&t->excavation,(uintptr_t)c->exe,f->generation,f->owner_thread,
  c->fixture_excavation?c->fixture_excavation:(MxbNativeExcavation)t->excavation_plan.original_target))goto bad;
 create_reject(r,"publication_ledger",-1);t->ledger=mxb_pub_create_sparse(t->pending.width,t->pending.height,f->generation,t->base,t->published,t->cells,&t->pending,c->target_capacity,c->native_event_capacity,c->max_changed_cells);
 if(!t->ledger)goto bad;
 MxbPublicationHookConfig hc={t->context,t->ledger,t->pending,t->published,t->original_publisher,t->original_dirty,t->publisher_plan,
  t->original_replace,t->receive_plan,c->max_batch_tiles};
 t->hooks=mxb_publication_hooks_create(&hc);t->zero=calloc(t->tiles,1);t->trace=calloc(c->trace_capacity?c->trace_capacity:1,sizeof(*t->trace));
 create_reject(r,"publication_hooks",-1);if(!t->hooks)goto bad;
 create_reject(r,"trace_allocation",-1);if(!t->zero||!t->trace)goto bad;
 unsigned char code[640]={0},zero[640]={0};relay(code,(void*)mxb_transport_publisher_entry);relay(code+128,(void*)mxb_transport_receive_entry);relay(code+256,(void*)mxb_transport_excavation_entry);
 memcpy(code+384,t->pacing_plan[0].stub,t->pacing_plan[0].stub_size);memcpy(code+512,t->pacing_plan[1].stub,t->pacing_plan[1].stub_size);
 patch_plan(&t->patches[0],c->near_code,zero,code,640,0);
 patch_plan(&t->patches[1],(void*)t->publisher_plan.site,t->publisher_plan.original,t->publisher_plan.patch,5,0);
 patch_plan(&t->patches[2],(void*)t->receive_plan.site,t->receive_plan.original,t->receive_plan.patch,5,0);
 patch_plan(&t->patches[3],(void*)t->excavation_plan.site,t->excavation_plan.original,t->excavation_plan.patch,5,0);
 for(int side=0;side<2;side++)patch_plan(&t->patches[4+side],(void*)t->pacing_plan[side].patch_address,t->pacing_plan[side].original,t->pacing_plan[side].patch,6,0);
 for(int side=0;side<2;side++){void *original=c->exe+0x1bc140,*next=(void*)t->context.wrapper[side];
  patch_plan(&t->patches[6+side],c->exe+(side?0xa1e4c8:0x5a2210),&original,&next,8,1);}
 t->phase=MXB_TRANSPORT_PREPARED;record(t,1,1,0,ticks());create_reject(r,"ready",-1);return t;
 bad:if(t->hooks)mxb_publication_hooks_destroy(t->hooks);mxb_pub_destroy(t->ledger);free(t->trace);free(t->zero);free(t);return NULL;
}
MxdirtTransport *mxb_transport_create(const MxbTransportConfig *c,const MxbTransportFacts *f){return mxb_transport_create_report(c,f,NULL);}
int mxb_transport_matches_initial(const MxdirtTransport *t,const MxbTransportFacts *f,
 const int16_t *base,const int32_t *initial,size_t cells){
 return facts(t,f,1)&&f->transport_quiescent&&t->phase==MXB_TRANSPORT_PREPARED&&!t->owned&&
  !t->installed&&!t->faulted&&mxb_pub_matches_initial(t->ledger,base,initial,cells);
}
static int restore_owned(MxdirtTransport *t){
 int ok=1;
 for(int k=7;k>=0;k--){if(!(t->owned&(1u<<k)))continue;
  /* Keep relay block while ANY native hook is still installed or unknown. */
  if(!k&&(t->owned&254u)){ok=0;continue;}
  TransportPatch *p=&t->patches[k];
  if(!mxb_pace_span(&t->context,p->address,p->bytes,0)){ok=0;continue;}
  if(!memcmp(p->address,p->original,p->bytes)){t->owned&=~(1u<<k);continue;}
  if(memcmp(p->address,p->next,p->bytes)||!t->c.patch(t->c.patch_user,p->address,p->next,p->original,p->bytes,p->kind)||
   memcmp(p->address,p->original,p->bytes)){ok=0;continue;}t->owned&=~(1u<<k);
 }return ok&&!t->owned;
}
int mxb_transport_install(MxdirtTransport *t,const MxbTransportFacts *f){
 if(!facts(t,f,1)||!f->transport_quiescent||t->phase!=MXB_TRANSPORT_PREPARED||t->owned||!clean_binding(t))return 0;
 for(unsigned k=0;k<8;k++)if(!mxb_pace_span(&t->context,t->patches[k].address,t->patches[k].bytes,0)||
  memcmp(t->patches[k].address,t->patches[k].original,t->patches[k].bytes))return 0;
 if(InterlockedCompareExchangePointer((void*volatile*)&transport_bound,t,NULL))return 0;
 int ok=1;
 for(unsigned k=0;k<8;k++){
  TransportPatch *p=&t->patches[k];int wrote=t->c.patch(t->c.patch_user,p->address,p->original,p->next,p->bytes,p->kind);
  if(memcmp(p->address,p->original,p->bytes))t->owned|=1u<<k;
  if(!wrote||memcmp(p->address,p->next,p->bytes)){ok=0;break;}
 }
 if(!ok){if(restore_owned(t)){InterlockedCompareExchangePointer((void*volatile*)&transport_bound,NULL,t);record(t,2,0,0,ticks());}
  else fault(t,MXB_PUB_FAULT);return 0;}
 t->installed=1;t->phase=MXB_TRANSPORT_INSTALLED;record(t,2,1,0,ticks());return 1;
}
int mxb_transport_activate(MxdirtTransport *t,const MxbTransportFacts *f,int explicit_activation){
 if(!facts(t,f,1)||!f->transport_quiescent||!explicit_activation||t->phase!=MXB_TRANSPORT_INSTALLED||t->faulted||!owned_all(t)||
  !clean_binding(t))return 0;
 if(!mxb_excavation_gate_activate(&t->excavation,f->generation,f->owner_thread,1,1,1))return 0;
 t->last=mxb_pub_activate(t->ledger,&t->pending,t->published,t->cells,1);
 if(t->last!=MXB_PUB_OK){fault(t,t->last);return 0;}
 /* Both selectors and dirty callbacks are owned BEFORE publishing the single
    shared gate. External quiescence excludes invocation between these stores. */
 InterlockedExchange(&t->pace.bits,31);
 if(!mxb_publication_hooks_enable(t->hooks,f->owner_thread)){fault(t,MXB_PUB_FAULT);return 0;}
 t->phase=MXB_TRANSPORT_ACTIVE;record(t,3,1,0,ticks());return 1;
}
int mxb_transport_accept(void *context,uint64_t generation,uint64_t previous,uint64_t sequence,const MxbPubAbsoluteCell *changes,size_t count){
 MxdirtTransport *t=context;if(!t)return -1;if(!owner(t,GetCurrentThreadId())){fault(t,MXB_PUB_FAULT);return -1;}
 if(t->faulted)return -1;if(t->phase==MXB_TRANSPORT_PAUSED||t->phase==MXB_TRANSPORT_DRAINED)return 0;
 if(t->phase!=MXB_TRANSPORT_ACTIVE||generation!=t->pace.generation){fault(t,MXB_PUB_INVALID);return -1;}
 health(t,GetCurrentThreadId());if(t->faulted)return -1;
 t->last=mxb_pub_enqueue_sparse(t->ledger,generation,previous,sequence,changes,count);
 if(t->last==MXB_PUB_OK||t->last==MXB_PUB_DUPLICATE){record(t,4,t->last,0,ticks());return 1;}
 if(t->last==MXB_PUB_QUEUE_FULL||t->last==MXB_PUB_BUSY)return 0;
 fault(t,t->last);record(t,4,t->last,0,ticks());return -1;
}
/* Self-heal (authored host policy): an external native terrain writer or a
   timing slip made native memory differ from the ledger. Adopt native state
   as truth instead of stopping deformation for the rest of the event. Only on
   the owner, between native publisher calls, with every patch still ours and
   the excavation gate still exclusive (native digging stays suppressed). */
void mxb_transport_set_adopt_foreign(MxdirtTransport *t,int enabled){if(t&&t->ledger)mxb_pub_set_adopt_foreign(t->ledger,enabled);}
uint64_t mxb_transport_foreign_tiles(const MxdirtTransport *t){return t&&t->ledger?mxb_pub_foreign_tiles(t->ledger):0;}
size_t mxb_transport_adopted_rects(const MxdirtTransport *t,uint64_t *cursor,uint32_t *rects,size_t max){return t&&t->ledger?mxb_pub_adopted_rects(t->ledger,cursor,rects,max):0;}
int mxb_transport_recover(MxdirtTransport *t,DWORD thread){
 if(!owner(t,thread)||!t->installed||atomic_read(&t->inflight)||!atomic_read(&t->faulted)||
    atomic_read(&t->phase)!=MXB_TRANSPORT_FAULTED||!atomic_read(&t->excavation.exclusive)||!owned_all(t))return 0;
 MxbPublicationHookState h;mxb_publication_hooks_state(t->hooks,&h);if(h.in_publisher||!h.enabled)return 0;
 if(mxb_pub_resync(t->ledger,&t->pending,t->published,t->cells)!=MXB_PUB_OK)return 0;
 if(!mxb_publication_hooks_recover(t->hooks,thread))return 0;
 InterlockedExchange(&t->excavation.faulted,0);InterlockedExchange(&t->pace.bits,31);
 t->fault_line=t->fault_why=0;t->fault_detail=0;t->last=MXB_PUB_OK;
 InterlockedExchange(&t->faulted,0);InterlockedExchange(&t->phase,MXB_TRANSPORT_ACTIVE);
 t->recoveries++;record(t,12,(int)t->recoveries,0,ticks());
 health(t,thread);return !atomic_read(&t->faulted);
}
int mxb_transport_pause(MxdirtTransport *t,DWORD thread){
 if(!owner(t,thread)||t->faulted||(t->phase!=MXB_TRANSPORT_ACTIVE&&t->phase!=MXB_TRANSPORT_PAUSED))return 0;
 t->phase=MXB_TRANSPORT_PAUSED;record(t,5,1,0,ticks());return 1;
}
int mxb_transport_resume(MxdirtTransport *t,DWORD thread){
 if(!owner(t,thread)||t->phase!=MXB_TRANSPORT_PAUSED||t->faulted||!owned_all(t))return 0;health(t,thread);if(t->faulted)return 0;
 t->phase=MXB_TRANSPORT_ACTIVE;record(t,5,0,0,ticks());return 1;
}
MxbPubResult mxb_transport_drain(MxdirtTransport *t,const MxbTransportFacts *f){
 if(!facts(t,f,1)||!f->transport_quiescent||t->phase!=MXB_TRANSPORT_PAUSED)return MXB_PUB_NEEDS_DRAIN;
 t->last=mxb_pub_deactivate_drained(t->ledger,&t->pending,t->published,t->cells,1);
 if(t->last==MXB_PUB_OK){if(!mxb_publication_hooks_disable_drained(t->hooks,f->owner_thread)){fault(t,MXB_PUB_FAULT);return MXB_PUB_FAULT;}t->phase=MXB_TRANSPORT_DRAINED;}
 else if(t->last<0)fault(t,t->last);record(t,6,t->last,0,ticks());return t->last;
}
int mxb_transport_retire(MxdirtTransport *t,const MxbTransportFacts *f){
 if(!facts(t,f,1)||!f->transport_quiescent||!f->old_track_retired)return 0;
 if(!mxb_publication_hooks_retire(t->hooks,1))return 0;t->phase=MXB_TRANSPORT_RETIRED;record(t,7,1,0,ticks());return 1;
}
int mxb_transport_remove(MxdirtTransport *t,const MxbTransportFacts *f){
 if(!facts(t,f,1)||!f->transport_quiescent||(t->phase!=MXB_TRANSPORT_DRAINED&&t->phase!=MXB_TRANSPORT_RETIRED&&t->phase!=MXB_TRANSPORT_INSTALLED))return 0;
 if(t->phase==MXB_TRANSPORT_RETIRED&&!f->old_track_retired)return 0;
 if(f->native_reset_slots){
  /* Teardown cleared these image records and may already have freed grids.
     Read no grid pointer on this path. Zero is accepted ONLY with explicit
     paired native-return evidence and exact current zeroed record snapshots. */
  if(f->native_reset_slots!=3||t->phase!=MXB_TRANSPORT_RETIRED||!f->old_track_retired)return 0;
  for(int side=0;side<2;side++){
   const void *r=t->c.exe+mxb_pace_record[side];
   if(!mxb_pace_span(&t->context,r,0x68,0)||!all_zero(r,0x68))return 0;
  }
  t->owned&=~(3u<<6);record(t,11,3,0,ticks());
 }
 InterlockedExchange(&t->pace.bits,0);
 if(!restore_owned(t)){fault(t,MXB_PUB_FAULT);record(t,8,0,0,ticks());return 0;}
 if(!mxb_excavation_gate_release(&t->excavation,f->generation,f->owner_thread,1,1)){fault(t,MXB_PUB_FAULT);return 0;}
 InterlockedCompareExchangePointer((void*volatile*)&transport_bound,NULL,t);t->installed=0;t->phase=MXB_TRANSPORT_REMOVED;record(t,8,1,0,ticks());return 1;
}
int mxb_transport_destroy(MxdirtTransport *t){
 if(!t)return 1;if(!owner(t,GetCurrentThreadId())||t==transport_bound||t->owned||t->installed||t->inflight||t->excavation.exclusive||
  (t->phase!=MXB_TRANSPORT_PREPARED&&t->phase!=MXB_TRANSPORT_REMOVED)||!mxb_publication_hooks_destroy(t->hooks))return 0;
 mxb_pub_destroy(t->ledger);free(t->zero);free(t->trace);free(t);return 1;
}
int mxb_transport_state(const MxdirtTransport *t,MxbTransportState *out){
 if(!t||!out||!owner(t,GetCurrentThreadId()))return 0;MxbPubState s;MxbPublicationHookState h;
 mxb_pub_state(t->ledger,&s);mxb_publication_hooks_state(t->hooks,&h);
 *out=(MxbTransportState){(MxbTransportPhase)t->phase,t->pace.generation,t->pace.owner_thread,t->owned,t->installed,t->excavation.exclusive,t->pace.bits,t->faulted,t->inflight,
  s.queued_targets,s.outstanding_native_events,t->trace_count,t->trace_dropped,s.allocated_bytes,s.last_enqueued_sequence,s.locally_issued_sequence,h.sent_batches,h.echo_batches,t->excavation.suppressed,t->last,t->fault_line,t->fault_why,t->fault_detail,h.fail_line,(int)h.last_ledger_result,s.fault_line};return 1;
}
const MxbTransportTrace *mxb_transport_trace(const MxdirtTransport *t,const MxbTransportFacts *f,size_t *count){
 if(!count||!facts(t,f,1))return NULL;*count=t->trace_count;return t->trace;
}
int mxb_transport_publisher(MxdirtTransport *t,const void *slot,uintptr_t lo,uintptr_t hi,DWORD thread){
 if(!t)return 0;DWORD incoming=GetLastError();InterlockedIncrement(&t->inflight);LONGLONG start=ticks();
 if((t->phase==MXB_TRANSPORT_ACTIVE||t->phase==MXB_TRANSPORT_PAUSED)&&!owned_all(t))fault(t,MXB_PUB_FAULT);
 SetLastError(incoming);int result=mxb_publication_run(t->hooks,slot,lo,hi,thread);DWORD outgoing=GetLastError();
 health(t,thread);if(owner(t,thread)){t->calls++;MxbPublicationHookState h;mxb_publication_hooks_state(t->hooks,&h);
  if(t->calls<=8||h.last_selected_tiles||t->faulted)record(t,9,result,0,start);}
 InterlockedDecrement(&t->inflight);SetLastError(outgoing);return result;
}
int mxb_transport_receive(MxdirtTransport *t,int id,const int32_t *full,int tile,const void *slot,uintptr_t lo,uintptr_t hi,DWORD thread){
 if(!t)return 1;DWORD incoming=GetLastError();InterlockedIncrement(&t->inflight);LONGLONG start=ticks();
 if((t->phase==MXB_TRANSPORT_ACTIVE||t->phase==MXB_TRANSPORT_PAUSED)&&!owned_all(t))fault(t,MXB_PUB_FAULT);
 if(t->faulted&&id==1){InterlockedDecrement(&t->inflight);SetLastError(incoming);return 1;}
 SetLastError(incoming);int result=mxb_publication_receive(t->hooks,id,full,tile,slot,lo,hi,thread);DWORD outgoing=GetLastError();
 health(t,thread);if(id==1)record(t,10,result,(uint32_t)tile,start);InterlockedDecrement(&t->inflight);SetLastError(outgoing);return result;
}
int mxb_transport_dirty(MxdirtTransport *t,int side,int id,unsigned mode,unsigned char **out,const int32_t *range,const void *slot,uintptr_t lo,uintptr_t hi,DWORD thread){
 if(!t||side<0||side>1)return 1;DWORD incoming=GetLastError();InterlockedIncrement(&t->inflight);int result;
 if((t->phase==MXB_TRANSPORT_ACTIVE||t->phase==MXB_TRANSPORT_PAUSED)&&!owned_all(t))fault(t,MXB_PUB_FAULT);
 if(t->faulted&&id==side+1&&mode==(side?4u:0x104u)){
  if(mxb_pace_span(&t->context,out,8,1))*out=t->zero;InterlockedDecrement(&t->inflight);SetLastError(incoming);return 1;}
 if(!side){SetLastError(incoming);result=mxb_publication_dirty(t->hooks,id,mode,out,range,slot,lo,hi,thread);}
 else if((t->phase==MXB_TRANSPORT_ACTIVE||t->phase==MXB_TRANSPORT_PAUSED||t->faulted)&&id==2&&mode==4){
  if(t->faulted||!mxb_pace_widen(&t->context,1,id,mode,range,slot,lo,hi,thread)){
   fault(t,MXB_PUB_FAULT);if(mxb_pace_span(&t->context,out,8,1))*out=t->zero;SetLastError(incoming);result=1;
  }else {SetLastError(incoming);result=t->original_dirty(id,mode,out,range);}
 }else {SetLastError(incoming);result=t->original_dirty(id,mode,out,range);}
 DWORD outgoing=GetLastError();health(t,thread);InterlockedDecrement(&t->inflight);SetLastError(outgoing);return result;
}
int mxb_transport_excavation(MxdirtTransport *t,void *terrain,float x,float z,double amount,DWORD thread){
 if(!t)return 0;DWORD incoming=GetLastError();InterlockedIncrement(&t->inflight);SetLastError(incoming);
 int result=mxb_excavation_invoke(&t->excavation,terrain,x,z,amount,thread);DWORD outgoing=GetLastError();
 if(t->excavation.faulted)fault(t,MXB_PUB_FAULT);InterlockedDecrement(&t->inflight);SetLastError(outgoing);return result;
}
int mxb_transport_publisher_entry(void){NT_TIB *t=(NT_TIB*)NtCurrentTeb();return mxb_transport_publisher(transport_bound,_AddressOfReturnAddress(),(uintptr_t)t->StackLimit,(uintptr_t)t->StackBase,GetCurrentThreadId());}
int mxb_transport_receive_entry(int id,const int32_t *full,int tile){NT_TIB *t=(NT_TIB*)NtCurrentTeb();return mxb_transport_receive(transport_bound,id,full,tile,_AddressOfReturnAddress(),(uintptr_t)t->StackLimit,(uintptr_t)t->StackBase,GetCurrentThreadId());}
int mxb_transport_client_dirty_entry(int id,unsigned mode,unsigned char **out,const int32_t *range){NT_TIB *t=(NT_TIB*)NtCurrentTeb();return mxb_transport_dirty(transport_bound,0,id,mode,out,range,_AddressOfReturnAddress(),(uintptr_t)t->StackLimit,(uintptr_t)t->StackBase,GetCurrentThreadId());}
int mxb_transport_host_dirty_entry(int id,unsigned mode,unsigned char **out,const int32_t *range){NT_TIB *t=(NT_TIB*)NtCurrentTeb();return mxb_transport_dirty(transport_bound,1,id,mode,out,range,_AddressOfReturnAddress(),(uintptr_t)t->StackLimit,(uintptr_t)t->StackBase,GetCurrentThreadId());}
int mxb_transport_excavation_entry(void *terrain,float x,float z,double amount){return mxb_transport_excavation(transport_bound,terrain,x,z,amount,GetCurrentThreadId());}
