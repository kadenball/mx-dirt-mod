/* MX Dirt Mod render plugin host (mx_dirt_render.dlo).
   Hooks, all for ONE exact mxbikes.exe (SHA-256 pinned below):
   - the CALL to the native terrain draw (rva 24f613) -> terrain_wrapper,
   - MX Bikes' own GL function-pointer slots for glDrawRangeElementsBaseVertex,
     glDrawRangeElementsEXT and glDrawElements -> read the camera (modelview /
     projection) at the first native terrain draw of each outer terrain call,
   - optional raised-dirt surface fix (mxdirt_surface_patch.h).
   Code patching happens only at the EventInit/EventDeinit/Shutdown lifecycle
   boundary on the Startup thread; every native call is forwarded exactly once
   with unchanged arguments. After the native terrain returns, the Dirt
   Material pass (mxdirt_render_content.c) draws over it. No terrain, mesh or
   texture writes. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#include "mxdirt_terrain_scope.h"
#include "mxdirt_camera.h"
#include "mxdirt_render_content.h"
#include "mxdirt_surface_patch.h"
#include "mxdirt_fast_read.h"
#define API __declspec(dllexport)
#define NOINLINE __declspec(noinline)
#define NATIVE_RETURN() __builtin_extract_return_addr(__builtin_return_address(0))
/* MX Bikes plugin SDK EventInit payload. */
typedef struct {
 char rider[100],bikeID[100],bikeName[100];int gears,maxRPM,limiter,shiftRPM;
 float temperature,alarm[2],fuel,susp[2],lock;
 char category[100],trackID[100],trackName[100];float length;int type;
} RenderEvent;
_Static_assert(sizeof(RenderEvent)==652,"SDK event ABI");
typedef void (APIENTRY *BaseFn)(GLenum,GLuint,GLuint,GLsizei,GLenum,const void*,GLint);
typedef void (APIENTRY *RangeFn)(GLenum,GLuint,GLuint,GLsizei,GLenum,const void*);
typedef void (APIENTRY *ElementsFn)(GLenum,GLsizei,GLenum,const void*);
static ElementsFn core_elements;
enum {AUDIT_MAX=128,LIFECYCLE_MAX=32};
enum { A_START=1,A_HASH_FAIL,A_GATE_FAIL,A_INSTALL,A_INSTALL_FAIL,A_ROLLBACK,
 A_FOREIGN_CALL,A_FOREIGN_SLOT,A_PROTECTION,A_THREAD,A_CONTEXT,A_FRAME,A_RUN,
 A_SLOT_INSTALL,A_SLOT_FAIL,A_NONQUIESCENT,A_SHUTDOWN,A_OVERFLOW,A_GENERATION,A_SLOT_CHECK,A_COUNT };
static const char *const audit_names[A_COUNT]={"?","start","exe_hash_mismatch","gate_fail","install","install_fail","rollback",
 "foreign_call","foreign_slot","protection","thread","context","frame","run","slot_install","slot_fail","nonquiescent","shutdown","overflow","generation","slot_check"};
enum { S_READ_FAILED=1,S_ABSENT,S_NOT_EXECUTABLE,S_RESOLVE_MISMATCH,S_VERIFIED,S_REPLACE_FAILED };
typedef struct {volatile LONG ready;uint32_t code;DWORD thread;uint64_t tick;uintptr_t a,b;uint32_t slot,reason;} Audit;
typedef struct {uint32_t event;uint64_t generation;} Lifecycle;
/* MX Bikes' GL entry-point slots (rva of the stored function pointers) and the
   return addresses of its terrain draws through them (16-bit / 32-bit indices). */
