/* MX Dirt core plugin (mx_dirt.dlo) for MX Bikes, one supported mxbikes.exe
   (SHA-256 pinned below; every engine offset in this plugin belongs to it).
   A CPU dirt simulation driven by the bike's own wheel contacts publishes its
   heights into MX Bikes' own terrain deformation grid:
     wheel hook (every physics step) -> mxdirt_contact -> mxdirt_session
     (bump stress, dirt sim, live gate, ghosts, pre-wear) -> DFM targets ->
     MxdirtTransport (exclusive native publication, MX Bikes' tyre digging off).
   Setup/patch transactions run only at verified lifecycle or startup
   receive-return ownership gaps, never inside an ordinary running callback.
   Exports the MX Bikes plugin API plus MxDirtBorrowFrame (src/shared/mxdirt_frame.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <intrin.h>
#include "mxdirt_frame_snapshot.h"
#include "mxdirt_game_path.h"
#include "mxdirt_wheel_sample.h"
#include "mxdirt_session.h"
#include "mxdirt_transport.h"
#include "mxdirt_runstop_origin.h"
#include "mxdirt_bootstrap_origin.h"
#include "mxdirt_patches.h"
#include "mxdirt_frame.h"

#define API __declspec(dllexport)
#define RA_RECORDS 32768u
#define RA_CELLS (ra_geom_valid?(size_t)ra_geom.width*ra_geom.height:(size_t)2049*2049)
#define RA_LAPS_DIR "mx_dirt_laps"
#define RA_OLD_LAPS_DIR "mxb_reflex_ghosts"
#define RA_TERRAIN_DIR "mx_dirt_terrain"
/* Height range of the sim's encoded heights (1 = +-0.61 m, 1.5 = +-0.91 m; the
   published DFM stays capped to the 1.0 range). 1.5 = the tuned releases. */
#define RA_HEIGHT_RESPONSE 1.5f
typedef struct {
 char rider[100],bikeID[100],bikeName[100];
 int gears,maxRPM,limiter,shiftRPM;float temperature,alarm[2],fuel,susp[2],lock;
 char category[100],trackID[100],trackName[100];float length;int type;
} RaEvent;
_Static_assert(sizeof(RaEvent)==652,"version8 event ABI");
typedef int (*RaWheelFn)(void *,int,float);
typedef int (*RaOuterFn)(int);
typedef int (*RaCloseFn)(void);
typedef struct {
 uint64_t base,current,published;uint32_t width,height;
 float x,z,scale,origin[3];
} RaGrid;
/* Terrain geometry of the loaded track (both native terrain copies agree). */
typedef struct {uint32_t width,height;float span_x,span_z,vertical_scale,origin[3];} RaGeometry;
typedef struct {
 unsigned char *site,*relay,original[5],patch[5];int owned;
} RaHook;
enum { RA_EVENT=1,RA_SETUP,RA_BIND,RA_BEGIN,RA_FORCE,RA_FRAME,RA_REJECT,
 RA_RESET,RA_STOP,RA_HOOK,RA_RESTORE,RA_BUDGET,RA_TRANSPORT,RA_PUBLISH,RA_RETAINED,RA_CLOSE,RA_PATCHES,RA_BOOTSTRAP };
typedef struct {
 volatile LONG ready;int kind,code,wheel,elapsed,steps,flags[4];
 DWORD thread;uint64_t order,outer,physics[2],bike,generation;
 LONGLONG qpc;double solver_ms;float dt,force,center[2][3],velocity[2][3],omega[2];
 size_t sampled,changed,clipped;
} RaRecord;
static const char ra_exe_sha[]="9e792d7aed1dc40df11eea51d1a6d0519daec7be2b65609905df8c9de804a291";
static const unsigned char ra_wheel_call[]={0xe8,0xef,0xb8,0xff,0xff};
static const unsigned char ra_wheel_entry[]={0xf3,0x0f,0x11,0x54,0x24,0x18,0x4c,0x8b,0xdc,0x49,0x89,0x4b,0x08,0x56,0x57,0x41,0x55,0x48,0x81,0xec,0x70,0x03,0,0};
/* Client/host track-close calls and FNV-1a hashes of their targets. */
static const unsigned char ra_close_calls[2][5]={{0xe8,0xe1,0xcc,0x17,0},{0xe8,0xc1,0x79,0x17,0}};
static const unsigned ra_close_sites[2]={0x1270ba,0x126bda};
static const unsigned ra_close_targets[2]={0x2a3da0,0x29e5a0};
static const unsigned ra_close_lengths[2]={156,520};
static const uint64_t ra_close_hashes[2]={UINT64_C(0xce1c6bd9f8393b5b),UINT64_C(0x2cb761237dcc3184)};
static unsigned char *ra_image;static uint64_t ra_image_size;
static RaWheelFn ra_original_wheel;static RaOuterFn ra_original_outer;
static RaCloseFn ra_original_close[2];static RaHook ra_hooks[4];static MxbFrameCallsitePlan ra_plan;
static MxbFrameSnapshotState ra_snapshot;static MxdirtSession *ra_session;
static MxdirtSessionConfig ra_config;static RaGrid ra_grid;
static RaGeometry ra_geom;static int ra_geom_valid;
/* [soil] settings; code defaults are the tuned values (see mx_dirt.ini). */
static unsigned ra_dirt_sim_depth=60,ra_dirt_sim_firmness=55,ra_dirt_sim_moisture=35;
static unsigned ra_bumps_percent=100,ra_rolling_bumps_percent=60;
static unsigned ra_hardness_percent=100,ra_breakdown_percent=100,ra_drying_percent=100,ra_sweep_percent=100;
static unsigned ra_extra_riders=0,ra_live_gate=1,ra_live_wear=0;
/* Online wear (EXPERIMENTAL, [online] online_wear=1): active in online race events too, and the other riders wear the track
   (mxdirt_remote.h). ra_online: this event is a race. No pre-wear, ghosts, lap recording or saved soil / marks there. */
static unsigned ra_online_wear=0,ra_online_echo=0;static int ra_online;
/* sharp_ruts (vertex-exact publication) is opt-in: the native mesh's fixed
   triangle split turns narrow ruts running along one diagonal into teeth. */
static unsigned ra_sharp_ruts=0,ra_capture_percent=100,ra_cohesion_percent=100,ra_clods_percent=100;static int ra_sharp_live;
/* Pre-wear gate (0 = off): virtual riders on the recorded laps before the first step. */
static unsigned ra_prewear_riders=50,ra_prewear_spread_cm=140,ra_prewear_clip_cm=400,ra_prewear_fresh_only=1;
static unsigned ra_soft_mm=85; /* soft top layer, display only (0 = off) */
#define RA_SOFT_DIR "mx_dirt_soft"
static char ra_verified_hash[65];
static DWORD ra_owner;static uint64_t ra_generation,ra_bike,ra_candidate,ra_physics[2];
static volatile LONG ra_allowed,ra_running,ra_inflight,ra_outer_depth,ra_reset_token;
static volatile LONG ra_new_run,ra_foreign;static LONG ra_applied_token;
static int ra_frame_token,ra_phase,ra_bad,ra_rebind,ra_have_position[2],ra_has_geometry[2];
static int ra_candidate_mask,ra_candidate_conflict,ra_fatal,ra_setup_attempted,ra_enabled;
static uint64_t ra_surface_frames[16]; /* wheel-frames per MX surface material */
static int ra_detail_armed,ra_budget_done;static uint64_t ra_solver_frames,ra_solver_slow;static double ra_solver_total_ms,ra_solver_max_ms,ra_solver_late_max_ms;
static MxbWheelKinematics ra_last_position[2];static MxbWheelSampleMetadata ra_meta[2];
static volatile LONG ra_reserved;static LONG ra_flushed;static volatile LONG64 ra_order;
static RaRecord ra_records[RA_RECORDS];static FILE *ra_log;static char ra_ini[MAX_PATH];
/* First fatal only; immutable until a validated, quiescent new event. */
typedef struct {
 volatile LONG ready;int flushed,reason,owner_match,phase,bad_before,rebind;
 DWORD thread,owner;LONGLONG qpc;
 uint64_t generation,bike,solver_frames,publish_offers,publish_accepted;
 uint64_t snapshot_generation,outer,next_outer,physics[2];
 int snapshot_initialized,snapshot_in_call,snapshot_busy,snapshot_fault;
 int snapshot_needs_rebind,snapshot_paused,snapshot_has_published,elapsed_ms;
 int session_present,transport_present,setup_attempted,frame_token;
 LONG reset_token,inflight,outer_depth,close_depth,records;
} RaFirstFault;
static RaFirstFault ra_first_fault;
static MxdirtTransport *ra_transport;static unsigned char *ra_transport_code;
static MxbTransportConfig ra_transport_config;
static int ra_event_valid,ra_pinned,ra_activation_requested=1,ra_stopped_gap;
/* Independent startup ownership, never a stopped flag or a ready transport. */
static RaHook ra_bootstrap_hooks[2];static RaCloseFn ra_bootstrap_original_receive;
static MxbNativeExcavation ra_bootstrap_original_excavation;
static volatile LONG ra_bootstrap_depth,ra_bootstrap_dig_depth,ra_bootstrap_cancel;
static int ra_bootstrap_phase,ra_bootstrap_gap,ra_bootstrap_requested;
static uint64_t ra_bootstrap_generation,ra_bootstrap_gap_generation,ra_bootstrap_elapsed,ra_bootstrap_clock,ra_bootstrap_attempt_at;
static LONG ra_bootstrap_gap_token;static uint64_t ra_bootstrap_calls,ra_bootstrap_attempts,ra_bootstrap_suppressed;
static int ra_bootstrap_arm(void),ra_bootstrap_remove(void),ra_bootstrap_owned(void);
static void ra_bootstrap_abort(int);
static int ra_engine_gap(void);
static int ra_read(void *,uint64_t,void *,size_t);
/* In-game "Deformation" setting. MX Bikes copies the testing slider, the race-setup
   value or the server's value into the current event at exe+E55BFC (float,
   slider 0..100% -> 0..1, 100..200% -> 1..20; MX default 1). Track conditions
   sit at exe+E55C14. */
#define RA_GAME_DEFORMATION_RVA 0xe55bfcu
#define RA_GAME_TRACK_CONDITIONS_RVA 0xe55c14u
static unsigned ra_follow_game_deformation=1,ra_game_deformation_reference=25;
static float ra_game_factor=1.f; /* this session's Deformation slider factor (1 at the reference), also scales the soft layer */
static int ra_game_deform_off;
static uint64_t ra_publish_offers,ra_publish_deferred,ra_publish_accepted;
static unsigned ra_close_mask;static uint64_t ra_close_generation;
static int ra_native_retired;static volatile LONG ra_close_depth;
static unsigned ra_dump_terrain;
static char ra_event_track[100];
/* [soil] sand_tracks: comma list of case-insensitive track-name fragments whose
   soil the DIRT SIM treats as sand (MX's own tyre grip still uses its material). */
static char ra_sand_tracks[512];
static int ra_track_sand;

static int ra_engine_gap(void){
 if(GetCurrentThreadId()!=ra_owner)return 0;
 if(ra_stopped_gap)return !ra_running&&!ra_inflight;
 return !ra_fatal&&!ra_foreign&&ra_bootstrap_gap&&ra_bootstrap_depth==1&&ra_inflight==1&&!ra_bootstrap_dig_depth&&
  !ra_outer_depth&&!ra_close_depth&&ra_running&&ra_event_valid&&!ra_bootstrap_cancel&&
  ra_bootstrap_gap_generation==ra_generation&&ra_bootstrap_gap_token==ra_reset_token&&
  mxb_bootstrap_immediate(ra_read,NULL,(uintptr_t)ra_image);
}
/* Ownership-gap facts come only from validated stopped lifecycle callbacks or
   the authenticated startup receive return. Pointer validity is not ownership. */
