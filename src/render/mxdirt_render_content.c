/* Dirt Material content: links to the core plugin (mx_dirt.dlo) through its
   read-only MxDirtBorrowFrame export, checks that the core's terrain is the
   terrain MX Bikes just drew, and draws the dirt pass over it. Read-only on
   native memory; GL work only on the captured render context. */
#include "mxdirt_render_content.h"
#include "mxdirt_dirt_gl.h"
#include "mxdirt_frame.h"
#include "mxdirt_fast_read.h"
#include <GL/gl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static DWORD rc_thread;
static int rc_started,rc_live,rc_retire,rc_fault;
static int rc_dirt,rc_dirt_fault,rc_soft=1,rc_soft_live,rc_soft_solid=1,rc_tyre_sink=1;
static MxDirt *rc_cpu;static MxDirtGl *rc_gl;static MxDirtLook rc_look;
static uint64_t rc_serial,rc_peak,rc_ticks,rc_recentres,rc_frames,rc_skips,rc_generation;
static int rc_state_live;static MxDirtStats rc_stats;
/* Measurement only (log): the longest gap between two drawn frames of the window and how many were over 20 / 33 ms,
   the GPU time of the pass (sampled), where the camera is; a gap over 40 ms also gets a line of its own. */
static uint64_t rc_part[3],rc_peak_part[3];static uint64_t rc_last_draw,rc_gap_peak;static unsigned rc_gap20,rc_gap33,rc_hitches,rc_gpu_n;static double rc_gpu_sum,rc_gpu_peak;static float rc_cam[3];static int rc_recentred;
static MxDirtTsTerrain rc_bound;
static MxDirtBorrowFrameFn rc_borrow;
static FILE *rc_log;static unsigned rc_lines;
static double rc_ms_per_tick;
static uint64_t rc_stamp(void){LARGE_INTEGER t;return QueryPerformanceCounter(&t)?(uint64_t)t.QuadPart:0;}
static uint64_t rc_elapsed(uint64_t start){uint64_t end=rc_stamp();return start&&end>=start?end-start:0;}
void mxdirt_content_note(const char *fmt,...){
 if(!rc_log||rc_lines>=4096)return;rc_lines++;fprintf(rc_log,"%llu ",(unsigned long long)GetTickCount64());
 va_list a;va_start(a,fmt);vfprintf(rc_log,fmt,a);va_end(a);fputc('\n',rc_log);fflush(rc_log);
}
/* INI paths may be absolute or relative to the game folder (the folder holding
   mxbikes.exe). Resolves in place; returns the new length or 0. */