static const uint32_t slot_rva[3]={0x398f08,0x398f00,0x3216d0};
static const uint32_t draw_returns[3][2]={{0x231fb5,0x231fe1},{0x232054,0x232080},{0x2320ca,0x2320e3}};
enum { TERRAIN_RETURN=MXDIRT_TS_TERRAIN_CALL+5, SURFACE_RELAY=0x100 };
static unsigned char *image_base,*relay_page;static size_t image_size;
static MxDirtTsCallPlan plan;static unsigned char call_owned,slot_owned[3];static void *slot_original[3];
static MxDirtTsTerrainFn original_terrain;
static volatile LONG inflight,event_allowed,run_initialized,running,foreign,flush_lock;
static DWORD lifecycle_thread,render_thread;static HGLRC render_context;
static uint64_t generation,lifecycle_revision;static int generation_fault,content_busy;
static MxDirtTerrainScope capture;
static MxDirtCameraScope camera;static int camera_active,camera_seen,camera_failed;
static int verified_image,content_ready,hooks_wanted,slots_attempted,slots_ok,retire_pending,shutdown_pending;
static unsigned char surface_patch[5];static int surface_enabled=1,surface_owned;
static wchar_t config_path[32768];
static Audit audit[AUDIT_MAX];static volatile LONG audit_reserved;static LONG audit_flushed;
static Lifecycle pending[LIFECYCLE_MAX];static uint32_t pending_count;
static volatile LONG64 forwarded[4]; /* terrain, base-vertex, range, elements */
static uint64_t frames_attempted,frames_drawn,frames_skipped,frames_failed;
static LONG atomic(volatile LONG *p){return InterlockedCompareExchange(p,0,0);}
static void mark_detail(uint32_t code,uintptr_t a,uintptr_t b,uint32_t slot,uint32_t reason){
 LONG k=InterlockedIncrement(&audit_reserved)-1;if(k>=AUDIT_MAX)return;
 Audit *r=&audit[k];r->code=code;r->a=a;r->b=b;r->thread=GetCurrentThreadId();r->tick=GetTickCount64();
 r->slot=slot;r->reason=reason;InterlockedExchange(&r->ready,1);
}
static void mark(uint32_t code,uintptr_t a,uintptr_t b){mark_detail(code,a,b,UINT32_MAX,0);}
static ElementsFn resolve_core_elements(void){
 HMODULE module=GetModuleHandleW(L"opengl32.dll");
 return module?(ElementsFn)(uintptr_t)GetProcAddress(module,"glDrawElements"):NULL;
}
static int readable(uintptr_t p,size_t n){
 if(!p||!n||n>UINTPTR_MAX-p)return 0;
 uintptr_t end=p+n;
 while(p<end){MEMORY_BASIC_INFORMATION m;
  if(!VirtualQuery((void*)p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
    !(m.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return 0;
  uintptr_t z=(uintptr_t)m.BaseAddress+m.RegionSize;if(z<=p)return 0;p=z<end?z:end;
 }return 1;
}
static int safe_copy(void *user,uintptr_t p,void *out,size_t n){
 (void)user;SIZE_T got=0;
 return readable(p,n)&&ReadProcessMemory(GetCurrentProcess(),(void*)p,out,n,&got)&&got==n;
}
/* Per-frame reads of the terrain descriptor handed to the native terrain draw:
   checked once, then plain copies (mxdirt_fast_read.h). Render thread only. */
static MxdFastRead frame_reads;static unsigned frame_reads_age;
static int frame_copy(void *user,uintptr_t p,void *out,size_t n){(void)user;return mxd_fr_read(&frame_reads,p,out,n);}
static int executable(void *p){
 MEMORY_BASIC_INFORMATION m;uintptr_t v=(uintptr_t)p;
 return v>65535&&v!=UINTPTR_MAX&&VirtualQuery(p,&m,sizeof(m))&&m.State==MEM_COMMIT&&
  !(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))&&(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
static uint32_t return_rva(void *ret){
 uintptr_t p=(uintptr_t)ret,b=(uintptr_t)image_base;
 return p>=b&&p-b<image_size?(uint32_t)(p-b):0;
}
static int online_wear; /* the core's experimental [online] online_wear (plugins\mx_dirt.ini): online races too */
static int event_valid(const RenderEvent *e){
 /* A terminated track ID without control characters, quotes or backslashes,
    in a testing session (type 1) - or an online race (type 2) when the core's
    experimental online wear is on. The core decides whether its sim runs;
    every frame still checks the core frame and native terrain identity. */
 char id[100];if(!e||!memchr(e->trackID,0,sizeof(e->trackID)))return 0;memcpy(id,e->trackID,sizeof(id));
 size_t n=strlen(id);if((e->type!=1&&!(e->type==2&&online_wear))||!n)return 0;
 for(size_t k=0;k<n;k++)if((unsigned char)id[k]<32||id[k]=='"'||id[k]=='\\')return 0;
 return 1;
}
/* Succeeds only for an exact current match. Caller owns lifecycle quiescence. */
static int code_replace(uintptr_t address,const unsigned char *expected,const unsigned char *replacement){
 unsigned char bytes[5];if(!safe_copy(NULL,address,bytes,5)||memcmp(bytes,expected,5))return 0;
 DWORD old;if(!VirtualProtect((void*)address,5,PAGE_EXECUTE_READWRITE,&old))return 0;
 memcpy((void*)address,replacement,5);
 if(!FlushInstructionCache(GetCurrentProcess(),(void*)address,5))mark(A_PROTECTION,address,1);
 DWORD ignored;if(!VirtualProtect((void*)address,5,old,&ignored))mark(A_PROTECTION,address,2);
 return 1; /* bytes were changed even if protection restoration reports failure */
}
static int slot_replace(unsigned i,void *expected,void *value){
 void *volatile *slot=(void*volatile*)(image_base+slot_rva[i]);DWORD old;
 if(!VirtualProtect((void*)slot,sizeof(void*),PAGE_READWRITE,&old))return 0;
 void *prior=InterlockedCompareExchangePointer(slot,value,expected);
 DWORD ignored;if(!VirtualProtect((void*)slot,sizeof(void*),old,&ignored))mark(A_PROTECTION,(uintptr_t)slot,3);
 return prior==expected;
}
static NOINLINE int terrain_wrapper(void*,void*,void*,const float*,uint32_t);
static NOINLINE void APIENTRY base_wrapper(GLenum,GLuint,GLuint,GLsizei,GLenum,const void*,GLint);
static NOINLINE void APIENTRY range_wrapper(GLenum,GLuint,GLuint,GLsizei,GLenum,const void*);
static NOINLINE void APIENTRY elements_wrapper(GLenum,GLsizei,GLenum,const void*);
static void *gl_wrapper(unsigned i){return i==0?(void*)base_wrapper:i==1?(void*)range_wrapper:(void*)elements_wrapper;}
static void restore_slots(void){
 for(unsigned i=0;i<3;i++)if(slot_owned[i]){
  if(slot_replace(i,gl_wrapper(i),slot_original[i]))slot_owned[i]=0;
  else {void *now=NULL;safe_copy(NULL,(uintptr_t)image_base+slot_rva[i],&now,8);
   if(now!=gl_wrapper(i)){slot_owned[i]=0;InterlockedExchange(&foreign,1);mark(A_FOREIGN_SLOT,i,(uintptr_t)now);}
  }
 }
 slots_ok=0;
}
static void restore_calls(void){
 InterlockedExchange(&event_allowed,0);InterlockedExchange(&running,0);
 if(GetCurrentThreadId()!=lifecycle_thread||atomic(&inflight)){mark(A_NONQUIESCENT,atomic(&inflight),0);return;}
 if(surface_owned&&code_replace((uintptr_t)image_base+MXDIRT_SURFACE_SITE,surface_patch,mxdirt_surface_original))surface_owned=0;
 if(call_owned){
  if(code_replace(plan.site,plan.patch,plan.original)){call_owned=0;mark(A_ROLLBACK,0,0);}
  else {unsigned char current[5];
   if(safe_copy(NULL,plan.site,current,5)&&memcmp(current,plan.patch,5)){call_owned=0;InterlockedExchange(&foreign,1);mark(A_FOREIGN_CALL,0,0);}
  }
 }
 restore_slots();
 /* The pinned DLL and relay page stay resident even if a restoration fails. */
}
static int install_slots(void){
 if(slots_attempted)return slots_ok;
 if(!wglGetCurrentContext())return 0;
 slots_attempted=1;
 /* The game loads exactly these names; core/EXT aliases may have different
    addresses, and accepting an unrelated alias would hide ownership. */
 const char *names[3]={"glDrawRangeElementsBaseVertex","glDrawRangeElementsEXT","glDrawElements"};
 unsigned i=0;void *p=NULL,*resolved=NULL;uint32_t reason=0;
 for(i=0;i<3;i++){
  p=NULL;resolved=i<2?(void*)wglGetProcAddress(names[i]):(void*)core_elements;
  if(!safe_copy(NULL,(uintptr_t)image_base+slot_rva[i],&p,8))reason=S_READ_FAILED;
  else if(!p&&i<2)reason=S_ABSENT;
  else if(!executable(p))reason=S_NOT_EXECUTABLE;
  else if(p!=resolved)reason=S_RESOLVE_MISMATCH;
  else reason=S_VERIFIED;
  mark_detail(A_SLOT_CHECK,(uintptr_t)p,(uintptr_t)resolved,i,reason);
  if(reason==S_ABSENT)continue; /* absent extension: the game falls through */
  if(reason!=S_VERIFIED)goto fail;
  slot_original[i]=p;
 }
 for(i=0;i<3;i++)if(slot_original[i]){
  if(!slot_replace(i,slot_original[i],gl_wrapper(i))){
   p=NULL;safe_copy(NULL,(uintptr_t)image_base+slot_rva[i],&p,8);
   resolved=slot_original[i];reason=S_REPLACE_FAILED;goto fail;
  }
  slot_owned[i]=1;
 }
 slots_ok=1;mark(A_SLOT_INSTALL,(uintptr_t)slot_original[0],(uintptr_t)slot_original[1]);return 1;
fail:mark_detail(A_SLOT_FAIL,(uintptr_t)p,(uintptr_t)resolved,i,reason);InterlockedExchange(&foreign,1);restore_slots();return 0;
}
/* One page within rel32 reach of the image for the call relay and surface relay. */
static int allocate_relay(void){
 if(relay_page)return 1;
 SYSTEM_INFO s;GetSystemInfo(&s);uintptr_t step=s.dwAllocationGranularity,b=(uintptr_t)image_base;
 for(uintptr_t k=0;k<2048&&!relay_page;k++)for(unsigned side=0;side<2&&!relay_page;side++){
  uintptr_t distance=0x2000000+k*step,p;
  if(side){if(b<distance)continue;p=b-distance;}else{if(b>UINTPTR_MAX-distance)continue;p=b+distance;}
  p&=~(step-1);
  if(p<(uintptr_t)s.lpMinimumApplicationAddress||p>(uintptr_t)s.lpMaximumApplicationAddress-4096)continue;
  relay_page=VirtualAlloc((void*)p,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
 }
 return relay_page!=NULL;
}
static int install_calls(void){
 if(!verified_image||atomic(&foreign)||GetCurrentThreadId()!=lifecycle_thread||atomic(&inflight)||atomic(&running)||call_owned||surface_owned)return 0;
 for(unsigned i=0;i<3;i++)if(slot_owned[i])return 0;
 if(!allocate_relay())return 0;
 MxDirtTsImage image={(uintptr_t)image_base,image_size,1,safe_copy,NULL};
 if(hooks_wanted&&!mxdirt_ts_plan_terrain_call(&image,(uintptr_t)terrain_wrapper,(uintptr_t)relay_page,&plan))return 0;
 /* Every plan exists before any byte in the image changes. */
 DWORD old;if(!VirtualProtect(relay_page,4096,PAGE_READWRITE,&old))return 0;
 if(hooks_wanted&&plan.relay_size)memcpy(relay_page,plan.relay_bytes,plan.relay_size);
 int surface_ready=surface_enabled&&image_size>MXDIRT_SURFACE_RETURN&&
  mxdirt_surface_build((uintptr_t)image_base,(uintptr_t)relay_page+SURFACE_RELAY,relay_page+SURFACE_RELAY,surface_patch);
 if(!VirtualProtect(relay_page,4096,PAGE_EXECUTE_READ,&old)||!FlushInstructionCache(GetCurrentProcess(),relay_page,4096))return 0;
 if(hooks_wanted){
  original_terrain=(MxDirtTsTerrainFn)plan.target;
  if(!code_replace(plan.site,plan.original,plan.patch)){restore_calls();return 0;}
  call_owned=1;
 }
 /* Optional, independent: a byte mismatch (other build/mod) leaves native behaviour. */
 if(surface_ready&&code_replace((uintptr_t)image_base+MXDIRT_SURFACE_SITE,mxdirt_surface_original,surface_patch))surface_owned=1;
 mark(A_INSTALL,call_owned+(surface_owned?100u:0u),(uintptr_t)relay_page);return call_owned||surface_owned;
}
/* Every frame proves our exact bytes/pointers are still in place. Loss of
   ownership disables the pass; a foreign owner is never superseded. */
static int owned_all(void){
 /* perf: the patch site and the slots lie inside the mapped mxbikes.exe image (both were read through
    safe_copy when they were installed) and an image stays mapped for the life of the process, so these
    are plain reads - they used to be 8 system-call reads per frame. */
 if(!call_owned||memcmp((const void*)plan.site,plan.patch,5)){InterlockedExchange(&foreign,1);mark(A_FOREIGN_CALL,0,0);return 0;}
 if(slots_ok)for(unsigned i=0;i<3;i++){void *now=*(void*volatile*)(image_base+slot_rva[i]);
  if(now!=(slot_owned[i]?gl_wrapper(i):slot_original[i])){
   InterlockedExchange(&foreign,1);mark(A_FOREIGN_SLOT,i,(uintptr_t)now);return 0;}}
 return !atomic(&foreign);
}
static int capture_active(void){return atomic(&event_allowed)&&atomic(&run_initialized)&&!atomic(&foreign)&&!generation_fault&&!content_busy;}
static void flush_log(int close){
 if(GetCurrentThreadId()!=lifecycle_thread||atomic(&inflight)||InterlockedCompareExchange(&flush_lock,1,0))return;
 while(audit_flushed<AUDIT_MAX&&atomic(&audit[audit_flushed].ready)){
  const Audit *a=&audit[audit_flushed++];
  mxdirt_content_note("audit %s code=%u thread=%lu tick=%llu a=0x%llx b=0x%llx slot=%u reason=%u",a->code<A_COUNT?audit_names[a->code]:"?",a->code,a->thread,
   (unsigned long long)a->tick,(unsigned long long)a->a,(unsigned long long)a->b,a->slot,a->reason);
 }
 if(close)mxdirt_content_note("summary frames=%llu drawn=%llu skipped=%llu failed=%llu audits=%ld forwarded=%lld,%lld,%lld,%lld call_owned=%u slots=%u,%u,%u surface=%d",
  (unsigned long long)frames_attempted,(unsigned long long)frames_drawn,(unsigned long long)frames_skipped,(unsigned long long)frames_failed,atomic(&audit_reserved),
  InterlockedCompareExchange64(&forwarded[0],0,0),InterlockedCompareExchange64(&forwarded[1],0,0),InterlockedCompareExchange64(&forwarded[2],0,0),InterlockedCompareExchange64(&forwarded[3],0,0),
  call_owned,slot_owned[0],slot_owned[1],slot_owned[2],surface_owned);
 InterlockedExchange(&flush_lock,0);
}
static void settle(void){
 if(GetCurrentThreadId()!=lifecycle_thread||atomic(&inflight)||capture.depth||content_busy)return;
 /* Only a recorded EventDeinit/Shutdown retires hooks; RunStop is never taken
    as proof of native quiescence. */
 if(retire_pending){restore_calls();retire_pending=0;}
 if(shutdown_pending){flush_log(1);shutdown_pending=0;}
 for(uint32_t i=0;i<pending_count;i++){
  content_busy=1;mxdirt_content_lifecycle(pending[i].event,pending[i].generation);content_busy=0;
 }
 pending_count=0;
}
static int lifecycle_owner(void){
 if(GetCurrentThreadId()==lifecycle_thread&&!content_busy)return 1;
 InterlockedExchange(&event_allowed,0);InterlockedExchange(&foreign,1);
 mark(A_NONQUIESCENT,GetCurrentThreadId(),lifecycle_thread);return 0;
}
static void notify_lifecycle(uint32_t event){
 lifecycle_revision++;
 if(pending_count==LIFECYCLE_MAX){InterlockedExchange(&event_allowed,0);InterlockedExchange(&foreign,1);mark(A_OVERFLOW,event,0);return;}
 pending[pending_count++]=(Lifecycle){event,generation};settle();
}
static int bind_generation(uint64_t current,HGLRC ctx,DWORD thread){
 if(!current)return 0;
 if(generation&&generation!=current){generation_fault=1;mark(A_GENERATION,(uintptr_t)generation,(uintptr_t)current);return 0;}
 if(render_context&&render_context!=ctx){generation_fault=1;mark(A_CONTEXT,(uintptr_t)ctx,(uintptr_t)render_context);return 0;}
 if(!generation){
  generation=current;render_thread=thread;render_context=ctx;
  MxDirtTsConfig conf={frame_copy,NULL,generation,thread};
  if(!mxdirt_ts_init(&capture,&conf)){generation_fault=1;return 0;}
 }
 return 1;
}
/* First native terrain draw inside the outer terrain call: its matrices are the
   camera the ground was drawn with. Any other shape fails this frame. */
static float proj_cache[16];static int proj_valid;static unsigned proj_age,undrawn;
static void observe_gl(uint32_t path,void *ret,GLenum mode,GLsizei count,GLenum type){
 if(GetCurrentThreadId()!=render_thread||!capture.depth||content_busy)return;
 if(!camera_active||capture.depth!=1||capture.scope_count!=1){camera_failed=1;return;}
 if(camera_seen||camera_failed)return;
 if(path>2||mode!=GL_TRIANGLES||count<=0||count%3||(type!=GL_UNSIGNED_SHORT&&type!=GL_UNSIGNED_INT)||
    return_rva(ret)!=draw_returns[path][type!=GL_UNSIGNED_SHORT]||wglGetCurrentContext()!=render_context){camera_failed=1;return;}
 /* perf: glGet* drains NVIDIA's threaded driver queue mid-terrain. The modelview IS the view handed to the
    terrain call (verified on every full check); the projection only changes with FOV / resolution. Both
    are read from GL every 30th frame and after any failed frame, otherwise reused. */
 if(!proj_valid||++proj_age>=30u){
  glGetFloatv(GL_MODELVIEW_MATRIX,camera.modelview);glGetFloatv(GL_PROJECTION_MATRIX,proj_cache);proj_valid=1;proj_age=0;
  if(memcmp(camera.modelview,capture.scopes[0].view,64)){camera_failed=1;proj_valid=0;} /* GL modelview == the view handed to the terrain call */
 }else memcpy(camera.modelview,capture.scopes[0].view,64);
 memcpy(camera.projection,proj_cache,64);
 for(unsigned j=0;j<16;j++)if(!isfinite(camera.modelview[j])||!isfinite(camera.projection[j])){camera_failed=1;proj_valid=0;}
 camera_seen=1;
}
static NOINLINE int terrain_wrapper(void *h,void *m,void *v,const float *matrix,uint32_t flags){
 void *ret=NATIVE_RETURN();DWORD error=GetLastError(),thread=GetCurrentThreadId();InterlockedIncrement(&inflight);
 MxDirtTsToken token={0};int outer=0;uint64_t revision=lifecycle_revision;
 if(thread==render_thread&&capture.depth&&!content_busy){
  /* Nested terrain gets a barrier token and fails this frame's camera. */
  token=mxdirt_ts_enter_terrain(&capture,generation,thread,return_rva(ret)-5,(uintptr_t)h,(uintptr_t)m,(uintptr_t)v,(uintptr_t)matrix,flags);
  camera_failed=1;
 }else if(thread==lifecycle_thread&&capture_active()&&return_rva(ret)==TERRAIN_RETURN&&flags==0){
  HGLRC ctx=wglGetCurrentContext();uint64_t active=mxdirt_content_generation();
  if(++frame_reads_age>=MXD_FR_FRAMES){mxd_fr_expire(&frame_reads);frame_reads_age=0;}
  if(ctx&&bind_generation(active,ctx,thread)&&owned_all()&&install_slots()&&mxdirt_ts_next_capture(&capture,thread)){
   token=mxdirt_ts_enter_terrain(&capture,generation,thread,MXDIRT_TS_TERRAIN_CALL,(uintptr_t)h,(uintptr_t)m,(uintptr_t)v,(uintptr_t)matrix,flags);
   if(token.serial){outer=1;memset(&camera,0,sizeof(camera));camera_active=1;camera_seen=camera_failed=0;
    camera.generation=token.generation;camera.serial=token.serial;camera.context=ctx;camera.thread=thread;}
  }
 }
 SetLastError(error);int result=original_terrain(h,m,v,matrix,flags);error=GetLastError();
 int left=token.serial?mxdirt_ts_leave(&capture,thread,token):0;
 if(outer){
  camera_active=0;
  camera.complete=left&&camera_seen&&!camera_failed&&!capture.poisoned&&capture.scope_count==1&&!capture.stats.rejected_entries;
  int eligible=camera.complete&&revision==lifecycle_revision&&capture_active()&&
   wglGetCurrentContext()==render_context&&mxdirt_content_generation()==generation&&owned_all();
  int drawn=0;
  if(eligible){content_busy=1;drawn=mxdirt_content_draw(&camera,&capture.scopes[0]);content_busy=0;}
  frames_attempted++;if(drawn>0)frames_drawn++;else if(drawn<0)frames_failed++;else frames_skipped++;
  /* Re-read the camera from GL after a frame that did not draw - but only for the first two in a row: a pass
     that stays ruled out (see the hold in mxdirt_gl_draw) must not cost two glGet* on every frame. */
  if(drawn>0)undrawn=0;else if(++undrawn<=2)proj_valid=0;
 }
 InterlockedIncrement64(&forwarded[0]);InterlockedDecrement(&inflight);settle();SetLastError(error);return result;
}
static NOINLINE void APIENTRY base_wrapper(GLenum m,GLuint a,GLuint z,GLsizei n,GLenum t,const void *p,GLint b){
 void *ret=NATIVE_RETURN();DWORD error=GetLastError();InterlockedIncrement(&inflight);
 observe_gl(0,ret,m,n,t);SetLastError(error);((BaseFn)slot_original[0])(m,a,z,n,t,p,b);error=GetLastError();
 InterlockedIncrement64(&forwarded[1]);InterlockedDecrement(&inflight);settle();SetLastError(error);
}
static NOINLINE void APIENTRY range_wrapper(GLenum m,GLuint a,GLuint z,GLsizei n,GLenum t,const void *p){
 void *ret=NATIVE_RETURN();DWORD error=GetLastError();InterlockedIncrement(&inflight);
 observe_gl(1,ret,m,n,t);SetLastError(error);((RangeFn)slot_original[1])(m,a,z,n,t,p);error=GetLastError();
 InterlockedIncrement64(&forwarded[2]);InterlockedDecrement(&inflight);settle();SetLastError(error);
}
static NOINLINE void APIENTRY elements_wrapper(GLenum m,GLsizei n,GLenum t,const void *p){
 void *ret=NATIVE_RETURN();DWORD error=GetLastError();InterlockedIncrement(&inflight);
 observe_gl(2,ret,m,n,t);SetLastError(error);((ElementsFn)slot_original[2])(m,n,t,p);error=GetLastError();
 InterlockedIncrement64(&forwarded[3]);InterlockedDecrement(&inflight);settle();SetLastError(error);
}
/* SHA-256 of the one supported mxbikes.exe (SUPPORTED_EXE.txt). */
static int executable_hash(const wchar_t *path){
 static const unsigned char expected[32]={0x9e,0x79,0x2d,0x7a,0xed,0x1d,0xc4,0x0d,0xf1,0x1e,0xea,0x51,0xd1,0xa6,0xd0,0x51,
  0x9d,0xae,0xc7,0xbe,0x2b,0x65,0x60,0x99,0x05,0xdf,0x8c,0x9d,0xe8,0x04,0xa2,0x91};
 HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
 if(f==INVALID_HANDLE_VALUE)return 0;
 BCRYPT_ALG_HANDLE algorithm=NULL;BCRYPT_HASH_HANDLE hash=NULL;unsigned char result[32];static unsigned char bytes[65536];int ok=0;DWORD got;
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,NULL,0)<0||BCryptCreateHash(algorithm,&hash,NULL,0,NULL,0,0)<0)goto done;
 for(;;){if(!ReadFile(f,bytes,sizeof(bytes),&got,NULL))goto done;if(!got)break;if(BCryptHashData(hash,bytes,got,0)<0)goto done;}
 if(BCryptFinishHash(hash,result,sizeof(result),0)>=0&&!memcmp(result,expected,32))ok=1;
done:if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);CloseHandle(f);return ok;
}

/* ---- MX Bikes plugin interface ---- */
API char *GetModID(void){return "mxbikes";}
API int GetModDataVersion(void){return 8;}
API int GetInterfaceVersion(void){return 9;}
static int configuration(void){return GetPrivateProfileIntW(L"renderer",L"enabled",0,config_path)==1;}
static int startup_fail(DWORD error){if(content_ready)mxdirt_content_lifecycle(MXDIRT_SHUTDOWN,0);content_ready=0;SetLastError(error);return -1;}
API int Startup(char *savepath){
 (void)savepath;DWORD error=GetLastError();if(lifecycle_thread){SetLastError(error);return -1;}
 lifecycle_thread=GetCurrentThreadId();image_base=(unsigned char*)GetModuleHandleW(NULL);
 core_elements=resolve_core_elements();HMODULE pinned=NULL;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)(uintptr_t)terrain_wrapper,&pinned)){SetLastError(error);return -1;}
 static wchar_t path[32768];DWORD n=GetModuleFileNameW(NULL,path,32768);
 if(!n||n>=32768){SetLastError(error);return -1;}
 static wchar_t dir[32768];memcpy(dir,path,sizeof(wchar_t)*(n+1));
 wchar_t *slash=wcsrchr(dir,L'\\');if(!slash){SetLastError(error);return -1;}slash[1]=0;if(wcslen(dir)+64>=32768){SetLastError(error);return -1;}
 swprintf(config_path,32768,L"%lsplugins\\mx_dirt_render.ini",dir);
 {static wchar_t core_ini[32768];swprintf(core_ini,32768,L"%lsplugins\\mx_dirt.ini",dir);online_wear=GetPrivateProfileIntW(L"online",L"online_wear",0,core_ini)==1;}
 content_busy=1;content_ready=mxdirt_content_startup(config_path)==1;content_busy=0;
 if(!content_ready){SetLastError(error);return -1;}
 verified_image=executable_hash(path);
 IMAGE_DOS_HEADER dos;IMAGE_NT_HEADERS64 nt;
 if(!safe_copy(NULL,(uintptr_t)image_base,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||dos.e_lfanew>0x100000||
    !safe_copy(NULL,(uintptr_t)image_base+(uint32_t)dos.e_lfanew,&nt,sizeof(nt))||nt.Signature!=IMAGE_NT_SIGNATURE||
    nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return startup_fail(error);
 image_size=nt.OptionalHeader.SizeOfImage;
 mark(verified_image?A_START:A_HASH_FAIL,(uintptr_t)image_base,image_size);
 surface_enabled=GetPrivateProfileIntW(L"renderer",L"raised_dirt_surface",1,config_path)!=0; /* 0 = native dig-only layer exposure */
 hooks_wanted=mxdirt_content_dirt_enabled();
 if(!verified_image||(!hooks_wanted&&!surface_enabled)){mark(A_GATE_FAIL,(uintptr_t)verified_image,(uintptr_t)hooks_wanted);flush_log(0);return startup_fail(error);}
 flush_log(0);SetLastError(error);return 3;
}
API void EventInit(void *data,int size){
 DWORD error=GetLastError();
 if(!lifecycle_owner()){SetLastError(error);return;}
 if(atomic(&inflight)||atomic(&running)||capture.depth){
  /* Loading during an active native frame cannot establish a new safe
     generation. Keep the wrappers until an explicit retirement arrives. */
  InterlockedExchange(&event_allowed,0);generation_fault=1;lifecycle_revision++;
  mark(A_NONQUIESCENT,1,0);SetLastError(error);return;
 }
 /* SDK loading boundary. A prior event must have explicitly retired first. */
 if(atomic(&event_allowed)||retire_pending||call_owned||surface_owned){mark(A_GATE_FAIL,1,0);SetLastError(error);return;}
 for(unsigned i=0;i<3;i++)if(slot_owned[i]){mark(A_GATE_FAIL,3,i);SetLastError(error);return;}
 RenderEvent e;
 if(!content_ready||!verified_image||!data||size<(int)sizeof(e)||atomic(&foreign)||!configuration()||
    !safe_copy(NULL,(uintptr_t)data,&e,sizeof(e))||!event_valid(&e)){mark(A_GATE_FAIL,4,0);flush_log(0);SetLastError(error);return;}
 generation=0;generation_fault=0;render_thread=0;render_context=NULL;mxd_fr_expire(&frame_reads);frame_reads_age=0;
 memset(&capture,0,sizeof(capture));memset(&camera,0,sizeof(camera));camera_active=camera_seen=camera_failed=0;
 slots_attempted=slots_ok=0;memset(slot_original,0,sizeof(slot_original));run_initialized=running=0;
 if(install_calls()){InterlockedExchange(&event_allowed,1);notify_lifecycle(MXDIRT_EVENT_BEGIN);}else mark(A_INSTALL_FAIL,0,0);
 flush_log(0);SetLastError(error);
}
API void RunInit(void *data,int size){
 (void)data;(void)size;DWORD error=GetLastError();if(lifecycle_owner()&&atomic(&event_allowed)){
  InterlockedExchange(&run_initialized,1);notify_lifecycle(MXDIRT_RUN_INIT);}SetLastError(error);
}
API void RunStart(void){DWORD error=GetLastError();if(lifecycle_owner()&&atomic(&event_allowed)&&atomic(&run_initialized)){
 InterlockedExchange(&running,1);notify_lifecycle(MXDIRT_RUN_START);}SetLastError(error);}
API void RunStop(void){DWORD error=GetLastError();if(lifecycle_owner()){
 InterlockedExchange(&running,0);notify_lifecycle(MXDIRT_RUN_STOP);flush_log(0);}SetLastError(error);}
API void RunDeinit(void){DWORD error=GetLastError();if(lifecycle_owner()){
 InterlockedExchange(&running,0);InterlockedExchange(&run_initialized,0);notify_lifecycle(MXDIRT_RUN_DEINIT);flush_log(0);}SetLastError(error);}
API void RunTelemetry(void *data,int size,float time,float pos){(void)data;(void)size;(void)time;(void)pos;
 DWORD error=GetLastError();settle();flush_log(0);SetLastError(error);}
API void EventDeinit(void){DWORD error=GetLastError();if(lifecycle_owner()){
 InterlockedExchange(&running,0);InterlockedExchange(&run_initialized,0);InterlockedExchange(&event_allowed,0);
 retire_pending=1;notify_lifecycle(MXDIRT_EVENT_RETIRE);flush_log(0);}SetLastError(error);}
API void Shutdown(void){DWORD error=GetLastError();if(lifecycle_owner()){
 InterlockedExchange(&running,0);InterlockedExchange(&run_initialized,0);InterlockedExchange(&event_allowed,0);
 retire_pending=shutdown_pending=1;mark(A_SHUTDOWN,0,0);notify_lifecycle(MXDIRT_SHUTDOWN);}SetLastError(error);}