static MxbTransportFacts ra_facts(int transport_quiescent,int old_track_retired) {
 MxbTransportFacts f={ra_generation,ra_owner,ra_event_valid,ra_event_valid,
  ra_pinned,ra_engine_gap(),transport_quiescent,old_track_retired,
  old_track_retired&&ra_native_retired?3u:0u};return f;
}
static int ra_span(const void *source,size_t bytes,int writable,void *user) {
 (void)user;if(!source||!bytes||bytes>UINTPTR_MAX-(uintptr_t)source)return 0;
 uintptr_t p=(uintptr_t)source,end=p+bytes;
 while(p<end){MEMORY_BASIC_INFORMATION m;
  if(!VirtualQuery((void*)p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return 0;
  DWORD a=m.Protect&0xff;
  if(writable){if(a!=PAGE_READWRITE&&a!=PAGE_WRITECOPY&&a!=PAGE_EXECUTE_READWRITE&&a!=PAGE_EXECUTE_WRITECOPY)return 0;}
  else if(a!=PAGE_READONLY&&a!=PAGE_READWRITE&&a!=PAGE_WRITECOPY&&a!=PAGE_EXECUTE_READ&&a!=PAGE_EXECUTE_READWRITE&&a!=PAGE_EXECUTE_WRITECOPY)return 0;
  uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=p)return 0;p=next;
 }return 1;
}
static unsigned char *ra_near_memory(void) {
 for(uintptr_t off=0x2000000;off<0x70000000;off+=0x10000){
  unsigned char *p=VirtualAlloc((void*)((uintptr_t)ra_image+off),4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);
  if(p)return p;
 }return NULL;
}
static int ra_transport_ready(void) {
 MxbTransportState s;
 return ra_transport&&mxb_transport_state(ra_transport,&s)&&!s.faulted&&
  s.phase==MXB_TRANSPORT_ACTIVE&&s.exclusive&&s.pace_bits==31;
}
static int ra_read(void *user,uint64_t source,void *dest,size_t bytes) {
 (void)user;if(!source||!dest||!bytes||bytes>UINTPTR_MAX-source)return 0;
 uintptr_t p=(uintptr_t)source,end=p+bytes;
 while(p<end){MEMORY_BASIC_INFORMATION m;
  if(!VirtualQuery((void *)p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return 0;
  DWORD a=m.Protect&0xff;
  if(a!=PAGE_READONLY&&a!=PAGE_READWRITE&&a!=PAGE_WRITECOPY&&a!=PAGE_EXECUTE_READ&&a!=PAGE_EXECUTE_READWRITE&&a!=PAGE_EXECUTE_WRITECOPY)return 0;
  uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=p)return 0;p=next;
 }
 memcpy(dest,(const void *)(uintptr_t)source,bytes);return 1;
}
/* Dirt-sim depth for this session from the in-game Deformation setting.
   The INI depth applies as-is at the reference slider value; other values
   scale it by sqrt(scale/reference) (half the slider -> ~0.7x, double ->
   ~1.4x), capped at 3x and the sim's 10..300% range. Slider 0 = no new
   deformation, like native MX. Unreadable/odd value -> INI depth unchanged. */
static float ra_game_depth(void){
 float scale=-1.f,cond=-1.f,factor=1.f,depth=(float)ra_dirt_sim_depth;ra_game_deform_off=0;ra_game_factor=1.f;
 int have=ra_follow_game_deformation&&ra_image&&RA_GAME_TRACK_CONDITIONS_RVA+4<=ra_image_size&&
  ra_read(NULL,(uint64_t)(uintptr_t)ra_image+RA_GAME_DEFORMATION_RVA,&scale,4)&&isfinite(scale)&&scale>=0.f&&scale<=20.f;
 if(ra_image&&RA_GAME_TRACK_CONDITIONS_RVA+4<=ra_image_size&&ra_read(NULL,(uint64_t)(uintptr_t)ra_image+RA_GAME_TRACK_CONDITIONS_RVA,&cond,4)&&!isfinite(cond))cond=-1.f;
 if(have){
  if(scale<=0.f){ra_game_deform_off=1;factor=0.f;ra_game_factor=0.f;}
  else{float r=(float)ra_game_deformation_reference,ref=r<=100.f?r/100.f:1.f+(r-100.f)*19.f/100.f;
   factor=sqrtf(scale/ref);if(factor>3.f)factor=3.f;
   depth*=factor;if(depth<10.f)depth=10.f;if(depth>300.f)depth=300.f;ra_game_factor=factor;}
 }
 if(ra_log){fprintf(ra_log,"{\"kind\":\"game_deformation\",\"follow\":%u,\"read\":%s,\"scale\":%.6g,\"slider_percent\":%.4g,\"reference_percent\":%u,\"factor\":%.6g,\"ini_depth_percent\":%u,\"effective_depth_percent\":%.6g,\"deformation_off\":%s,\"track_conditions\":%.6g}\n",
  ra_follow_game_deformation,have?"true":"false",(double)scale,have?(double)(scale<=1.f?scale*100.f:100.f+(scale-1.f)*100.f/19.f):-1.0,ra_game_deformation_reference,(double)factor,ra_dirt_sim_depth,(double)depth,ra_game_deform_off?"true":"false",(double)cond);fflush(ra_log);}
 return depth;
}
static RaRecord *ra_record(int kind,int code) {
 if(InterlockedCompareExchange(&ra_reserved,0,0)>=(LONG)RA_RECORDS)return NULL;
 LONG i=InterlockedIncrement(&ra_reserved)-1;if(i<0||(unsigned)i>=RA_RECORDS)return NULL;
 RaRecord *r=&ra_records[i];r->kind=kind;r->code=code;r->thread=GetCurrentThreadId();
 r->order=(uint64_t)InterlockedIncrement64(&ra_order);r->bike=ra_bike;r->generation=ra_generation;
 LARGE_INTEGER t;QueryPerformanceCounter(&t);r->qpc=t.QuadPart;return r;
}
static void ra_done(RaRecord *r){if(r)InterlockedExchange(&r->ready,1);}
static void ra_mark(int kind,int code){ra_done(ra_record(kind,code));}
static void ra_latch_first_fault(int reason) {
 DWORD incoming=GetLastError();
 if(InterlockedCompareExchange(&ra_first_fault.ready,-1,0)!=0){SetLastError(incoming);return;}
 RaFirstFault *f=&ra_first_fault;LARGE_INTEGER now;QueryPerformanceCounter(&now);
 f->reason=reason;f->thread=GetCurrentThreadId();f->owner=ra_owner;f->owner_match=f->thread==f->owner;f->qpc=now.QuadPart;
 f->reset_token=InterlockedCompareExchange(&ra_reset_token,0,0);f->inflight=InterlockedCompareExchange(&ra_inflight,0,0);
 f->outer_depth=InterlockedCompareExchange(&ra_outer_depth,0,0);f->close_depth=InterlockedCompareExchange(&ra_close_depth,0,0);
 f->records=InterlockedCompareExchange(&ra_reserved,0,0);
 /* Only the serialized owner may copy non-atomic controller/snapshot state. */
 if(f->owner_match){
  f->generation=ra_generation;f->bike=ra_bike;f->solver_frames=ra_solver_frames;
  f->publish_offers=ra_publish_offers;f->publish_accepted=ra_publish_accepted;
  f->phase=ra_phase;f->bad_before=ra_bad;f->rebind=ra_rebind;f->frame_token=ra_frame_token;
  f->session_present=ra_session!=NULL;f->transport_present=ra_transport!=NULL;f->setup_attempted=ra_setup_attempted;
  f->snapshot_generation=ra_snapshot.config.generation;f->outer=ra_snapshot.published.sequence;f->next_outer=ra_snapshot.next_sequence;
  f->physics[0]=ra_physics[0];f->physics[1]=ra_physics[1];
  f->snapshot_initialized=ra_snapshot.initialized;f->snapshot_in_call=ra_snapshot.in_call;f->snapshot_busy=ra_snapshot.busy;
  f->snapshot_fault=ra_snapshot.fault;f->snapshot_needs_rebind=ra_snapshot.needs_rebind;f->snapshot_paused=ra_snapshot.paused;
  f->snapshot_has_published=ra_snapshot.has_published;f->elapsed_ms=ra_snapshot.elapsed_ms;
 }
 InterlockedExchange(&f->ready,1);SetLastError(incoming);
}
static const char *ra_first_fault_stage(int reason) {
 switch(reason){
 case MXB_FRAME_IDENTITY:return "owner_identity";
 case -107:return "session_step";case -108:return "grid_binding";case -110:return "snapshot_reprime";
 case -111:return "bike_conflict";case -121:return "transport_resume";case -122:return "session_publish";
 case -123:return "transport_health";case -124:return "hook_install";case -125:return "transport_activate";
 case -126:return "retained_event";case -127:return "engine_patches_missing";case -129:return "bootstrap_restore";
 case -130:return "setup_failed";case -131:return "close_provenance";default:return "unclassified";
 }
}
static void ra_flush_first_fault(void) {
 if(!ra_log||GetCurrentThreadId()!=ra_owner||InterlockedCompareExchange(&ra_inflight,0,0)||
    InterlockedCompareExchange(&ra_first_fault.ready,0,0)!=1||ra_first_fault.flushed)return;
 const RaFirstFault *f=&ra_first_fault;
 int n=fprintf(ra_log,"{\"active\":true,\"kind\":\"first_fatal\",\"code\":%d,\"stage\":\"%s\",\"qpc\":%lld,\"thread\":%lu,\"owner\":%lu,\"owner_match\":%d,\"generation\":%llu,\"bike\":%llu,\"solver_frames\":%llu,\"publish_offers\":%llu,\"publish_accepted\":%llu,\"snapshot_generation\":%llu,\"outer\":%llu,\"next_outer\":%llu,\"physics\":[%llu,%llu],\"phase\":%d,\"bad_before\":%d,\"rebind\":%d,\"snapshot_initialized\":%d,\"snapshot_in_call\":%d,\"snapshot_busy\":%d,\"snapshot_fault\":%d,\"snapshot_needs_rebind\":%d,\"snapshot_paused\":%d,\"snapshot_has_published\":%d,\"elapsed_ms\":%d,\"session_present\":%d,\"transport_present\":%d,\"setup_attempted\":%d,\"frame_token\":%d,\"reset_token\":%ld,\"inflight\":%ld,\"outer_depth\":%ld,\"close_depth\":%ld,\"detail_records_at_fault\":%ld}\n",
  f->reason,ra_first_fault_stage(f->reason),(long long)f->qpc,f->thread,f->owner,f->owner_match,
  (unsigned long long)f->generation,(unsigned long long)f->bike,(unsigned long long)f->solver_frames,
  (unsigned long long)f->publish_offers,(unsigned long long)f->publish_accepted,(unsigned long long)f->snapshot_generation,
  (unsigned long long)f->outer,(unsigned long long)f->next_outer,(unsigned long long)f->physics[0],(unsigned long long)f->physics[1],
  f->phase,f->bad_before,f->rebind,f->snapshot_initialized,f->snapshot_in_call,f->snapshot_busy,f->snapshot_fault,
  f->snapshot_needs_rebind,f->snapshot_paused,f->snapshot_has_published,f->elapsed_ms,f->session_present,f->transport_present,
  f->setup_attempted,f->frame_token,f->reset_token,f->inflight,f->outer_depth,f->close_depth,f->records);
 if(n>=0&&fflush(ra_log)==0)ra_first_fault.flushed=1;
}
/* Exact transport fault cause, captured at the -123 health check. */
static MxbTransportState ra_transport_fault_state;static int ra_transport_fault_have,ra_transport_fault_got;
/* Self-heal (mxb_transport_recover). Burst budget: up to 24 banked recoveries,
   +1 per 60 owner outer frames. An in-game fault with sends in flight costs ~6
   (fault + late echoes); a fault that recurs every frame drains the bank in ~24
   frames and shuts down. */
#define RA_TRANSPORT_RECOVERY_BURST 24
#define RA_TRANSPORT_RECOVERY_REFILL 60
static unsigned ra_transport_recoveries,ra_transport_recovery_failures,ra_transport_recovery_budget=RA_TRANSPORT_RECOVERY_BURST,ra_transport_recovery_clock;
static void ra_flush(void) {
 /* Only the lifecycle owner at a quiescent callback. No physics-thread I/O. */
 if(!ra_log||GetCurrentThreadId()!=ra_owner||InterlockedCompareExchange(&ra_inflight,0,0))return;
 ra_flush_first_fault();
 if(ra_transport_recoveries||ra_transport_recovery_failures||mxb_transport_foreign_tiles(ra_transport))fprintf(ra_log,"{\"active\":true,\"kind\":\"transport_recovery\",\"generation\":%llu,\"recoveries\":%u,\"failures\":%u,\"budget\":%u,\"foreign_tiles_adopted\":%llu}\n",(unsigned long long)ra_generation,ra_transport_recoveries,ra_transport_recovery_failures,ra_transport_recovery_budget,(unsigned long long)mxb_transport_foreign_tiles(ra_transport));
 if(ra_transport_fault_have==1){const MxbTransportState *t=&ra_transport_fault_state;ra_transport_fault_have=2;
  fprintf(ra_log,"{\"active\":true,\"kind\":\"transport_fault\",\"state_read\":%d,\"phase\":%d,\"faulted\":%d,\"pace_bits\":%d,\"last_result\":%d,\"fault_line\":%d,\"fault_why\":%d,\"fault_detail\":%u,\"hook_fail_line\":%d,\"hook_result\":%d,\"ledger_fault_line\":%d,\"queued\":%llu,\"outstanding\":%llu,\"accepted_sequence\":%llu,\"issued_sequence\":%llu,\"sent_batches\":%llu,\"echo_batches\":%llu}\n",
   ra_transport_fault_got,(int)t->phase,t->faulted,t->pace_bits,(int)t->last_result,t->fault_line,t->fault_why,t->fault_detail,t->hook_fail_line,t->hook_result,t->ledger_fault_line,
   (unsigned long long)t->queued_targets,(unsigned long long)t->outstanding_events,(unsigned long long)t->accepted_sequence,(unsigned long long)t->locally_issued_sequence,
   (unsigned long long)t->sent_batches,(unsigned long long)t->echo_batches);}
 while((unsigned)ra_flushed<RA_RECORDS&&InterlockedCompareExchange(&ra_records[ra_flushed].ready,0,0)){
  RaRecord *r=&ra_records[ra_flushed++];
  fprintf(ra_log,"{\"active\":true,\"kind\":%d,\"code\":%d,\"order\":%llu,\"qpc\":%lld,\"thread\":%lu,\"generation\":%llu,\"bike\":%llu,\"outer\":%llu,\"physics\":[%llu,%llu],\"wheel\":%d,\"elapsed_ms\":%d,\"steps\":%d,\"dt\":%.9g,\"solver_ms\":%.6f,\"force_newtons\":%.9g,\"flags\":[%d,%d,%d,%d],\"centers\":[[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g]],\"velocities\":[[%.9g,%.9g,%.9g],[%.9g,%.9g,%.9g]],\"spin\":[%.9g,%.9g],\"sampled\":%zu,\"changed_from_initial\":%zu,\"clipped\":%zu}\n",
   r->kind,r->code,(unsigned long long)r->order,(long long)r->qpc,r->thread,
   (unsigned long long)r->generation,(unsigned long long)r->bike,(unsigned long long)r->outer,
   (unsigned long long)r->physics[0],(unsigned long long)r->physics[1],r->wheel,r->elapsed,r->steps,r->dt,r->solver_ms,r->force,
   r->flags[0],r->flags[1],r->flags[2],r->flags[3],r->center[0][0],r->center[0][1],r->center[0][2],r->center[1][0],r->center[1][1],r->center[1][2],
   r->velocity[0][0],r->velocity[0][1],r->velocity[0][2],r->velocity[1][0],r->velocity[1][1],r->velocity[1][2],
   r->omega[0],r->omega[1],r->sampled,r->changed,r->clipped);
 }
 fprintf(ra_log,"{\"active\":true,\"kind\":\"counters\",\"solver_frames\":%llu,\"solver_total_ms\":%.6f,\"solver_max_ms\":%.6f,\"ride_max_ms\":%.3f,\"ride_over_2ms\":%llu,\"detail_armed\":%d,\"budget_done\":%d,\"records\":%ld}\n",
  (unsigned long long)ra_solver_frames,ra_solver_total_ms,ra_solver_max_ms,ra_solver_late_max_ms,(unsigned long long)ra_solver_slow,ra_detail_armed,ra_budget_done,ra_reserved);
 fprintf(ra_log,"{\"active\":true,\"kind\":\"surface_materials\",\"wheel_frames\":{\"soil\":%llu,\"soft\":%llu,\"compact\":%llu,\"sand\":%llu,\"gravel\":%llu,\"other\":%llu}}\n",
  (unsigned long long)(ra_surface_frames[4]+ra_surface_frames[7]),(unsigned long long)ra_surface_frames[10],(unsigned long long)ra_surface_frames[11],(unsigned long long)ra_surface_frames[5],(unsigned long long)ra_surface_frames[12],
  (unsigned long long)(ra_surface_frames[0]+ra_surface_frames[1]+ra_surface_frames[2]+ra_surface_frames[3]+ra_surface_frames[6]+ra_surface_frames[8]+ra_surface_frames[9]+ra_surface_frames[13]+ra_surface_frames[14]+ra_surface_frames[15]));
 {uint64_t frames=0;double total[3],peak[3];
  if(ra_session&&mxdirt_session_step_timing(ra_session,&frames,total,peak)&&frames)
   fprintf(ra_log,"{\"active\":true,\"kind\":\"step_timing\",\"frames\":%llu,\"mean_ms\":{\"contacts\":%.4f,\"dirt_sim\":%.4f,\"height_convert\":%.4f},\"max_ms\":{\"contacts\":%.3f,\"dirt_sim\":%.3f,\"height_convert\":%.3f}}\n",
    (unsigned long long)frames,total[0]/frames,total[1]/frames,total[2]/frames,peak[0],peak[1],peak[2]);}
 {double st[6];if(ra_session&&mxdirt_session_dirt_sim_stats(ra_session,st))
   {MxdirtRemoteState rs;LARGE_INTEGER c,f;QueryPerformanceCounter(&c);QueryPerformanceFrequency(&f);
    if(f.QuadPart>0&&mxdirt_session_remote_state(ra_session,(double)c.QuadPart/(double)f.QuadPart,&rs))
     fprintf(ra_log,"{\"active\":true,\"kind\":\"online_riders\",\"experimental\":true,\"calibrated\":%s,\"layout\":%d,\"own_race_number\":%d,\"riders\":%d,\"passes\":%llu,\"position_batches\":%llu,\"calibrations\":%llu,\"reference_ahead_m\":%.3f,\"reference_height_m\":%.3f}\n",
      rs.calibrated?"true":"false",rs.map,rs.self_num,rs.riders,(unsigned long long)rs.passes,(unsigned long long)rs.batches,(unsigned long long)rs.locks,(double)rs.ref_off,(double)rs.y_off);}
   fprintf(ra_log,"{\"active\":true,\"kind\":\"dirt_sim\",\"volume_error_m3\":%.6g,\"cut_m3\":%.6g,\"compacted_m3\":%.6g,\"roost_m3\":%.6g,\"dust_m3\":%.6g,\"relax_moves\":%.0f,\"ghost_passes\":%llu}\n",
    st[0],st[1],st[2],st[3],st[4],st[5],(unsigned long long)mxdirt_session_ghost_passes(ra_session));}
 MxbTransportState s;if(ra_transport&&mxb_transport_state(ra_transport,&s))
  fprintf(ra_log,"{\"active\":true,\"kind\":\"transport\",\"phase\":%d,\"faulted\":%d,\"exclusive\":%d,\"pace_bits\":%d,\"owned_patches\":%u,\"queued_targets\":%zu,\"outstanding_events\":%zu,\"accepted_sequence\":%llu,\"issued_sequence\":%llu,\"sent_batches\":%llu,\"echo_batches\":%llu,\"suppressed_excavations\":%llu,\"offers\":%llu,\"deferred\":%llu,\"accepted_offers\":%llu,\"trace_dropped\":%zu}\n",
   s.phase,s.faulted,s.exclusive,s.pace_bits,s.owned_patch_mask,s.queued_targets,s.outstanding_events,
   (unsigned long long)s.accepted_sequence,(unsigned long long)s.locally_issued_sequence,
   (unsigned long long)s.sent_batches,(unsigned long long)s.echo_batches,(unsigned long long)s.suppressed_excavations,
   (unsigned long long)ra_publish_offers,(unsigned long long)ra_publish_deferred,(unsigned long long)ra_publish_accepted,s.trace_dropped);
 fprintf(ra_log,"{\"active\":true,\"kind\":\"startup_bootstrap\",\"phase\":%d,\"cancelled\":%ld,\"receive_owned\":%d,\"excavation_owned\":%d,\"elapsed_running_ms\":%llu,\"receive_calls\":%llu,\"full_attempts\":%llu,\"suppressed_new_excavations\":%llu}\n",
  ra_bootstrap_phase,ra_bootstrap_cancel,ra_bootstrap_hooks[0].owned,ra_bootstrap_hooks[1].owned,
  (unsigned long long)ra_bootstrap_elapsed,(unsigned long long)ra_bootstrap_calls,(unsigned long long)ra_bootstrap_attempts,(unsigned long long)ra_bootstrap_suppressed);
 fflush(ra_log);
}
static int ra_hash_file(const char *path,char hex[65]) {
 FILE *f=fopen(path,"rb");HCRYPTPROV provider=0;HCRYPTHASH hash=0;int ok=0;
 unsigned char buffer[65536],digest[32];size_t total=0,n;DWORD len=32;
 if(!f||!CryptAcquireContextA(&provider,NULL,NULL,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)||
    !CryptCreateHash(provider,CALG_SHA_256,0,0,&hash))goto end;
 while((n=fread(buffer,1,sizeof(buffer),f))!=0){
  if(total>256u*1024u*1024u-n||!CryptHashData(hash,buffer,(DWORD)n,0))goto end;total+=n;
 }
 if(ferror(f)||!CryptGetHashParam(hash,HP_HASHVAL,digest,&len,0)||len!=32)goto end;
 for(unsigned j=0;j<32;j++)snprintf(hex+j*2,3,"%02x",digest[j]);hex[64]=0;ok=1;
end:if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);if(f)fclose(f);return ok;
}
/* ---- Track terrain: MX Bikes keeps two terrain objects (client exe+568488,
   host exe+56e5e8); each holds width/height at +740, int16 physics base at +748,
   render heights at +750, span x/z, vertical scale and origin at +758..+76f,
   and the published deformation (int32 DFM) at +7a8. */
static int ra_sand_match(const char *track){
 const char *p=ra_sand_tracks;
 while(*p){
  while(*p==','||*p==' ')p++;
  const char *e=p;while(*e&&*e!=',')e++;
  size_t n=(size_t)(e-p);while(n&&p[n-1]==' ')n--;
  if(n)for(const char *t=track;*t;t++){size_t k=0;while(k<n&&t[k]&&tolower((unsigned char)t[k])==tolower((unsigned char)p[k]))k++;if(k==n)return 1;}
  p=e;
 }
 return 0;
}
/* Wheel material as the dirt sim sees it: soil on a sand track becomes sand. */
static int ra_sim_material(int m){return ra_track_sand&&(m==4||m==7||m==10||m==11)?5:m;}
static int ra_grid_raw(unsigned side,RaGrid *out){
 unsigned char raw[0x70];RaGrid g;memset(&g,0,sizeof(g));
 if(side>1||!ra_read(NULL,(uint64_t)(uintptr_t)ra_image+0x568488+side*0x6160+0x740,raw,sizeof(raw)))return 0;
 memcpy(&g.width,raw,4);memcpy(&g.height,raw+4,4);memcpy(&g.base,raw+8,8);memcpy(&g.current,raw+0x10,8);
 memcpy(&g.x,raw+0x18,4);memcpy(&g.z,raw+0x1c,4);memcpy(&g.scale,raw+0x20,4);
 memcpy(g.origin,raw+0x24,12);memcpy(&g.published,raw+0x68,8);*out=g;return 1;
}
static int ra_geometry_valid(const RaGeometry *g){
 if(!g||g->width<64||g->height<64||g->width>8193||g->height>8193||
    (uint64_t)g->width*g->height>34000000||!isfinite(g->span_x)||!isfinite(g->span_z)||
    g->span_x<1.f||g->span_z<1.f||g->span_x>1e7f||g->span_z>1e7f||
    !isfinite(g->vertical_scale)||g->vertical_scale<1e-6f||g->vertical_scale>1e6f)return 0;
 for(unsigned k=0;k<3;k++)if(!isfinite(g->origin[k])||fabsf(g->origin[k])>1e8f)return 0;
 return isfinite(g->origin[0]+g->span_x)&&isfinite(g->origin[2]+g->span_z)&&
  g->origin[0]+g->span_x>g->origin[0]&&g->origin[2]+g->span_z>g->origin[2];
}
static RaGeometry ra_geometry_of(const RaGrid *g){
 RaGeometry r={g->width,g->height,g->x,g->z,g->scale,{g->origin[0],g->origin[1],g->origin[2]}};return r;
}
static int ra_grid_read(RaGrid *out) {
 RaGrid g;if(!ra_geom_valid||!ra_grid_raw(0,&g))return 0;
 RaGeometry now=ra_geometry_of(&g);
 if(memcmp(&now,&ra_geom,sizeof(now))||!g.base||!g.current||!g.published)return 0;
 *out=g;return 1;
}
static int ra_track_select_event(const char *name){
 if(!name)return 0;
 size_t n=strlen(name);if(!n||n>=sizeof(ra_event_track))return 0;
 for(size_t k=0;k<n;k++)if((unsigned char)name[k]<32||name[k]=='"'||name[k]=='\\')return 0;
 ra_geom_valid=0;memset(&ra_geom,0,sizeof(ra_geom));
 memcpy(ra_event_track,name,n+1);ra_track_sand=ra_sand_match(ra_event_track);return 1;
}
/* Loading can change buffers after EventInit. Adopt the terrain geometry only at
   the RunInit / qualified receive boundary, when both native terrain copies hold
   the same geometry and the same int16 physics base. */
static int ra_track_adopt_loaded(void){
 if(!ra_engine_gap()||ra_session||ra_transport)return 0;
 if(ra_geom_valid)return 1;
 RaGrid g[2];if(!ra_grid_raw(0,g)||!ra_grid_raw(1,g+1))return 0;
 RaGeometry a=ra_geometry_of(g),b=ra_geometry_of(g+1);
 if(!ra_geometry_valid(&a)||memcmp(&a,&b,sizeof(a))||!g[0].base||!g[1].base)return 0;
 size_t cells=(size_t)a.width*a.height;
 if(!ra_span((const void*)(uintptr_t)g[0].base,cells*2,0,NULL)||!ra_span((const void*)(uintptr_t)g[1].base,cells*2,0,NULL)||
    memcmp((const void*)(uintptr_t)g[0].base,(const void*)(uintptr_t)g[1].base,cells*2)||!ra_engine_gap())return 0;
 ra_geom=a;ra_geom_valid=1;
 if(ra_log){fprintf(ra_log,"{\"kind\":\"track\",\"track\":\"%s\",\"width\":%u,\"height\":%u,\"span_x\":%.9g,\"span_z\":%.9g,\"vertical_scale\":%.9g,\"sand\":%s,\"both_native_copies_verified\":true}\n",
  ra_event_track,a.width,a.height,(double)a.span_x,(double)a.span_z,(double)a.vertical_scale,ra_track_sand?"true":"false");fflush(ra_log);}
 return 1;
}
/* Session geometry: the sim is 2048-square over the whole terrain; native units
   are feet (.3048 m) relative to the terrain centre. */
static int ra_session_config(uint64_t generation,MxdirtSessionConfig *out){
 if(!out||!generation||!ra_geom_valid)return 0;
 const RaGeometry *g=&ra_geom;MxdirtSessionConfig c;memset(&c,0,sizeof(c));
 c.contact.origin_metres[0]=g->origin[0]+g->span_x*.5f;
 c.contact.origin_metres[1]=g->origin[1];
 c.contact.origin_metres[2]=g->origin[2]+g->span_z*.5f;
 c.contact.native_length_per_metre=(float)(1./.3048);c.contact.generation=generation;
 c.height=(MxdirtHeightConfig){2048,2048,2048,g->width,g->height,
  (float)((double)g->span_x/.3048),(float)((double)g->span_z/.3048),2.f,128.f/255.f,
  .3048,(double)g->vertical_scale,generation};
 size_t cells=(size_t)g->width*g->height;c.sparse_capacity=cells<65536?cells:65536;*out=c;return 1;
}
/* One-shot terrain capture for offline analysis ([debug] dump_terrain=1): the
   loaded track's physics base (int16), render heights (uint16) and published
   DFM (int32, metres = -v*vertical_scale/(65535*65536)) plus geometry, once per
   track name, to plugins\mx_dirt_terrain\<track>.mxt. Read-only on the game. */
static void ra_terrain_dump(const RaGrid *g,const int16_t *base,const int32_t *dfm,double vertical_scale,double metres){
 if(!ra_dump_terrain||!ra_event_track[0])return;
 char rel[MAX_PATH]="plugins\\" RA_TERRAIN_DIR,path[MAX_PATH],track[100];size_t k=0;
 if(!mxb_game_path_a(rel,(DWORD)strlen(rel),sizeof(rel)))return;CreateDirectoryA(rel,NULL);
 for(const char *c=ra_event_track;*c&&k<sizeof(track)-1;c++)track[k++]=(strchr("\\/:*?\"<>|",*c)?'_':*c);track[k]=0;
 if(!k||snprintf(path,sizeof(path),"%s\\%s.mxt",rel,track)>=(int)sizeof(path)||GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES)return;
 uint16_t *cur=malloc(RA_CELLS*2);if(!cur)return;
 if(ra_read(NULL,g->current,cur,RA_CELLS*2)){FILE *f=fopen(path,"wb");
  if(f){fwrite("MXT1",1,4,f);fwrite(&g->width,4,1,f);fwrite(&g->height,4,1,f);fwrite(&g->x,4,1,f);fwrite(&g->z,4,1,f);fwrite(&g->scale,4,1,f);fwrite(g->origin,4,3,f);
   fwrite(&vertical_scale,8,1,f);fwrite(&metres,8,1,f);fwrite(base,2,RA_CELLS,f);fwrite(cur,2,RA_CELLS,f);fwrite(dfm,4,RA_CELLS,f);fclose(f);
   if(ra_log){fprintf(ra_log,"{\"active\":true,\"kind\":\"terrain_dump\",\"track\":\"%s\"}\n",track);fflush(ra_log);}}}
 free(cur);
}
static void ra_ghost_free(void);
static void ra_soft_save(void);
static void ra_soil_save(void);
static void ra_solver_destroy(void){
 if(!ra_online){
 ra_soft_save(); /* the track keeps its soft-layer marks with its saved wear */
 ra_soil_save(); /* and the dirt sim's packing, loose soil, moisture and prints */
 }
 mxdirt_session_destroy(ra_session);ra_session=NULL;ra_ghost_free(); /* laps were borrowed by that session */
}
#include "mxdirt_laps.inc"
/* plugins\mx_dirt_soft\<track>\layer.msl */
static int ra_soft_path(char *out,size_t cap,int create){
 char dir[MAX_PATH];return ra_lap_dir(RA_SOFT_DIR,dir,sizeof(dir),create)&&snprintf(out,cap,"%s\\layer.msl",dir)<(int)cap;
}
static void ra_soft_save(void){
 MxdirtSoftView v;char path[MAX_PATH];if(!ra_session||!mxdirt_session_soft_view(ra_session,&v)||(!v.stamps&&!v.loaded)||!ra_soft_path(path,sizeof(path),1))return;
 LARGE_INTEGER a,b,f;QueryPerformanceCounter(&a);int ok=mxdirt_session_soft_save(ra_session,path);QueryPerformanceCounter(&b);QueryPerformanceFrequency(&f);
 if(ra_log){fprintf(ra_log,"{\"kind\":\"soft_layer_save\",\"ok\":%d,\"tiles\":%u,\"refused\":%u,\"stamps\":%llu,\"ms\":%.0f}\n",ok,v.allocated,v.refused,(unsigned long long)v.stamps,(double)(b.QuadPart-a.QuadPart)*1000./(double)f.QuadPart);fflush(ra_log);}
}
/* plugins\mx_dirt_soft\<track>\soil.mss: the dirt sim's soil state (MX Bikes saves only heights) */
static int ra_soil_path(char *out,size_t cap,int create){
 char dir[MAX_PATH];return ra_lap_dir(RA_SOFT_DIR,dir,sizeof(dir),create)&&snprintf(out,cap,"%s\\soil.mss",dir)<(int)cap;
}
static void ra_soil_save(void){
 char path[MAX_PATH];if(!ra_session||!ra_soil_path(path,sizeof(path),1))return;
 uint64_t wear=mxdirt_session_saved_wear(ra_session,.015);if(wear<=4000)return; /* nothing worn worth keeping (a reset track saves no soil) */
 LARGE_INTEGER a,b,f;QueryPerformanceCounter(&a);int ok=mxdirt_session_soil_save(ra_session,path,wear);QueryPerformanceCounter(&b);QueryPerformanceFrequency(&f);
 if(ra_log){fprintf(ra_log,"{\"kind\":\"soil_state_save\",\"ok\":%d,\"wear\":%llu,\"ms\":%.0f}\n",ok,(unsigned long long)wear,(double)(b.QuadPart-a.QuadPart)*1000./(double)f.QuadPart);fflush(ra_log);}
}
static void ra_soil_setup(uint64_t saved_wear){
 char path[MAX_PATH];int tiles=0;
 if(ra_session&&saved_wear>4000&&ra_soil_path(path,sizeof(path),0))tiles=mxdirt_session_soil_load(ra_session,path,saved_wear);
 if(ra_log){fprintf(ra_log,"{\"kind\":\"soil_state\",\"saved_wear\":%llu,\"loaded_tiles\":%d}\n",(unsigned long long)saved_wear,tiles);fflush(ra_log);}
}
/* At setup, before pre-wear: a track with saved wear gets its saved marks back. */
static void ra_soft_setup(uint64_t saved_wear){
 if(!ra_session||!ra_soft_mm)return;
 /* the in-game Deformation slider scales the soft layer like the ruts (slider 0: no marks, no layer) */
 if(ra_game_deform_off)return;
 float t=(float)(ra_soft_mm<5?5:ra_soft_mm)/1000.f*ra_game_factor;if(t<.005f)t=.005f;if(t>.10f)t=.10f;
 if(!mxdirt_session_set_soft_layer(ra_session,t)){if(ra_log){fprintf(ra_log,"{\"kind\":\"soft_layer\",\"ok\":0}\n");fflush(ra_log);}return;}
 char path[MAX_PATH];int loaded=saved_wear>4000&&ra_soft_path(path,sizeof(path),0)&&mxdirt_session_soft_load(ra_session,path);
 MxdirtSoftView v;memset(&v,0,sizeof(v));mxdirt_session_soft_view(ra_session,&v);
 if(ra_log){fprintf(ra_log,"{\"kind\":\"soft_layer\",\"ok\":1,\"thickness_mm\":%u,\"scaled_mm\":%.1f,\"loaded\":%d,\"tiles\":%u}\n",ra_soft_mm,(double)(t*1000.f),loaded,v.allocated);fflush(ra_log);}
}
static int ra_setup(void) {
 if(ra_session)return 1;
 if(ra_setup_attempted||!ra_engine_gap())return 0;
 RaGrid first,last;if(!ra_grid_read(&first))return 0; /* loading: may retry later */
 LARGE_INTEGER start,end,freq;QueryPerformanceCounter(&start);QueryPerformanceFrequency(&freq);
 ra_setup_attempted=1;int16_t *base=malloc(RA_CELLS*2);int32_t *dfm=malloc(RA_CELLS*4);int ok=0;
 const char *stage="read_grid"; /* logged on failure: which setup step refused */
 if(!base||!dfm||!ra_read(NULL,first.base,base,RA_CELLS*2)||!ra_read(NULL,first.published,dfm,RA_CELLS*4)||
    !ra_grid_read(&last)||memcmp(&first,&last,sizeof(first)))goto end;
 MxbTransportFacts seed_facts=ra_facts(1,0);
 stage="transport_initial";if(!mxb_transport_matches_initial(ra_transport,&seed_facts,base,dfm,RA_CELLS))goto end;
 stage="track_config";if(!ra_session_config(ra_generation,&ra_config))goto end;
 ra_terrain_dump(&first,base,dfm,ra_config.height.mxb_vertical_scale,ra_config.height.native_metres);
 stage="session_create";
 if(!mxdirt_session_create(&ra_session,&ra_config,base,dfm,RA_CELLS))goto end;
 stage="depth_response";
 if(!mxdirt_session_set_depth_response(ra_session,RA_HEIGHT_RESPONSE)){
  /* A track may already contain deeper saved ruts than the extended range
     envelope. Preserve that saved surface and use the default range instead. */
  if(!mxdirt_session_set_depth_response(ra_session,1.f)){ra_solver_destroy();goto end;}
  if(ra_log){fprintf(ra_log,"{\"kind\":\"height_range_fallback\",\"response_percent\":100,\"reason\":\"saved_surface_outside_extended_range\",\"saved_terrain_preserved\":true}\n");fflush(ra_log);}
 }
 stage="session_settings";
 /* Sharp ruts are optional per track: an unusual grid keeps blended publication. */
 ra_sharp_live=ra_sharp_ruts&&mxdirt_session_set_sharp_ruts(ra_session,1);
 if((ra_bumps_percent&&(!mxdirt_session_set_bumps(ra_session,(float)ra_bumps_percent)||!mxdirt_session_set_rolling_bumps(ra_session,(float)ra_rolling_bumps_percent)))||
    !mxdirt_session_start_sim(ra_session,ra_game_depth(),(float)ra_dirt_sim_firmness)||
    !mxdirt_session_set_dirt_moisture(ra_session,(float)ra_dirt_sim_moisture)||
    !mxdirt_session_set_dirt_response(ra_session,(float)ra_hardness_percent,(float)ra_breakdown_percent)||
    !mxdirt_session_set_dirt_weather(ra_session,(float)ra_drying_percent,(float)ra_sweep_percent)||
    !mxdirt_session_set_dirt_shape(ra_session,(float)ra_capture_percent,(float)ra_cohesion_percent,(float)ra_clods_percent)||
    !mxdirt_session_set_extra_riders(ra_session,ra_extra_riders)||!mxdirt_session_set_live_gate(ra_session,ra_live_gate)||!mxdirt_session_set_live_wear(ra_session,(float)ra_live_wear)){
  ra_solver_destroy();goto end;
 }
 stage="grid_changed";
 if(!ra_grid_read(&last)||memcmp(&first,&last,sizeof(first))){ra_solver_destroy();goto end;}
 ra_grid=first;ra_rebind=1;ok=1;
 {uint64_t sw=ra_online?0:mxdirt_session_saved_wear(ra_session,.015); /* an online race never loads this PC's saved soil or marks */
  if(!ra_online)ra_soil_setup(sw); /* optional: the saved packing / loose soil / moisture / prints of a ridden track */
  ra_soft_setup(ra_soft_mm?sw:0);} /* optional, display only */
 if(!ra_online)ra_ghost_load(); /* optional: no laps or a bad file never fails setup. Online: no pre-wear, no ghosts (every player has other laps) */
 if(ra_online||ra_online_echo){int on=mxdirt_session_set_remote(ra_session,1,ra_sim_material(7));
  if(ra_log){fprintf(ra_log,"{\"kind\":\"online_wear\",\"experimental\":true,\"race_event\":%s,\"remote_riders\":%s,\"echo_test\":%u}\n",ra_online?"true":"false",on?"true":"false",ra_online_echo);fflush(ra_log);}}
 if(ra_log){fprintf(ra_log,"{\"active\":true,\"kind\":\"rut_shape\",\"sharp_ruts\":%d,\"requested\":%u,\"capture_percent\":%u,\"cohesion_percent\":%u,\"clods_percent\":%u}\n",ra_sharp_live,ra_sharp_ruts,ra_capture_percent,ra_cohesion_percent,ra_clods_percent);fflush(ra_log);}
end:if(!ok)ra_solver_destroy();free(base);free(dfm);QueryPerformanceCounter(&end);RaRecord *r=ra_record(RA_SETUP,ok);
 if(r){r->solver_ms=(double)(end.QuadPart-start.QuadPart)*1000./freq.QuadPart;ra_done(r);}
 if(!ok&&ra_log){fprintf(ra_log,"{\"active\":false,\"kind\":\"setup_failed\",\"stage\":\"%s\"}\n",stage);fflush(ra_log);}
 if(!ok){ra_latch_first_fault(-130);ra_fatal=1;}return ok;
}
static int ra_same_grid_check(void){RaGrid now;return ra_grid_read(&now)&&!memcmp(&now,&ra_grid,sizeof(now));}
static MxbFrameGate ra_gate(void) {
 MxbFrameGate g={ra_generation,ra_bike,GetCurrentThreadId(),1,
  InterlockedCompareExchange(&ra_allowed,0,0)!=0,InterlockedCompareExchange(&ra_running,0,0)!=0};return g;
}
static void ra_reject(int reason){ra_bad=reason;ra_rebind=1;ra_mark(RA_REJECT,reason);}
static void ra_fault(int reason) {
 ra_latch_first_fault(reason);ra_fatal=1;ra_reject(reason);
 if(ra_transport)mxb_transport_pause(ra_transport,GetCurrentThreadId());
}
static int ra_write_code(void *,const void *,size_t);
static int ra_wheel(void *,int,float);
static __declspec(noinline) int ra_outer(int);
static __declspec(noinline) int ra_client_close(void),ra_host_close(void);
static int ra_bootstrap_receive(void);
static int ra_bootstrap_excavation(void *,float,float,double);
static int ra_reprime(void) {
 MxdirtSessionState s;if(!ra_session||!ra_bike||!mxdirt_session_state(ra_session,&s)||s.faulted||
  !mxdirt_session_reprime(ra_session))return 0;
 MxbFrameSnapshotConfig c={ra_read,NULL,ra_plan,ra_generation,ra_bike,ra_owner,s.outer_sequence+1,100};
 if(mxb_frame_snapshot_init(&ra_snapshot,&c)!=MXB_FRAME_OK)return 0;
 memset(ra_have_position,0,sizeof(ra_have_position));memset(ra_has_geometry,0,sizeof(ra_has_geometry));
 ra_rebind=0;ra_bad=0;ra_mark(RA_RESET,1);return 1;
}
static int ra_jump(const MxbWheelKinematics *a,const MxbWheelKinematics *b,float dt) {
 /* Attachment guard: reject a displacement larger than .5m plus the faster
    endpoint speed times elapsed physics dt. */
 double d=0,va=0,vb=0;for(int k=0;k<3;k++){double q=(double)a->center[k]-b->center[k];d+=q*q;va+=(double)a->linear_world[k]*a->linear_world[k];vb+=(double)b->linear_world[k]*b->linear_world[k];}
 double limit=.5+sqrt(va>vb?va:vb)*dt;return !isfinite(d)||d>limit*limit;
}
/* Wraps MX Bikes' per-physics-step wheel-force call (exe+1a76bc -> 1a2fb0). */
static int ra_wheel(void *bike,int wheel,float dt) {
 DWORD incoming=GetLastError();unsigned incoming_csr=_mm_getcsr();InterlockedIncrement(&ra_inflight);
 _mm_setcsr(incoming_csr);SetLastError(incoming);
 int result=ra_original_wheel(bike,wheel,dt);DWORD outgoing=GetLastError();unsigned outgoing_csr=_mm_getcsr();
 if(GetCurrentThreadId()!=ra_owner)InterlockedExchange(&ra_foreign,1);
 if(InterlockedCompareExchange(&ra_allowed,0,0)&&InterlockedCompareExchange(&ra_running,0,0)&&!ra_fatal&&
    GetCurrentThreadId()==ra_owner&&ra_outer_depth==1&&wheel>=0&&wheel<2){
  MxbWheelSampleRequest q={(uint64_t)(uintptr_t)ra_image,(uint64_t)(uintptr_t)bike,ra_generation,ra_physics[wheel]+1,wheel,result,1,dt};
  MxdirtPhysicsSample p;MxbWheelSampleMetadata m;
  if(!mxb_wheel_sample(ra_read,NULL,&q,&p,&m)){
   if(ra_phase&&q.bike==ra_bike)ra_reject(-101);
  }else if(!ra_bike){
   if(!ra_candidate)ra_candidate=q.bike;
   if(ra_candidate!=q.bike)ra_candidate_conflict=1;else ra_candidate_mask|=1<<wheel;
  }else if(q.bike!=ra_bike){ra_reject(-102);
  }else if(ra_phase&&!ra_bad&&ra_frame_token==ra_reset_token){
   if(ra_have_position[wheel]&&ra_jump(&ra_last_position[wheel],&p.body,dt))ra_reject(-103);
   else if(!mxdirt_session_physics(ra_session,wheel,&p))ra_reject(-104);
   else {MxbFrameGate g=ra_gate();int code=mxb_frame_snapshot_note(&ra_snapshot,&g,wheel,m.body,&p);
    if(code!=MXB_FRAME_OK)ra_reject(code);
    else {ra_physics[wheel]=p.sequence;ra_meta[wheel]=m;ra_last_position[wheel]=p.body;ra_have_position[wheel]=1;
     if(p.active)ra_has_geometry[wheel]=1;
     if(!ra_detail_armed&&p.active&&p.body.linear_world[0]*p.body.linear_world[0]+p.body.linear_world[2]*p.body.linear_world[2]>.25f){ra_detail_armed=1;ra_mark(RA_BIND,2);}
     RaRecord *r=ra_detail_armed?ra_record(RA_FORCE,result):NULL;if(r){r->wheel=wheel;r->dt=dt;r->force=p.normal_force_newtons;
      r->outer=mxb_frame_snapshot_upcoming_sequence(&ra_snapshot);r->physics[wheel]=p.sequence;
      memcpy(r->center[wheel],p.body.center,12);memcpy(r->velocity[wheel],p.body.linear_world,12);
      float angular[3];mxb_wheel_angular_local(angular,&p.body);r->omega[wheel]=angular[0];
      r->flags[0]=m.current;r->flags[1]=m.previous;r->flags[2]=m.eligible;r->flags[3]=m.material;ra_done(r);}
    }
   }
  }
 }
 InterlockedDecrement(&ra_inflight);_mm_setcsr(outgoing_csr);SetLastError(outgoing);return result;
}
static void ra_settings_live(void);
static void ra_complete_frame(int elapsed) {
 {static unsigned poll;if(!(++poll&63u))ra_settings_live();} /* about twice a second: one file-time query */
 MxbFrameSnapshot f;MxbFrameGate g=ra_gate();int result=mxb_frame_snapshot_finish(&ra_snapshot,&g,&f);
 if(result!=MXB_FRAME_OK&&result!=MXB_FRAME_NO_NEW_STEP){if(result!=MXB_FRAME_WAITING)ra_reject(result);return;}
 if(!ra_has_geometry[0]||!ra_has_geometry[1]){ra_mark(RA_FRAME,MXB_FRAME_WAITING);return;}
 for(int w=0;w<2;w++)if(ra_jump(&ra_last_position[w],&f.postintegration[w],.002f)){ra_reject(-105);return;}
 MxdirtControllerFrame frames[2];memset(frames,0,sizeof(frames));
 for(int w=0;w<2;w++){
  if(ra_meta[w].generation!=f.generation||ra_meta[w].sequence!=f.last_preintegration[w].sequence||
     ra_meta[w].body!=f.body_address[w]||ra_meta[w].availability.normal_contact!=f.last_preintegration[w].active){ra_reject(-128);return;}
  frames[w].terrain_span_native=ra_config.height.native_span_x;
  frames[w].qualified_contact=ra_meta[w].availability.heightfield_qualified;
  /* Deformable MX Bikes surfaces: soil 4/7, sand 5, soft 10, compact 11, gravel 12. */
  int material=ra_meta[w].material;
  frames[w].deformation_enabled=material==4||material==5||material==7||material==10||material==11||material==12;
  if(ra_game_deform_off)frames[w].deformation_enabled=0; /* in-game Deformation 0 */
  if(mxb_frame_snapshot_controller_values(&f,w,&frames[w])!=MXB_FRAME_OK){ra_reject(-106);return;}
 }
 MxdirtHeightStats stats;LARGE_INTEGER start,end,freq;
 QueryPerformanceCounter(&start);QueryPerformanceFrequency(&freq);
 const int soil_material[2]={ra_sim_material(ra_meta[0].material),ra_sim_material(ra_meta[1].material)};
 for(int w=0;w<2;w++){int m=ra_meta[w].material;ra_surface_frames[m>=0&&m<16?m:15]++;}
 if(!mxdirt_session_step(ra_session,frames,soil_material,&stats)){ra_fault(-107);return;}
 if(!ra_online)ra_ghost_record_frame((float)f.dt);
 QueryPerformanceCounter(&end);double ms=(double)(end.QuadPart-start.QuadPart)*1000./freq.QuadPart;
 ra_solver_frames++;ra_solver_total_ms+=ms;if(ms>ra_solver_max_ms)ra_solver_max_ms=ms;
 /* the first frames carry the pre-wear's one big height update; these two say what a ride itself costs */
 if(ra_solver_frames>600){if(ms>2.)ra_solver_slow++;if(ms>ra_solver_late_max_ms)ra_solver_late_max_ms=ms;}
 for(int w=0;w<2;w++)ra_last_position[w]=f.postintegration[w];
 RaRecord *r=(ra_detail_armed||f.sequence<=3||f.sequence%60==0)?ra_record(RA_FRAME,result):NULL;
 if(r){r->outer=f.sequence;r->elapsed=elapsed;r->steps=(int)f.new_steps;r->dt=f.dt;r->solver_ms=ms;
  r->sampled=stats.sampled;r->changed=stats.changed_from_initial;r->clipped=stats.clipped;
  for(int w=0;w<2;w++){r->physics[w]=f.last_preintegration[w].sequence;memcpy(r->center[w],f.postintegration[w].center,12);
   memcpy(r->velocity[w],f.postintegration[w].linear_world,12);r->omega[w]=f.angular_local[w][0];r->flags[w]=ra_meta[w].material;
  }
  ra_done(r);
 }
}
/* Wraps MX Bikes' outer scheduler call (exe+135d70 -> 2a9bd0): publish the
   previous frame's targets before, run the dirt step after. */
static __declspec(noinline) int ra_outer(int elapsed) {
 DWORD incoming=GetLastError();
 InterlockedIncrement(&ra_inflight);LONG depth=InterlockedIncrement(&ra_outer_depth);
 int owner=GetCurrentThreadId()==ra_owner;
 if(depth==1&&owner){ra_phase=0;ra_bad=0;ra_frame_token=ra_reset_token;
  if(ra_frame_token!=ra_applied_token){ra_rebind=1;ra_applied_token=ra_frame_token;}
  if(InterlockedExchange(&ra_new_run,0)){ra_bike=ra_candidate=0;ra_candidate_mask=ra_candidate_conflict=0;ra_rebind=1;}
  if(InterlockedCompareExchange(&ra_foreign,0,0))ra_fault(MXB_FRAME_IDENTITY);
  if(ra_enabled&&ra_allowed&&ra_running&&!ra_fatal){
   if(!ra_session||!ra_transport){ra_reject(-120); /* no allocation/activation while running */}
   else if(!mxdirt_patches_present(ra_image)){ra_fault(-127);}
   else if(!ra_same_grid_check()){ra_fault(-108);}
   else if(elapsed<0||elapsed>100){ra_reject(-109);}
   else if(ra_bike){
    if(!ra_transport_ready()){
     MxbTransportState state;
     if(!mxb_transport_state(ra_transport,&state)||state.faulted||state.phase!=MXB_TRANSPORT_PAUSED||
        !mxb_transport_resume(ra_transport,ra_owner))ra_fault(-121);
    }
    if(!ra_fatal&&(ra_rebind||ra_snapshot.paused)&&!ra_reprime())ra_fault(-110);
    if(!ra_fatal){MxbFrameGate g=ra_gate();int code=mxb_frame_snapshot_begin(&ra_snapshot,&g,elapsed);
     if(code==MXB_FRAME_OK)ra_phase=1;else ra_reject(code);
     uint64_t seq=mxb_frame_snapshot_upcoming_sequence(&ra_snapshot);
     if(ra_phase&&seq){
      {static uint64_t adopt_cursor;uint32_t rects[4*64];size_t k=mxb_transport_adopted_rects(ra_transport,&adopt_cursor,rects,64);
       if(k==(size_t)-1)mxdirt_session_resend_rect(ra_session,0,0,0xffffffffu,0xffffffffu); /* more than we track: resend everything */
       else for(size_t i=0;i<k;i++)mxdirt_session_resend_rect(ra_session,rects[4*i],rects[4*i+1],rects[4*i+2],rects[4*i+3]);}
      int offer=mxdirt_session_publish(ra_session,seq,mxb_transport_accept,ra_transport);ra_publish_offers++;
      if(offer<0){MxbTransportState st; /* transport refused because it faulted: self-heal, defer this offer */
       if(ra_transport&&ra_transport_recovery_budget&&mxb_transport_state(ra_transport,&st)&&st.faulted){
        if(ra_transport_fault_have!=2){ra_transport_fault_state=st;ra_transport_fault_got=1;ra_transport_fault_have=1;}
        if(mxb_transport_recover(ra_transport,ra_owner)){ra_transport_recoveries++;ra_transport_recovery_budget--;offer=0;}}}
      if(offer<0){ra_phase=0;ra_fault(-122);}
      else if(!offer)ra_publish_deferred++;else ra_publish_accepted++;
      if(offer<=0)ra_mark(RA_PUBLISH,offer);
     }
     RaRecord *r=(ra_detail_armed||seq<=3||seq%60==0)?ra_record(RA_BEGIN,code):NULL;if(r){r->elapsed=elapsed;r->outer=seq;ra_done(r);}}
   }
  }else if(!ra_running)ra_rebind=1;
 }else if(owner){ra_bad=MXB_FRAME_REENTRY;ra_rebind=1;ra_mark(RA_REJECT,MXB_FRAME_REENTRY);}
 else InterlockedExchange(&ra_foreign,1);
 SetLastError(incoming);int result=ra_original_outer(elapsed);DWORD outgoing=GetLastError();
 if(depth==1&&owner){
  if(ra_foreign)ra_fault(MXB_FRAME_IDENTITY);
  if(++ra_transport_recovery_clock>=RA_TRANSPORT_RECOVERY_REFILL){ra_transport_recovery_clock=0;if(ra_transport_recovery_budget<RA_TRANSPORT_RECOVERY_BURST)ra_transport_recovery_budget++;}
  if(ra_transport&&!ra_fatal){MxbTransportState state;int got=mxb_transport_state(ra_transport,&state);
  if(!got||state.faulted){if(ra_transport_fault_have!=2){ra_transport_fault_state=state;ra_transport_fault_got=got;ra_transport_fault_have=1;}
    if(got&&ra_transport_recovery_budget&&mxb_transport_recover(ra_transport,ra_owner)){ra_transport_recoveries++;ra_transport_recovery_budget--;}
    else{ra_transport_recovery_failures++;ra_fault(-123);}}}
  if(ra_phase&&!ra_bad&&!ra_fatal&&ra_frame_token==ra_reset_token&&ra_running&&ra_allowed)ra_complete_frame(elapsed);
  else if(ra_phase)ra_rebind=1;
  if(!ra_bike&&ra_candidate_mask==3&&!ra_candidate_conflict&&ra_session){ra_bike=ra_candidate;ra_rebind=1;ra_mark(RA_BIND,1);}
  if(ra_candidate_conflict)ra_fault(-111);
  if(!ra_budget_done&&ra_reserved>=(LONG)RA_RECORDS-8){ra_budget_done=1;ra_mark(RA_BUDGET,1);}
  ra_phase=0;
 }
 InterlockedDecrement(&ra_outer_depth);InterlockedDecrement(&ra_inflight);SetLastError(outgoing);return result;
}
static int ra_write_code(void *to,const void *bytes,size_t n) {
 DWORD old;if(!VirtualProtect(to,n,PAGE_EXECUTE_READWRITE,&old))return 0;
 memcpy(to,bytes,n);int flushed=FlushInstructionCache(GetCurrentProcess(),to,n)!=0;
 DWORD ignored;int protected_again=VirtualProtect(to,n,old,&ignored)!=0;
 if(!protected_again)ra_mark(RA_HOOK,-112);
 return flushed&&protected_again; /* failure may follow a complete write */
}
static int ra_restore(void) {
 if((ra_inflight&&!ra_engine_gap())||GetCurrentThreadId()!=ra_owner)return 0;
 int ok=1;for(int j=3;j>=0;j--){
  RaHook *h=&ra_hooks[j];if(!h->owned)continue;
  unsigned char got[5];if(!ra_read(NULL,(uint64_t)(uintptr_t)h->site,got,5)){ok=0;continue;}
  if(!memcmp(got,h->original,5)){
   if(!FlushInstructionCache(GetCurrentProcess(),h->site,5)){ok=0;continue;}h->owned=0;continue;
  }
  if(memcmp(got,h->patch,5)||!ra_write_code(h->site,h->original,5)){ok=0;continue;}h->owned=0;
 }
 ra_mark(RA_RESTORE,ok);return ok;
}
static int ra_hook_plan(RaHook *h,unsigned char *site,const unsigned char original[5],uintptr_t fn) {
 h->site=site;memcpy(h->original,original,5);
 if(!h->relay){unsigned char *relay=NULL;for(uintptr_t off=0x2000000;off<0x70000000&&!relay;off+=0x10000)
   relay=VirtualAlloc((void *)((uintptr_t)ra_image+off),4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
  if(!relay)return 0;unsigned char bytes[14]={0xff,0x25,0,0,0,0};memcpy(bytes+6,&fn,8);memcpy(relay,bytes,14);
  DWORD old;if(!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(relay,0,MEM_RELEASE);return 0;}
  if(!FlushInstructionCache(GetCurrentProcess(),relay,14)){VirtualFree(relay,0,MEM_RELEASE);return 0;}h->relay=relay;
 }
 unsigned char expected[14]={0xff,0x25,0,0,0,0},current[14];memcpy(expected+6,&fn,8);
 if(!ra_read(NULL,(uintptr_t)h->relay,current,14)||memcmp(current,expected,14))return 0;
 int64_t rel=(int64_t)(uintptr_t)h->relay-(int64_t)(uintptr_t)(site+5);
 if(rel<INT32_MIN||rel>INT32_MAX)return 0;int32_t d=(int32_t)rel;h->patch[0]=0xe8;memcpy(h->patch+1,&d,4);return 1;
}
static int ra_dispose_stopped(int transport_quiescent,int old_track_retired);
static void ra_patches_release(const char *when);
static int ra_zero_native_retirement(void) {
 /* Read ONLY static image objects, never any saved/freed allocation. MX Bikes
    (exe+1f7ea0) clears these fields on both worlds before the paired close returns. */
 static const unsigned allocations[]={0x7a8,0x7b0,0x7b8,0x7c0,0x7c8,0x7d0,0x7d8,0x7e0,0x7e8,0x7f0};
 static const unsigned records[]={0x5a21c8,0xa1e470};unsigned char record[0x68];
 if(ra_close_mask!=3||ra_close_generation!=ra_generation||ra_inflight||ra_close_depth||
    ra_running||GetCurrentThreadId()!=ra_owner||ra_foreign)return 0;
 for(unsigned side=0;side<2;side++){
  if(!ra_read(NULL,(uintptr_t)ra_image+records[side],record,sizeof(record)))return 0;
  for(size_t j=0;j<sizeof(record);j++)if(record[j])return 0;
  for(size_t j=0;j<sizeof(allocations)/sizeof(allocations[0]);j++){
   uint64_t pointer;if(!ra_read(NULL,(uintptr_t)ra_image+0x568488+side*0x6160+allocations[j],&pointer,8)||pointer)return 0;
  }
 }
 MxbTransportState s;return !ra_transport||(mxb_transport_state(ra_transport,&s)&&!s.inflight);
}
static int ra_close(unsigned side,uintptr_t return_address) {
 DWORD incoming=GetLastError();uint64_t generation=ra_generation;
 LONG depth=InterlockedIncrement(&ra_close_depth);InterlockedIncrement(&ra_inflight);
 int owner=GetCurrentThreadId()==ra_owner;
 int guarded=return_address==(uintptr_t)ra_image+ra_close_sites[side]+5&&ra_hooks[side+2].owned;
 InterlockedExchange(&ra_allowed,0);InterlockedExchange(&ra_running,0);InterlockedIncrement(&ra_reset_token);
 if(owner){if(ra_transport)mxb_transport_pause(ra_transport,ra_owner);if(depth!=1||!guarded){ra_latch_first_fault(-131);ra_fatal=1;}}
 else InterlockedExchange(&ra_foreign,1);
 SetLastError(incoming);int result=ra_original_close[side]();DWORD outgoing=GetLastError();
 InterlockedDecrement(&ra_inflight);InterlockedDecrement(&ra_close_depth);
 if(owner&&depth==1&&guarded&&generation==ra_generation&&ra_event_valid&&!ra_foreign&&result==0){
  if(!ra_close_mask)ra_close_generation=generation;
  if(ra_close_generation==generation)ra_close_mask|=1u<<side;
  ra_mark(RA_CLOSE,(int)ra_close_mask);
  if(ra_zero_native_retirement()){
   ra_native_retired=1;ra_stopped_gap=1;
   if(ra_dispose_stopped(1,1)){ra_event_valid=0;ra_mark(RA_CLOSE,4);}
   else ra_mark(RA_RETAINED,5);
   ra_stopped_gap=0;ra_patches_release("track_close");ra_flush();
  }else if(ra_close_mask==3)ra_mark(RA_RETAINED,6);
 }
 SetLastError(outgoing);return result;
}
static __declspec(noinline) int ra_client_close(void){return ra_close(0,(uintptr_t)_ReturnAddress());}
static __declspec(noinline) int ra_host_close(void){return ra_close(1,(uintptr_t)_ReturnAddress());}
static int ra_close_signature(unsigned side) {
 unsigned char bytes[520],call[5];uint64_t hash=UINT64_C(14695981039346656037);
 if(!ra_read(NULL,(uintptr_t)ra_image+ra_close_sites[side],call,5)||memcmp(call,ra_close_calls[side],5)||
    !ra_read(NULL,(uintptr_t)ra_image+ra_close_targets[side],bytes,ra_close_lengths[side]))return 0;
 for(unsigned i=0;i<ra_close_lengths[side];i++)hash=(hash^bytes[i])*UINT64_C(1099511628211);
 return hash==ra_close_hashes[side];
}
static int ra_install(const char *verified_hash) {
 if(!ra_engine_gap())return 0;
 for(int j=0;j<4;j++)if(ra_hooks[j].owned)return 0;
 unsigned char call[5],entry[sizeof(ra_wheel_entry)];
 if(mxb_frame_callsite_plan(ra_read,NULL,(uint64_t)(uintptr_t)ra_image,ra_image_size,verified_hash,&ra_plan)!=MXB_FRAME_OK||
    !ra_read(NULL,(uint64_t)(uintptr_t)ra_image+0x1a76bc,call,5)||memcmp(call,ra_wheel_call,5)||
    !ra_read(NULL,(uint64_t)(uintptr_t)ra_image+0x1a2fb0,entry,sizeof(entry))||memcmp(entry,ra_wheel_entry,sizeof(entry))||
    !ra_close_signature(0)||!ra_close_signature(1))return 0;
 ra_original_wheel=(RaWheelFn)(ra_image+0x1a2fb0);ra_original_outer=(RaOuterFn)(ra_image+0x2a9bd0);
 if(!ra_hook_plan(&ra_hooks[0],ra_image+0x1a76bc,ra_wheel_call,(uintptr_t)ra_wheel)||
    !ra_hook_plan(&ra_hooks[1],ra_image+0x135d70,ra_plan.original_call,(uintptr_t)ra_outer)||
    !ra_hook_plan(&ra_hooks[2],ra_image+ra_close_sites[0],ra_close_calls[0],(uintptr_t)ra_client_close)||
    !ra_hook_plan(&ra_hooks[3],ra_image+ra_close_sites[1],ra_close_calls[1],(uintptr_t)ra_host_close))return 0;
 for(int j=0;j<2;j++)ra_original_close[j]=(RaCloseFn)(ra_image+ra_close_targets[j]);
 for(int j=0;j<4;j++){
  RaHook *h=&ra_hooks[j];if(memcmp(h->site,h->original,5)){ra_restore();return 0;}
  int wrote=ra_write_code(h->site,h->patch,5);
  /* A failed primitive may have completed the write. Retain ownership of any
     changed/unknown bytes so rollback never abandons a reachable wrapper. */
  if(memcmp(h->site,h->original,5))h->owned=1;
  if(!wrote||memcmp(h->site,h->patch,5)){ra_restore();return 0;}
 }
 ra_mark(RA_HOOK,1);return 1;
}
static int ra_prepare_transport(void) {
 ra_bootstrap_requested=0;
 if(!ra_event_valid||!ra_pinned||!ra_activation_requested||!ra_engine_gap()||ra_fatal||ra_foreign)return 0;
 if(ra_transport)return !ra_fatal;
 if(!ra_track_adopt_loaded())return 0;
 if(!mxdirt_patches_present(ra_image)){ra_mark(RA_TRANSPORT,-3);return 0;}
 if(!ra_transport_code)ra_transport_code=ra_near_memory();if(!ra_transport_code)return 0;
 MxbTransportConfig c;memset(&c,0,sizeof(c));
 c.exe=ra_image;c.image_bytes=(size_t)ra_image_size;c.verified_executable_sha256=ra_verified_hash;
 c.near_code=ra_transport_code;c.near_code_bytes=4096;c.span=ra_span;
 c.target_capacity=2;c.native_event_capacity=32;c.max_changed_cells=RA_CELLS<65536?RA_CELLS:65536;
 size_t tiles=(size_t)((ra_geom.width+63)/64)*((ra_geom.height+63)/64);
 c.max_batch_tiles=tiles<4?tiles:4;c.trace_capacity=1024;
 ra_transport_config=c;MxbTransportFacts f=ra_facts(1,0);
 MxbTransportCreateReport report;
 ra_transport=mxb_transport_create_report(&c,&f,&report);
 /* Another native terrain writer (MX's own tyre digging) is adopted per tile
    instead of faulting the whole ledger: no global resync hitch, no shutdown. */
 if(ra_transport)mxb_transport_set_adopt_foreign(ra_transport,1);
 if(ra_log&&!ra_bootstrap_gap){fprintf(ra_log,"{\"active\":true,\"kind\":\"transport_create\",\"generation\":%llu,\"stage\":\"%s\",\"side\":%d,\"succeeded\":%s}\n",(unsigned long long)ra_generation,report.stage,report.side,ra_transport?"true":"false");fflush(ra_log);}
 if(!ra_transport){ra_bootstrap_requested=!strcmp(report.stage,"host_source_mismatch")||!strcmp(report.stage,"stream_last_mismatch")||!strcmp(report.stage,"pending_not_zero")||!strcmp(report.stage,"dirty_flags_not_zero");ra_mark(RA_TRANSPORT,-1);return 0;}
 /* Prepare the solver only after the native streams agree. An earlier loading
    callback must not leave a zero seed cached for a later retry after saved
    terrain has arrived. No game terrain is reset here. */
 if(!ra_setup()){
  mxb_transport_destroy(ra_transport);ra_transport=NULL;return 0;
 }
 if(!ra_engine_gap()||!ra_install(ra_verified_hash)||!ra_engine_gap()||!mxb_transport_install(ra_transport,&f)){
  MxbTransportState s;mxb_transport_state(ra_transport,&s);
  if(s.phase==MXB_TRANSPORT_PREPARED&&!s.owned_patch_mask){
   mxb_transport_destroy(ra_transport);ra_transport=NULL;
   if(ra_restore()){ra_solver_destroy();ra_setup_attempted=0;}
   else ra_fault(-124);
  }
  else ra_fault(-124);
  ra_mark(RA_TRANSPORT,-2);return 0;
 }
 if(!ra_engine_gap()||!mxb_transport_activate(ra_transport,&f,1)){ra_fault(-125);return 0;}
 InterlockedExchange(&ra_allowed,1);ra_mark(RA_TRANSPORT,1);return 1;
}
#include "mxdirt_bootstrap.inc"
/* No SDK callback name alone supplies old_track_retired. The caller must have
   a proven stopped native transport boundary. A failed drain keeps context,
   hooks, gate and session alive, with no return to the competing writer. */
static int ra_dispose_stopped(int transport_quiescent,int old_track_retired) {
 if(!ra_stopped_gap||ra_inflight||GetCurrentThreadId()!=ra_owner)return 0;
 if(old_track_retired&&!ra_native_retired)return 0;
 if(ra_transport){MxbTransportState s;if(!mxb_transport_state(ra_transport,&s))return 0;
  MxbTransportFacts f=ra_facts(transport_quiescent,old_track_retired);
  if(s.phase==MXB_TRANSPORT_ACTIVE)mxb_transport_pause(ra_transport,ra_owner);
  if(old_track_retired){if(!mxb_transport_retire(ra_transport,&f))return 0;}
  else if(s.phase!=MXB_TRANSPORT_INSTALLED&&s.phase!=MXB_TRANSPORT_DRAINED&&s.phase!=MXB_TRANSPORT_REMOVED&&
          s.phase!=MXB_TRANSPORT_PREPARED&&mxb_transport_drain(ra_transport,&f)!=MXB_PUB_OK){ra_mark(RA_RETAINED,1);return 0;}
  mxb_transport_state(ra_transport,&s);
  if(s.phase!=MXB_TRANSPORT_PREPARED&&s.phase!=MXB_TRANSPORT_REMOVED&&!mxb_transport_remove(ra_transport,&f)){ra_mark(RA_RETAINED,2);return 0;}
  if(!mxb_transport_destroy(ra_transport))return 0;ra_transport=NULL;
 }
 if(!ra_restore())return 0;
 if(ra_session)ra_solver_destroy();
 return 1;
}
/* Engine patches are removed only when nothing of ours still runs through them. */
static void ra_patches_release(const char *when){
 if(GetCurrentThreadId()!=ra_owner||ra_inflight||!mxdirt_patches_owned())return;
 if(ra_transport||ra_hooks[0].owned||ra_hooks[1].owned||ra_hooks[2].owned||ra_hooks[3].owned||ra_bootstrap_owned()){
  if(ra_log){fprintf(ra_log,"{\"kind\":\"engine_patches\",\"result\":\"retained\",\"when\":\"%s\"}\n",when);fflush(ra_log);}return;
 }
 mxdirt_patches_remove();
 if(ra_log){fprintf(ra_log,"{\"kind\":\"engine_patches\",\"result\":\"%s\",\"when\":\"%s\"}\n",mxdirt_patches_owned()?"restore_incomplete":"restored",when);fflush(ra_log);}
}
API char *GetModID(void){return "mxbikes";}
API int GetModDataVersion(void){return 8;}
API int GetInterfaceVersion(void){return 9;}
/* Read-only dirt state for the render plugin (src/shared/mxdirt_frame.h). */
API int MxDirtBorrowFrame(uint32_t abi,uint32_t bytes,MxDirtFrame *out){
 DWORD previous=GetLastError();int ok=0;MxdirtSessionState s;MxdirtSimView v;
 if(abi!=MXDIRT_FRAME_ABI||bytes!=sizeof(*out)||!out||GetCurrentThreadId()!=ra_owner||
    ra_inflight||ra_outer_depth||ra_close_depth||!ra_event_valid||!ra_allowed||ra_fatal||ra_foreign||
    !ra_session||!ra_geom_valid||!mxdirt_session_state(ra_session,&s)||s.faulted||
    !mxdirt_session_sim_view(ra_session,&v))goto end;
 MxDirtFrame f;memset(&f,0,sizeof(f));
 f.abi=MXDIRT_FRAME_ABI;f.bytes=sizeof(f);f.owner_thread=ra_owner;f.reserved=0;f.generation=ra_generation;
 f.grid_width=ra_geom.width;f.grid_height=ra_geom.height;f.span_x_m=ra_geom.span_x;f.span_z_m=ra_geom.span_z;
 memcpy(f.origin_m,ra_geom.origin,sizeof(f.origin_m));
 f.sim_size=v.size;f.sim_vertex_exact=v.vertex_exact;f.sim_moisture=v.moisture;f.sim_compaction0=v.compaction0;f.sim_steps=v.steps;
 f.sim_comp=v.comp;f.sim_loose=v.loose;f.sim_moist=v.moist;f.sim_clods=v.clods;
 f.sim_print=v.print;f.sim_print_dir=v.print_dir;f.sim_print_off=v.print_off;
 if(!f.sim_comp||!f.sim_loose||!f.sim_moist||!f.sim_clods||!f.sim_print||!f.sim_print_dir||!f.sim_print_off)goto end;
 {MxdirtSoftView sv;if(mxdirt_session_soft_view(ra_session,&sv)&&sv.tile&&sv.serial){
   f.soft_cell_m=sv.cell;f.soft_thickness_m=sv.thickness;f.soft_tiles_x=sv.tiles_x;f.soft_tiles_z=sv.tiles_z;f.soft_tile_side=sv.tile_side;f.soft_unit_um=1000;
   f.soft_tile=sv.tile;f.soft_tile_serial=sv.serial;}}
 *out=f;ok=1;
end:SetLastError(previous);return ok;
}
/* Settings (mx_dirt.ini): [section] key, default, range, where it lives. Read at Startup, and again whenever
   the file changes while the game runs (the launcher, live): see ra_settings_live. */
typedef struct {const char *section,*key;unsigned def,lo,hi,*value;} RaSetting;
static const RaSetting ra_settings[]={
 {"soil","dirt_sim_depth_percent",60,10,300,&ra_dirt_sim_depth},{"soil","dirt_sim_firmness_percent",55,0,100,&ra_dirt_sim_firmness},
 {"soil","dirt_sim_moisture_percent",35,0,100,&ra_dirt_sim_moisture},{"soil","braking_bumps_percent",100,0,200,&ra_bumps_percent},
 {"soil","rolling_bumps_percent",60,0,100,&ra_rolling_bumps_percent},{"soil","packed_hardness_percent",100,25,200,&ra_hardness_percent},
 {"soil","breakdown_percent",100,0,300,&ra_breakdown_percent},{"soil","drying_percent",100,0,400,&ra_drying_percent},
 {"soil","sweep_percent",100,0,300,&ra_sweep_percent},{"soil","sharp_ruts",0,0,1,&ra_sharp_ruts},
 {"soil","rut_capture_percent",100,0,300,&ra_capture_percent},{"soil","wall_cohesion_percent",100,0,300,&ra_cohesion_percent},
 {"soil","roost_clods_percent",100,0,300,&ra_clods_percent},{"soil","prewear_riders",50,0,400,&ra_prewear_riders},
 {"soil","prewear_spread_cm",140,0,500,&ra_prewear_spread_cm},{"soil","prewear_clip_cm",400,0,600,&ra_prewear_clip_cm},
 {"soil","prewear_fresh_only",1,0,1,&ra_prewear_fresh_only},{"soil","soft_layer_mm",85,0,100,&ra_soft_mm},
 {"soil","extra_riders",0,0,15,&ra_extra_riders},{"soil","ghost_riders",0,0,MXDIRT_SESSION_MAX_GHOSTS,&ra_ghost_riders},
 {"soil","record_ghosts",1,0,1,&ra_record_ghosts},{"soil","follow_game_deformation",1,0,1,&ra_follow_game_deformation},
 {"soil","game_deformation_reference_percent",25,1,200,&ra_game_deformation_reference},{"soil","live_wear_percent",0,0,200,&ra_live_wear},
 {"online","online_wear",0,0,1,&ra_online_wear},{"debug","online_echo",0,0,1,&ra_online_echo},{"debug","dump_terrain",0,0,1,&ra_dump_terrain},
};
enum { RA_SETTINGS=sizeof(ra_settings)/sizeof(*ra_settings) };
/* All settings from the file into the variables, or nothing (0) if any value is out of range. */
static int ra_settings_load(void){
 unsigned v[RA_SETTINGS];
 for(unsigned k=0;k<RA_SETTINGS;k++){const RaSetting *q=ra_settings+k;UINT x=GetPrivateProfileIntA(q->section,q->key,(INT)q->def,ra_ini);if(x<q->lo||x>q->hi)return 0;v[k]=x;}
 for(unsigned k=0;k<RA_SETTINGS;k++)*ra_settings[k].value=v[k];
 ra_live_gate=GetPrivateProfileIntA("soil","live_gate_lines",1,ra_ini);if(ra_live_gate<1||ra_live_gate>8)ra_live_gate=1;
 return 1;
}
/* 1 once per change of the settings file, a quarter of a second after it was last written (never mid-save). */
static FILETIME ra_ini_time;
static int ra_ini_changed(void){
 WIN32_FILE_ATTRIBUTE_DATA a;if(!GetFileAttributesExA(ra_ini,GetFileExInfoStandard,&a)||!CompareFileTime(&a.ftLastWriteTime,&ra_ini_time))return 0;
 FILETIME now;GetSystemTimeAsFileTime(&now);
 const uint64_t n=((uint64_t)now.dwHighDateTime<<32)|now.dwLowDateTime,w=((uint64_t)a.ftLastWriteTime.dwHighDateTime<<32)|a.ftLastWriteTime.dwLowDateTime;
 if(n>=w&&n-w<2500000u)return 0;
 ra_ini_time=a.ftLastWriteTime;return 1;
}
/* Live settings. Owner thread, between frames. A changed file is read again; a file with a value out of
   range changes nothing. Soil tuning, bumps, wear speed and lines reach the running session at once (not in
   an online race, where every player must run the same values); pre-wear, the soft layer's depth, sharp ruts,
   ghost riders and online wear are used when the next track loads. */
static void ra_settings_live(void){
 if(!ra_ini_changed())return;
 const unsigned depth=ra_dirt_sim_depth,firm=ra_dirt_sim_firmness,moist=ra_dirt_sim_moisture,bumps=ra_bumps_percent,rolling=ra_rolling_bumps_percent,
  hard=ra_hardness_percent,breakdown=ra_breakdown_percent,drying=ra_drying_percent,sweep=ra_sweep_percent,capture=ra_capture_percent,
  cohesion=ra_cohesion_percent,clods=ra_clods_percent,gate=ra_live_gate,wear=ra_live_wear;
 const int ok=ra_settings_load();int applied=0;
 if(ok&&ra_session&&!ra_online){
  MxdirtLiveSettings t;float *f=(float*)&t;for(unsigned k=0;k<sizeof(t)/sizeof(float);k++)f[k]=-1.f;
  if(depth!=ra_dirt_sim_depth){float d=(float)ra_dirt_sim_depth; /* x this session's Deformation slider factor, as ra_game_depth */
   if(!ra_game_deform_off&&ra_game_factor>0.f){d*=ra_game_factor;if(d<10.f)d=10.f;if(d>300.f)d=300.f;}t.depth_percent=d;}
  if(firm!=ra_dirt_sim_firmness)t.firmness_percent=(float)ra_dirt_sim_firmness;
  if(moist!=ra_dirt_sim_moisture)t.moisture_percent=(float)ra_dirt_sim_moisture;
  if(bumps!=ra_bumps_percent)t.bumps_percent=(float)ra_bumps_percent;
  if(rolling!=ra_rolling_bumps_percent)t.rolling_bumps_percent=(float)ra_rolling_bumps_percent;
  if(hard!=ra_hardness_percent)t.hardness_percent=(float)ra_hardness_percent;
  if(breakdown!=ra_breakdown_percent)t.breakdown_percent=(float)ra_breakdown_percent;
  if(drying!=ra_drying_percent)t.drying_percent=(float)ra_drying_percent;
  if(sweep!=ra_sweep_percent)t.sweep_percent=(float)ra_sweep_percent;
  if(capture!=ra_capture_percent)t.capture_percent=(float)ra_capture_percent;
  if(cohesion!=ra_cohesion_percent)t.cohesion_percent=(float)ra_cohesion_percent;
  if(clods!=ra_clods_percent)t.clods_percent=(float)ra_clods_percent;
  applied=mxdirt_session_live_settings(ra_session,&t);
  if(gate!=ra_live_gate)applied=mxdirt_session_set_live_gate(ra_session,ra_live_gate)&&applied;
  if(wear!=ra_live_wear)applied=mxdirt_session_set_live_wear(ra_session,(float)ra_live_wear)&&applied;
 }
 if(ra_log){fprintf(ra_log,"{\"kind\":\"live_settings\",\"read\":%d,\"session\":%d,\"applied_now\":%d,\"dirt_sim_depth_percent\":%u,\"dirt_sim_firmness_percent\":%u,\"dirt_sim_moisture_percent\":%u,\"braking_bumps_percent\":%u,\"rolling_bumps_percent\":%u,\"live_wear_percent\":%u,\"live_gate_lines\":%u,\"prewear_riders\":%u,\"soft_layer_mm\":%u}\n",
  ok,ra_session?1:0,applied,ra_dirt_sim_depth,ra_dirt_sim_firmness,ra_dirt_sim_moisture,ra_bumps_percent,ra_rolling_bumps_percent,ra_live_wear,ra_live_gate,ra_prewear_riders,ra_soft_mm);fflush(ra_log);}
}
API int Startup(char *savepath) {
 (void)savepath;ra_owner=GetCurrentThreadId();ra_image=(unsigned char *)GetModuleHandleW(NULL);
 char path[MAX_PATH],logpath[MAX_PATH];
 DWORD n=GetModuleFileNameA(NULL,path,MAX_PATH);if(!n||n>=MAX_PATH||!ra_hash_file(path,ra_verified_hash)||strcmp(ra_verified_hash,ra_exe_sha))return -1;
 IMAGE_DOS_HEADER dos;IMAGE_NT_HEADERS64 pe;
 if(!ra_read(NULL,(uint64_t)(uintptr_t)ra_image,&dos,sizeof(dos))||dos.e_magic!=IMAGE_DOS_SIGNATURE||dos.e_lfanew<0||
    !ra_read(NULL,(uint64_t)(uintptr_t)ra_image+(uint32_t)dos.e_lfanew,&pe,sizeof(pe))||pe.Signature!=IMAGE_NT_SIGNATURE||pe.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return -1;
 ra_image_size=pe.OptionalHeader.SizeOfImage;
 char *slash=strrchr(path,'\\');if(!slash)return -1;slash[1]=0;
 if(snprintf(ra_ini,sizeof(ra_ini),"%splugins\\mx_dirt.ini",path)>=(int)sizeof(ra_ini)||
    snprintf(logpath,sizeof(logpath),"%splugins\\mx_dirt.jsonl",path)>=(int)sizeof(logpath))return -1;
 ra_enabled=GetPrivateProfileIntA("active","enabled",1,ra_ini)==1;
 if(!ra_enabled)return -1;
 if(!ra_settings_load())return -1;ra_ini_changed(); /* remembers the file time: later changes are picked up live */
 GetPrivateProfileStringA("soil","sand_tracks","",ra_sand_tracks,sizeof(ra_sand_tracks),ra_ini);
 HMODULE pinned;if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCSTR)(uintptr_t)ra_outer,&pinned))return -1;ra_pinned=1;
 ra_log=fopen(logpath,"w");if(!ra_log)return -1;
 LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
 fprintf(ra_log,"{\"active\":true,\"kind\":\"startup\",\"plugin\":\"mx_dirt\",\"schema\":1,\"qpc_frequency\":%lld,\"exe_sha256\":\"%s\",\"load_tau_s\":%.3g,\"height_response\":%.3g}\n",
  frequency.QuadPart,ra_verified_hash,(double)MXDIRT_LOAD_TAU_S,(double)RA_HEIGHT_RESPONSE);
 fprintf(ra_log,"{\"kind\":\"config\",\"dirt_sim_depth_percent\":%u,\"dirt_sim_firmness_percent\":%u,\"dirt_sim_moisture_percent\":%u,\"braking_bumps_percent\":%u,\"rolling_bumps_percent\":%u,\"packed_hardness_percent\":%u,\"breakdown_percent\":%u,\"drying_percent\":%u,\"sweep_percent\":%u,\"sharp_ruts\":%u,\"rut_capture_percent\":%u,\"wall_cohesion_percent\":%u,\"roost_clods_percent\":%u,\"prewear_riders\":%u,\"prewear_spread_cm\":%u,\"prewear_clip_cm\":%u,\"prewear_fresh_only\":%u,\"soft_layer_mm\":%u,\"extra_riders\":%u,\"live_gate_lines\":%u,\"live_wear_percent\":%u,\"ghost_riders\":%u,\"record_ghosts\":%u,\"follow_game_deformation\":%u,\"game_deformation_reference_percent\":%u,\"sand_tracks\":\"%s\",\"dump_terrain\":%u,\"online_wear\":%u}\n",
  ra_dirt_sim_depth,ra_dirt_sim_firmness,ra_dirt_sim_moisture,ra_bumps_percent,ra_rolling_bumps_percent,ra_hardness_percent,ra_breakdown_percent,ra_drying_percent,ra_sweep_percent,ra_sharp_ruts,ra_capture_percent,ra_cohesion_percent,ra_clods_percent,
  ra_prewear_riders,ra_prewear_spread_cm,ra_prewear_clip_cm,ra_prewear_fresh_only,ra_soft_mm,ra_extra_riders,ra_live_gate,ra_live_wear,ra_ghost_riders,ra_record_ghosts,ra_follow_game_deformation,ra_game_deformation_reference,
  strchr(ra_sand_tracks,'"')||strchr(ra_sand_tracks,'\\')?"?":ra_sand_tracks,ra_dump_terrain,ra_online_wear);
 fflush(ra_log);
 return 3; /* telemetry is unused */
}
API void EventInit(void *data,int size) {
 if(GetCurrentThreadId()!=ra_owner)InterlockedExchange(&ra_foreign,1);
 InterlockedExchange(&ra_allowed,0);InterlockedExchange(&ra_running,0);InterlockedIncrement(&ra_reset_token);
 if(!ra_enabled||GetCurrentThreadId()!=ra_owner||ra_inflight)return;
 ra_settings_live(); /* settings saved since the last track: this one loads with them */
 if(!ra_bootstrap_remove()){ra_fault(-129);return;}
 /* Retirement proof is supplied separately; never reinterpret pause as it. */
 if(ra_transport||ra_hooks[0].owned||ra_hooks[1].owned||ra_hooks[2].owned||ra_hooks[3].owned){ra_fault(-126);ra_mark(RA_RETAINED,3);return;}
 if(ra_session)ra_solver_destroy();
 ra_event_valid=0;ra_geom_valid=0;
 ra_close_mask=0;ra_close_generation=0;ra_native_retired=0;
 ra_bike=ra_candidate=0;ra_candidate_mask=ra_candidate_conflict=ra_fatal=ra_setup_attempted=ra_phase=0;ra_rebind=1;
 memset(ra_surface_frames,0,sizeof(ra_surface_frames));ra_game_deform_off=0;ra_transport_recoveries=ra_transport_recovery_failures=ra_transport_recovery_clock=0;ra_transport_recovery_budget=RA_TRANSPORT_RECOVERY_BURST;ra_transport_fault_have=0;
 InterlockedExchange(&ra_new_run,0);InterlockedExchange(&ra_foreign,0);ra_applied_token=ra_reset_token;
 memset(ra_physics,0,sizeof(ra_physics));
 if(!data||size<(int)sizeof(RaEvent)||ra_generation==UINT64_MAX)return;
 /* Testing (type 1) always; an online race (type 2) only with the experimental [online] online_wear=1. */
 RaEvent e;if(!ra_read(NULL,(uint64_t)(uintptr_t)data,&e,sizeof(e))||(e.type!=1&&!(e.type==2&&ra_online_wear))||
    !memchr(e.trackID,0,sizeof(e.trackID))||!ra_track_select_event(e.trackID))return;
 ra_online=e.type==2;
 ra_flush_first_fault();memset(&ra_first_fault,0,sizeof(ra_first_fault));
 /* Both engine fixes must be in place before any session starts on this track. */
 const char *why="";int patched=mxdirt_patches_install(ra_image,&why);
 if(ra_log){fprintf(ra_log,"{\"kind\":\"engine_patches\",\"result\":\"%s\",\"reason\":\"%s\",\"track\":\"%s\",\"signed_add_rva\":\"1f61e6\",\"nonnegative_material_rva\":\"1f1f65\"}\n",
  patched==MXDIRT_PATCHES_INSTALLED?"installed":patched==MXDIRT_PATCHES_FOREIGN?"present":"failed",why,ra_event_track);fflush(ra_log);}
 if(!patched){ra_mark(RA_PATCHES,0);ra_flush();return;} /* inactive for this track */
 ra_generation++;ra_event_valid=1;ra_mark(RA_EVENT,1);
 memcpy(ra_ghost_bike,e.bikeID,sizeof(ra_ghost_bike)-1);ra_ghost_bike[sizeof(ra_ghost_bike)-1]=0;ra_ghost_recording_reset();
 ra_bootstrap_phase=ra_bootstrap_requested=0;InterlockedExchange(&ra_bootstrap_cancel,0);
 /* EventInit can precede loading the run's saved surface. First adoption is
    deferred to RunInit; EventInit must neither cache an early zero seed nor own the writer. */
 if(ra_log){fprintf(ra_log,"{\"kind\":\"track_wait\",\"track\":\"%s\",\"generation\":%llu,\"waiting_for\":\"loaded_native_terrain\"}\n",ra_event_track,(unsigned long long)ra_generation);fflush(ra_log);}
 ra_flush();
}
API void RunInit(void *data,int size){(void)data;(void)size;
 if(GetCurrentThreadId()!=ra_owner)InterlockedExchange(&ra_foreign,1);
 InterlockedExchange(&ra_running,0);InterlockedIncrement(&ra_reset_token);InterlockedExchange(&ra_new_run,1);
 if(GetCurrentThreadId()==ra_owner&&!ra_inflight&&ra_event_valid&&!ra_fatal){
  if(!ra_bootstrap_remove()){ra_fault(-129);return;}ra_bootstrap_phase=0;InterlockedExchange(&ra_bootstrap_cancel,0);
  ra_stopped_gap=1;if(!ra_prepare_transport()&&ra_bootstrap_requested)ra_bootstrap_arm();ra_stopped_gap=0;
 }
}
API void RunStart(void){
 DWORD incoming=GetLastError();if(GetCurrentThreadId()!=ra_owner)InterlockedExchange(&ra_foreign,1);
 /* Saved terrain can still be in transit at RunInit. Retry only before the
    SDK resumes this owner, only for an unowned first session, and with every
    convergence/seed check intact. */
 if(!ra_running&&ra_new_run&&ra_event_valid&&!ra_session&&!ra_transport&&!ra_fatal&&!ra_foreign&&!ra_bootstrap_owned()&&
    GetCurrentThreadId()==ra_owner&&!ra_inflight){
  ra_stopped_gap=1;if(!ra_prepare_transport()&&ra_bootstrap_requested)ra_bootstrap_arm();ra_stopped_gap=0;
 }
 InterlockedIncrement(&ra_reset_token);InterlockedExchange(&ra_running,1);SetLastError(incoming);
}
API __declspec(noinline) void RunStop(void){
 void *return_slot=_AddressOfReturnAddress();DWORD incoming=GetLastError();
 LONG was_running=InterlockedExchange(&ra_running,0);InterlockedIncrement(&ra_reset_token);ra_bootstrap_clock=0;
 if(GetCurrentThreadId()==ra_owner){
  /* First ownership only after a real ordinary pause. Internal teardown,
     replay, retained/faulted state and nested callbacks never retry. */
  if(was_running&&ra_new_run&&ra_event_valid&&!ra_allowed&&!ra_session&&!ra_transport&&
     !ra_setup_attempted&&!ra_fatal&&!ra_foreign&&!ra_inflight&&!ra_outer_depth&&!ra_close_depth&&!ra_bootstrap_owned()&&
     !ra_hooks[0].owned&&!ra_hooks[1].owned&&!ra_hooks[2].owned&&!ra_hooks[3].owned){
   NT_TIB *tib=(NT_TIB*)NtCurrentTeb();
   MxbRunStopOriginRequest origin={(uintptr_t)ra_image,ra_image_size,
    (uintptr_t)tib->StackLimit,(uintptr_t)tib->StackBase,(uintptr_t)return_slot,ra_verified_hash};
   int qualified=mxb_runstop_pause_origin(ra_read,NULL,&origin);
   if(qualified==MXB_RUNSTOP_ORDINARY_PAUSE){
    ra_stopped_gap=1;int adopted=ra_prepare_transport();ra_stopped_gap=0;
    if(ra_log)fprintf(ra_log,"{\"active\":true,\"kind\":\"pause_adoption\",\"generation\":%llu,\"ordinary_pause_verified\":true,\"adopted\":%s,\"still_paused\":true}\n",(unsigned long long)ra_generation,adopted?"true":"false");
   }
  }
  if(ra_transport)mxb_transport_pause(ra_transport,ra_owner);ra_mark(RA_STOP,1);ra_flush();
 }else InterlockedExchange(&ra_foreign,1);
 SetLastError(incoming);
}
API void RunDeinit(void){RunStop();ra_bootstrap_abort(-10);if(GetCurrentThreadId()==ra_owner&&!ra_inflight&&!ra_bootstrap_remove())ra_fault(-129);}
API void RunTelemetry(void *data,int size,float time,float pos){(void)data;(void)size;(void)time;(void)pos;}
/* Online wear (experimental): where the game says every bike is. Only copied here; the session turns the
   other bikes' tracks into tyre passes on its next step, once the layout of these positions has been
   proven against the local bike (mxdirt_remote.h). */