static DWORD game_path(wchar_t *path,DWORD n,size_t cap){
 if(!n||n>=cap-1)return 0;
 if((n>=3&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/'))||(n>=3&&path[0]==L'\\'&&path[1]==L'\\'))return n;
 static wchar_t dir[32768];DWORD d=GetModuleFileNameW(NULL,dir,32768);if(!d||d>=32768)return 0;
 wchar_t *slash=wcsrchr(dir,L'\\');if(!slash)return 0;d=(DWORD)(slash+1-dir);
 if((size_t)d+n>=cap-1)return 0;
 wmemmove(path+d,path,(size_t)n+1);wmemcpy(path,dir,d);return d+n;
}
/* Native memory reads: checked once, remembered for MXD_FR_FRAMES frames, then plain
   copies (mxdirt_fast_read.h). Owner thread only. */
static MxdFastRead rc_reads;
static int rc_read(uintptr_t p,void *out,size_t n){return mxd_fr_read(&rc_reads,p,out,n);}
/* The core is optional and may load after us: look it up lazily. It is pinned
   once found so the borrowed export can never unload under us. */
static int rc_resolve(void){
 if(rc_borrow)return 1;
 HMODULE module=GetModuleHandleW(L"mx_dirt.dlo");if(!module)return 0;
 FARPROC entry=GetProcAddress(module,"MxDirtBorrowFrame");if(!entry)return 0;
 HMODULE pinned=NULL;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)(uintptr_t)entry,&pinned)||pinned!=module)return 0;
 rc_borrow=(MxDirtBorrowFrameFn)(uintptr_t)entry;mxdirt_content_note("core_export_found abi=%u",(unsigned)MXDIRT_FRAME_ABI);return 1;
}
static int rc_frame(MxDirtFrame *f){
 memset(f,0,sizeof(*f));
 if(!rc_resolve()||rc_borrow(MXDIRT_FRAME_ABI,(uint32_t)sizeof(*f),f)!=1)return 0;
 return f->abi==MXDIRT_FRAME_ABI&&f->bytes==sizeof(*f)&&f->owner_thread==rc_thread&&f->generation;
}
/* GL cleanup only on the owner context; retried on the next frame otherwise. */
static int rc_cleanup(void){
 if(!rc_retire)return 1;
 if(rc_gl&&!mxdirt_gl_destroy(rc_gl))return 0;rc_gl=NULL;mxdirt_destroy(rc_cpu);rc_cpu=NULL;rc_serial=0;rc_dirt_fault=0;
 memset(&rc_bound,0,sizeof(rc_bound));rc_generation=0;rc_retire=0;mxdirt_content_note("scene_retired");return 1;
}
/* The launcher key: pressed in the game - only while the game's own window is in front - it brings up the
   MX Dirt Mod launcher (plugins\mx_dirt_launcher, next to the settings file): the open one, or a new one
   that starts on top. Polled once per drawn frame. */