typedef struct {int num;float x,y,z,yaw,track;int crashed;} RaTrackPosition;
API void RaceTrackPosition(int count,void *array,int elem){
 DWORD incoming=GetLastError();
 if(GetCurrentThreadId()==ra_owner&&ra_session&&(ra_online||ra_online_echo)&&!ra_fatal&&!ra_inflight&&
    InterlockedCompareExchange(&ra_allowed,0,0)&&array&&count>0&&elem>=(int)sizeof(RaTrackPosition)&&elem<=256){
  MxdRemotePos pos[65];int n=0;if(count>64)count=64;
  for(int i=0;i<count;i++){RaTrackPosition t;if(!ra_read(NULL,(uint64_t)(uintptr_t)array+(uint64_t)i*(uint64_t)elem,&t,sizeof(t)))break;
   pos[n].num=t.num;pos[n].p[0]=t.x;pos[n].p[1]=t.y;pos[n].p[2]=t.z;pos[n].crashed=t.crashed;n++;}
  /* [debug] online_echo=1 (offline test): a copy of every reported bike riding 3 m beside it */
  if(ra_online_echo&&n){int m=n<32?n:32;for(int i=0;i<m&&n<65;i++){pos[n]=pos[i];pos[n].num=pos[i].num+100000;pos[n].p[0]+=3.f;n++;}}
  LARGE_INTEGER c,f;QueryPerformanceCounter(&c);QueryPerformanceFrequency(&f);
  if(n&&f.QuadPart>0)mxdirt_session_remote_feed(ra_session,pos,n,(double)c.QuadPart/(double)f.QuadPart);
 }
 SetLastError(incoming);
}
API void EventDeinit(void){
 InterlockedExchange(&ra_allowed,0);RunStop();ra_bootstrap_abort(-11);
 if(GetCurrentThreadId()==ra_owner&&!ra_inflight){
  if(!ra_bootstrap_remove()){ra_fault(-129);return;}
  ra_stopped_gap=1;
  /* This SDK callback runs before client grids are freed. Only both guarded
     native close returns plus the static null/reset checks may retire them. */
  if(!ra_dispose_stopped(0,0))ra_mark(RA_RETAINED,4);
  ra_stopped_gap=0;ra_patches_release("event_deinit");ra_flush();
 }
}
API void Shutdown(void){EventDeinit();ra_lap_join();if(GetCurrentThreadId()==ra_owner&&!ra_inflight){
 ra_patches_release("shutdown");
 if(!ra_session&&!ra_transport){free(ra_rec);ra_rec=NULL;ra_rec_cap=ra_rec_n=0;}
 if(ra_log){fclose(ra_log);ra_log=NULL;}
}}