static int rc_key,rc_key_down;static wchar_t rc_ini[1024];
static void rc_launcher(void){
 HWND open=FindWindowW(NULL,L"MX Dirt Mod");
 if(open){if(IsIconic(open))ShowWindow(open,SW_RESTORE);SetForegroundWindow(open);mxdirt_content_note("launcher_key shown=1");return;}
 wchar_t dir[1100],sys[300],cmd[2600];wcscpy(dir,rc_ini);wchar_t *slash=wcsrchr(dir,L'\\');if(!slash)return;wcscpy(slash+1,L"mx_dirt_launcher");
 UINT n=GetSystemDirectoryW(sys,300);if(!n||n>=300)return;
 if(_snwprintf(cmd,2600,L"\"%ls\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden -File \"%ls\\MXDirtLauncher.ps1\" -OnTop",sys,dir)<0)return;
 cmd[2599]=0;wchar_t script[1200];_snwprintf(script,1200,L"%ls\\MXDirtLauncher.ps1",dir);script[1199]=0;
 if(GetFileAttributesW(script)==INVALID_FILE_ATTRIBUTES){mxdirt_content_note("launcher_key launcher_missing=1");return;}
 STARTUPINFOW si;memset(&si,0,sizeof(si));si.cb=sizeof(si);PROCESS_INFORMATION pi;
 const int ok=CreateProcessW(NULL,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,dir,&si,&pi)!=0;
 if(ok){CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
 mxdirt_content_note("launcher_key started=%d",ok);
}
static void rc_key_poll(void){
 if(!rc_key)return;const int down=(GetAsyncKeyState(rc_key)&0x8000)!=0;
 if(down&&!rc_key_down){DWORD pid=0;HWND front=GetForegroundWindow();if(front&&GetWindowThreadProcessId(front,&pid)&&pid==GetCurrentProcessId())rc_launcher();}
 rc_key_down=down;
}
/* The look settings (mx_dirt_render.ini). Read at startup and again whenever the file changes while the game
   runs (the launcher, live). At startup an invalid dirt key switches only this pass off; later, a file with
   an invalid value changes nothing. The pass can be switched off and on again live, but hooks are only
   installed if it was on when the game started. */
static FILETIME rc_ini_time;
static int rc_look_read(int startup){
 UINT dirt=GetPrivateProfileIntW(L"renderer",L"dirt_material",1,rc_ini),strength=GetPrivateProfileIntW(L"renderer",L"dirt_strength",135,rc_ini),loose=GetPrivateProfileIntW(L"renderer",L"dirt_loose",55,rc_ini);
 UINT fade_start=GetPrivateProfileIntW(L"renderer",L"dirt_fade_start",75,rc_ini),fade_end=GetPrivateProfileIntW(L"renderer",L"dirt_fade_end",100,rc_ini);
 /* 45..60 was the default and the most the smaller window of 1.5 could show: a settings file that still says so gets today's default */
 if(fade_start==45&&fade_end==60){fade_start=75;fade_end=100;}
 UINT relief=GetPrivateProfileIntW(L"renderer",L"dirt_relief",100,rc_ini);
 const int valid=dirt<=1&&strength<=200&&loose<=100&&fade_start<fade_end&&fade_end<=100&&relief<=200;
 if(!valid&&!startup)return 0;
 rc_soft=GetPrivateProfileIntW(L"renderer",L"soft_layer",1,rc_ini)!=0;
 rc_soft_solid=GetPrivateProfileIntW(L"renderer",L"soft_solid",1,rc_ini)!=0;rc_tyre_sink=GetPrivateProfileIntW(L"renderer",L"tyre_sink",1,rc_ini)!=0;
 /* An invalid dirt key disables only this pass, never the renderer. */
 rc_dirt=dirt==1&&valid;
 if(valid){rc_look.strength=(float)strength/100.f;rc_look.base_loose=(float)loose/100.f;rc_look.fade_start=(float)fade_start;rc_look.fade_end=(float)fade_end;rc_look.relief=(float)relief/100.f;}
 /* launcher_key = F1..F12 (default F10); anything else = no key */
 {wchar_t k[8];GetPrivateProfileStringW(L"renderer",L"launcher_key",L"F10",k,8,rc_ini);int n=(k[0]==L'F'||k[0]==L'f')?_wtoi(k+1):0;rc_key=n>=1&&n<=12?VK_F1+n-1:0;}
 return 1;
}
/* 1 once per change of the settings file, a quarter of a second after it was last written (never mid-save). */
static int rc_ini_changed(void){
 WIN32_FILE_ATTRIBUTE_DATA a;if(!rc_ini[0]||!GetFileAttributesExW(rc_ini,GetFileExInfoStandard,&a)||!CompareFileTime(&a.ftLastWriteTime,&rc_ini_time))return 0;
 FILETIME now;GetSystemTimeAsFileTime(&now);
 const uint64_t n=((uint64_t)now.dwHighDateTime<<32)|now.dwLowDateTime,w=((uint64_t)a.ftLastWriteTime.dwHighDateTime<<32)|a.ftLastWriteTime.dwLowDateTime;
 if(n>=w&&n-w<2500000u)return 0;
 rc_ini_time=a.ftLastWriteTime;return 1;
}
int mxdirt_content_startup(const wchar_t *ini){
 DWORD error=GetLastError();int ok=0;if(rc_started||!ini)goto end;
 rc_thread=GetCurrentThreadId();{LARGE_INTEGER f;if(QueryPerformanceFrequency(&f)&&f.QuadPart>0)rc_ms_per_tick=1000./(double)f.QuadPart;}
 if(GetPrivateProfileIntW(L"renderer",L"enabled",0,ini)!=1)goto end;
 if(wcslen(ini)>=sizeof(rc_ini)/sizeof(*rc_ini))goto end;wcscpy(rc_ini,ini);
 (void)rc_look_read(1);(void)rc_ini_changed(); /* remembers the file time: later changes are picked up live */
 wchar_t log[32768];DWORD n=GetPrivateProfileStringW(L"renderer",L"content_log",L"plugins\\mx_dirt_render.log",log,32768,ini);
 n=game_path(log,n,32768);
 if(n>=4&&n<32767&&log[1]==L':'&&(log[2]==L'\\'||log[2]==L'/'))rc_log=_wfopen(log,L"wb");
 rc_started=1;ok=1;
 mxdirt_content_note("content_ready dirt_material=%d strength=%.0f loose=%.0f fade=%.0f..%.0f soft_layer=%d",rc_dirt,(double)rc_look.strength*100.,(double)rc_look.base_loose*100.,(double)rc_look.fade_start,(double)rc_look.fade_end,rc_soft);
end:SetLastError(error);return ok;
}
int mxdirt_content_dirt_enabled(void){return rc_started&&rc_dirt;}
uint64_t mxdirt_content_generation(void){
 DWORD error=GetLastError();uint64_t generation=0;MxDirtFrame f;
 if(rc_started&&rc_live&&!rc_fault&&GetCurrentThreadId()==rc_thread&&rc_frame(&f))generation=f.generation;
 SetLastError(error);return generation;
}
static int near_value(float a,float b){return isfinite(a)&&isfinite(b)&&fabsf(a-b)<=1e-3f+1e-5f*fabsf(b);}
static uint32_t rc_u32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static uintptr_t rc_ptr(const unsigned char *p){uintptr_t v;memcpy(&v,p,8);return v;}
/* The core's terrain (grid size, span, origin) is the terrain MX Bikes drew,
   and the native descriptor still says what it said at the call. */
static int rc_identity(const MxDirtTsTerrain *t,const MxDirtFrame *f){
 if(!t||!f||t->flags||!t->h||!t->model||!t->base_grid||!t->render_grid)return 0;
 if(f->grid_width!=t->width||f->grid_height!=t->height||!near_value(f->span_x_m,t->extent_x)||!near_value(f->span_z_m,t->extent_z))return 0;
 for(unsigned k=0;k<3;k++)if(!near_value(f->origin_m[k],t->origin[k]))return 0;
 unsigned char h[0x30],m[8],hp[8];
 if(t->h>UINTPTR_MAX-sizeof(h)||t->model>UINTPTR_MAX-0x1900||
    !rc_read(t->h,h,sizeof(h))||!rc_read(t->model+0x10c,m,8)||!rc_read(t->model+0x18f8,hp,8))return 0;
 return rc_u32(m)==1&&!(rc_u32(m+4)&0x100)&&rc_ptr(hp)==t->h&&rc_u32(h)==t->width&&rc_u32(h+4)==t->height&&
  rc_ptr(h+8)==t->base_grid&&rc_ptr(h+0x10)==t->render_grid&&!memcmp(h+0x18,&t->extent_x,4)&&
  !memcmp(h+0x1c,&t->extent_z,4)&&!memcmp(h+0x20,&t->height_scale,4)&&!memcmp(h+0x24,t->origin,12);
}
static int rc_same(const MxDirtTsTerrain *a,const MxDirtTsTerrain *b){
 return a->h==b->h&&a->model==b->model&&a->base_grid==b->base_grid&&a->render_grid==b->render_grid;
}
/* Dirt window reads: both native grids only, bounded spans. */
typedef struct {uintptr_t first[2],last[2];} RcReader;
static int rc_grid_read(void *user,uintptr_t address,void *out,size_t bytes){
 RcReader *r=user;if(!r||!bytes||bytes>(size_t)64<<20)return 0;
 for(unsigned k=0;k<2;k++)if(address>=r->first[k]&&address<=r->last[k]&&bytes<=r->last[k]-address)return rc_read(address,out,bytes);
 return 0;
}
/* The same bounded spans, looked at in place instead of copied out. */
static const void *rc_grid_view(void *user,uintptr_t address,size_t bytes){
 RcReader *r=user;if(!r||!bytes||bytes>(size_t)64<<20)return NULL;
 for(unsigned k=0;k<2;k++)if(address>=r->first[k]&&address<=r->last[k]&&bytes<=r->last[k]-address)
  return mxd_fr_check(&rc_reads,address,bytes)?(const void*)address:NULL;
 return NULL;
}
static int rc_draw_dirt(const MxDirtFrame *frame,const MxDirtCameraScope *scope,const MxDirtTsTerrain *terrain){
 rc_ticks=0;memset(&rc_stats,0,sizeof(rc_stats));if(!rc_dirt||rc_dirt_fault||scope->serial<=rc_serial)return 0;
 size_t grid_bytes=(size_t)terrain->width*terrain->height*2;
 if(terrain->render_grid>UINTPTR_MAX-grid_bytes||terrain->base_grid>UINTPTR_MAX-grid_bytes)return 0;
 RcReader reader={{terrain->render_grid,terrain->base_grid},{terrain->render_grid+grid_bytes,terrain->base_grid+grid_bytes}};
 MxDirtInput in;memset(&in,0,sizeof(in));memcpy(in.modelview,scope->modelview,sizeof(in.modelview));in.current_grid=terrain->render_grid;in.base_grid=terrain->base_grid;
 in.width=terrain->width;in.height=terrain->height;in.extent_x=terrain->extent_x;in.extent_z=terrain->extent_z;in.height_scale=terrain->height_scale;
 memcpy(in.origin,terrain->origin,sizeof(in.origin));in.read=rc_grid_read;in.view=rc_grid_view;in.read_user=&reader;
 if(!rc_cpu)rc_cpu=mxdirt_create();if(!rc_cpu)return 0;rc_serial=scope->serial;
 uint64_t start=rc_stamp();MxDirtWindow window;rc_part[0]=rc_part[1]=rc_part[2]=0;
 if(!mxdirt_update(rc_cpu,&in,&window,&rc_stats)){rc_ticks=rc_elapsed(start);return 0;}
 rc_part[0]=rc_elapsed(start);
 if(window.recentered)rc_recentres++;
 memcpy(rc_cam,window.camera,sizeof(rc_cam));rc_recentred=window.recentered!=0;
 /* Identity brackets the sparse reads. */
 if(!rc_identity(terrain,frame)){rc_ticks=rc_elapsed(start);return 0;}
 if(!rc_gl){if(!mxdirt_gl_create(&rc_gl,frame->generation)){int stage=0,major=0,profile=0;mxdirt_gl_last_failure(&stage,&major,&profile);
   const char *ver=(const char*)glGetString(GL_VERSION),*ren=(const char*)glGetString(GL_RENDERER);rc_dirt_fault=1;
   mxdirt_content_note("dirt_disabled resource_create=1 stage=%d gl_major=%d profile_mask=%d gl_version=%s gl_renderer=%s",stage,major,profile,ver?ver:"?",ren?ren:"?");return -1;}}
 /* The dirt sim's own per-texel state; without it the pass shades from the height delta only. */
 MxDirtState state;memset(&state,0,sizeof(state));rc_state_live=0;
 if(frame->sim_comp){MxDirtSim sim={frame->sim_comp,frame->sim_loose,frame->sim_moist,frame->sim_clods,frame->sim_size,frame->sim_vertex_exact,frame->sim_moisture,frame->sim_compaction0,frame->sim_steps};
  rc_state_live=mxdirt_state(rc_cpu,&window,terrain->width,terrain->height,&sim,&state);}
 rc_part[1]=rc_elapsed(start)-rc_part[0];
 /* The core's soft top layer (look only), when both sides have it on. */
 MxDirtSoftInput soft;memset(&soft,0,sizeof(soft));int soft_on=0;
 if(rc_soft&&frame->soft_tile&&frame->soft_tile_serial){
  soft.tile=frame->soft_tile;soft.serial=frame->soft_tile_serial;soft.tiles_x=frame->soft_tiles_x;soft.tiles_z=frame->soft_tiles_z;soft.side=frame->soft_tile_side;
  soft.cell=frame->soft_cell_m;soft.thickness=frame->soft_thickness_m;soft.unit=(float)frame->soft_unit_um*1e-6f;soft.origin_x=frame->origin_m[0];soft.origin_z=frame->origin_m[2];soft.solid=rc_soft_solid;soft.sink=rc_tyre_sink;soft_on=1;}
 rc_soft_live=soft_on;
 int result=mxdirt_gl_draw(rc_gl,scope,&window,&rc_look,&state,soft_on?&soft:NULL);rc_ticks=rc_elapsed(start);
 rc_part[2]=rc_ticks-rc_part[0]-rc_part[1];
 if(rc_ticks>rc_peak){rc_peak=rc_ticks;memcpy(rc_peak_part,rc_part,sizeof(rc_part));}
 if(result<0){rc_dirt_fault=1;mxdirt_content_note("dirt_disabled resource_or_owner=1");}return result;
}
int mxdirt_content_draw(const MxDirtCameraScope *scope,const MxDirtTsTerrain *terrain){
 DWORD error=GetLastError();int result=0;MxDirtFrame frame;uint64_t began=rc_stamp();
 if(!rc_started||!rc_live||rc_fault||GetCurrentThreadId()!=rc_thread||!scope||!terrain||scope->thread!=rc_thread||
    !scope->context||scope->context!=wglGetCurrentContext()||!scope->complete||!scope->serial)goto end;
 if(!rc_cleanup()||!rc_frame(&frame)||frame.generation!=scope->generation){if(rc_skips++<4)mxdirt_content_note("dirt_waiting core_frame=0");goto end;}
 if(rc_frames%MXD_FR_FRAMES==0)mxd_fr_expire(&rc_reads);
 rc_key_poll();
 if((rc_frames&63u)==63u&&rc_ini_changed()){const int ok=rc_look_read(0); /* about twice a second: one file-time query */
  mxdirt_content_note("look_settings read=%d dirt_material=%d strength=%.0f loose=%.0f fade=%.0f..%.0f relief=%.0f soft_layer=%d",ok,rc_dirt,(double)rc_look.strength*100.,(double)rc_look.base_loose*100.,(double)rc_look.fade_start,(double)rc_look.fade_end,(double)rc_look.relief*100.,rc_soft);}
 uint64_t t=rc_stamp();
 int identity=rc_identity(terrain,&frame)&&!(rc_generation&&(rc_generation!=frame.generation||!rc_same(&rc_bound,terrain)));
 uint64_t identity_ticks=rc_elapsed(t);
 if(!identity){rc_fault=1;result=-1;
  mxdirt_content_note("dirt_disabled identity=1 core_grid=%ux%u span=%.4f,%.4f origin=%.4f,%.4f,%.4f native_grid=%ux%u extent=%.4f,%.4f origin=%.4f,%.4f,%.4f flags=%u",
   frame.grid_width,frame.grid_height,(double)frame.span_x_m,(double)frame.span_z_m,(double)frame.origin_m[0],(double)frame.origin_m[1],(double)frame.origin_m[2],
   terrain->width,terrain->height,(double)terrain->extent_x,(double)terrain->extent_z,(double)terrain->origin[0],(double)terrain->origin[1],(double)terrain->origin[2],terrain->flags);
  goto end;}
 if(!rc_generation){rc_bound=*terrain;rc_generation=frame.generation;}
 /* the gap that ended here; rc_ticks, rc_stats and rc_cam still describe the frame before it */
 {uint64_t gap=rc_last_draw&&began>rc_last_draw?began-rc_last_draw:0;rc_last_draw=began;double gap_ms=gap*rc_ms_per_tick;
  if(rc_frames>8){if(gap>rc_gap_peak)rc_gap_peak=gap;if(gap_ms>20.)rc_gap20++;if(gap_ms>33.)rc_gap33++;
   if(gap_ms>40.&&rc_hitches<400){rc_hitches++;mxdirt_content_note("hitch after_frame=%llu gap_ms=%.1f prev_dirt_ms=%.3f prev_recentred=%d prev_changed=%u sim_steps=%llu cam=%.0f,%.0f",
    (unsigned long long)rc_frames,gap_ms,rc_ticks*rc_ms_per_tick,rc_recentred,rc_stats.changed,(unsigned long long)frame.sim_steps,(double)rc_cam[0],(double)rc_cam[2]);}}}
 int dirt_result=rc_draw_dirt(&frame,scope,terrain);
 rc_frames++;result=dirt_result>0?1:dirt_result;
 {double g=0;if(rc_gl&&mxdirt_gl_gpu(rc_gl,&g,NULL,NULL)){rc_gpu_sum+=g;rc_gpu_n++;if(g>rc_gpu_peak)rc_gpu_peak=g;}}
 if(rc_frames<=8||rc_frames%120==0){
  int fbo=0,samples=0,skip=0,vw=0,vh=0;mxdirt_gl_target(rc_gl,&fbo,&samples,&skip);mxdirt_gl_gpu(rc_gl,NULL,&vw,&vh);
  mxdirt_content_note("dirt frame=%llu generation=%llu serial=%llu soft=%d sim_state=%d sim_steps=%llu result=%d stage=%u reads=%u read_bytes=%u changed=%u recentres=%llu identity_ms=%.3f dirt_ms=%.3f dirt_max_ms=%.3f content_ms=%.3f fbo=%d samples=%d skip=%d gap_max_ms=%.1f gap20=%u gap33=%u gpu_ms=%.3f gpu_max_ms=%.3f gpu_n=%u cam=%.0f,%.0f vp=%dx%d worst_update_ms=%.3f worst_state_ms=%.3f worst_gl_ms=%.3f",
   (unsigned long long)rc_frames,(unsigned long long)frame.generation,(unsigned long long)scope->serial,rc_soft_live,rc_state_live,(unsigned long long)frame.sim_steps,dirt_result,rc_stats.stage,rc_stats.read_calls,rc_stats.read_bytes,rc_stats.changed,
   (unsigned long long)rc_recentres,identity_ticks*rc_ms_per_tick,rc_ticks*rc_ms_per_tick,rc_peak*rc_ms_per_tick,rc_elapsed(began)*rc_ms_per_tick,fbo,samples,skip,
   rc_gap_peak*rc_ms_per_tick,rc_gap20,rc_gap33,rc_gpu_n?rc_gpu_sum/rc_gpu_n:0.,rc_gpu_peak,rc_gpu_n,(double)rc_cam[0],(double)rc_cam[2],vw,vh,
   rc_peak_part[0]*rc_ms_per_tick,rc_peak_part[1]*rc_ms_per_tick,rc_peak_part[2]*rc_ms_per_tick);
  rc_peak=0;rc_gap_peak=0;rc_gap20=rc_gap33=rc_gpu_n=0;rc_gpu_sum=rc_gpu_peak=0;}
end:SetLastError(error);return result;
}
void mxdirt_content_lifecycle(uint32_t event,uint64_t generation){
 DWORD error=GetLastError();if(!rc_started||GetCurrentThreadId()!=rc_thread)goto end;
 mxd_fr_expire(&rc_reads);
 mxdirt_content_note("lifecycle event=%u generation=%llu",event,(unsigned long long)generation);
 if(event==MXDIRT_EVENT_BEGIN){rc_live=1;rc_fault=0;rc_frames=rc_skips=rc_recentres=0;rc_peak=0;rc_last_draw=0;rc_hitches=0;if(rc_generation||rc_cpu||rc_gl)rc_retire=1;(void)rc_cleanup();}
 else if(event==MXDIRT_EVENT_RETIRE||event==MXDIRT_SHUTDOWN){rc_live=0;rc_retire=1;(void)rc_cleanup();}
 /* Pausing (RunStop) keeps history and event identity; no teardown. */
 if(event==MXDIRT_SHUTDOWN&&rc_log){fclose(rc_log);rc_log=NULL;}
end:SetLastError(error);
}
