/* Lap recordings (mxb_dirt_ghost.h .dghost format): record every real tyre
 * contact of a lap (owner thread, after a completed dirt frame), save each valid
 * lap at RunLap to
 *   plugins\mx_dirt_laps\<track>\<lap ms>_<date>.dghost
 * and at session setup load the fastest laps of this track for pre-wear and
 * [soil] ghost_riders. If that folder has no laps for the track, laps recorded
 * by pre-release development builds in plugins\mxb_reflex_ghosts\<track>\ are read instead
 * (read-only). Laps are freed only after the session that borrows them is
 * destroyed (ra_solver_destroy). Included by mx_dirt.c. */
static MxbDirtGhostSample *ra_rec;static uint32_t ra_rec_n,ra_rec_cap;static float ra_rec_t;static int ra_rec_overflow;
static unsigned ra_ghost_riders=0,ra_record_ghosts=1;
static MxbDirtGhostLap ra_ghosts[MXDIRT_SESSION_MAX_GHOSTS];static unsigned ra_ghost_n;
static char ra_ghost_bike[64];
static void ra_ghost_free(void){for(unsigned i=0;i<ra_ghost_n;i++)mxb_dirt_ghost_free(ra_ghosts+i);ra_ghost_n=0;}
static void ra_ghost_recording_reset(void){ra_rec_n=0;ra_rec_t=0.f;ra_rec_overflow=0;}
/* plugins\<root>\<track> under the game folder; created on demand when create. */
static int ra_lap_dir(const char *root,char *out,size_t cap,int create){
 char rel[MAX_PATH];if(!ra_event_track[0]||snprintf(rel,sizeof(rel),"plugins\\%s",root)>=(int)sizeof(rel))return 0;
 DWORD n=mxb_game_path_a(rel,(DWORD)strlen(rel),sizeof(rel));if(!n)return 0;
 if(create)CreateDirectoryA(rel,NULL);
 char track[100];size_t k=0;for(const char *c=ra_event_track;*c&&k<sizeof(track)-1;c++)track[k++]=(strchr("\\/:*?\"<>|",*c)?'_':*c);track[k]=0;
 if(!k||snprintf(out,cap,"%s\\%s",rel,track)>=(int)cap)return 0;
 if(create)CreateDirectoryA(out,NULL);return 1;
}
/* After each completed dirt frame: append this frame's real contacts. */
static void ra_ghost_record_frame(float dt){
 if(!ra_record_ghosts||!ra_session||!(dt>0.f)||dt>.25f||ra_rec_overflow)return;
 MxbDirtGhostSample live[2];int ok[2];if(!mxdirt_session_live_contacts(ra_session,live,ok))return;
 ra_rec_t+=dt;
 for(int w=0;w<2;w++){if(!ok[w])continue;
  if(ra_rec_n>=MXB_DIRT_GHOST_MAX_SAMPLES){ra_rec_overflow=1;return;}
  if(ra_rec_n==ra_rec_cap){uint32_t cap=ra_rec_cap?ra_rec_cap*2:16384;if(cap>MXB_DIRT_GHOST_MAX_SAMPLES)cap=MXB_DIRT_GHOST_MAX_SAMPLES;
   MxbDirtGhostSample *p=realloc(ra_rec,(size_t)cap*sizeof(*p));if(!p){ra_rec_overflow=1;return;}ra_rec=p;ra_rec_cap=cap;}
  live[w].t=ra_rec_t;if(mxb_dirt_ghost_sample_ok(live+w))ra_rec[ra_rec_n++]=live[w];
 }
}
/* MX Bikes plugin API lap record (SPluginsBikeLap_t). */
typedef struct {int lap_num,invalid,lap_time_ms,best;} RaLap;
API void RunLap(void *data,int size){
 if(GetCurrentThreadId()!=ra_owner||!data||size<(int)sizeof(RaLap))return;
 RaLap lap;memcpy(&lap,data,sizeof(lap));
 int saved=0;char path[MAX_PATH]="";
 if(ra_record_ghosts&&!lap.invalid&&lap.lap_time_ms>10000&&lap.lap_time_ms<3600000&&!ra_rec_overflow&&ra_rec_n>=200&&
    ra_rec_t>.5f*lap.lap_time_ms/1000.f){
  char dir[MAX_PATH];SYSTEMTIME st;GetLocalTime(&st);
  if(ra_lap_dir(RA_LAPS_DIR,dir,sizeof(dir),1)&&snprintf(path,sizeof(path),"%s\\%07d_%04u%02u%02u-%02u%02u%02u.dghost",dir,lap.lap_time_ms,
     st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond)<(int)sizeof(path)){
   MxbDirtGhostHeader h;memset(&h,0,sizeof(h));h.magic=MXB_DIRT_GHOST_MAGIC;h.version=MXB_DIRT_GHOST_VERSION;
   h.sample_bytes=sizeof(MxbDirtGhostSample);h.samples=ra_rec_n;h.lap_ms=lap.lap_time_ms;h.lap_num=lap.lap_num;
   strncpy(h.track,ra_event_track,sizeof(h.track)-1);strncpy(h.bike,ra_ghost_bike,sizeof(h.bike)-1);
   saved=mxb_dirt_ghost_write(path,&h,ra_rec);
  }
 }
 if(ra_log){fprintf(ra_log,"{\"kind\":\"lap_recording\",\"lap\":%d,\"invalid\":%d,\"lap_ms\":%d,\"samples\":%u,\"recorded_s\":%.2f,\"overflow\":%d,\"saved\":%d}\n",
  lap.lap_num,lap.invalid,lap.lap_time_ms,ra_rec_n,(double)ra_rec_t,ra_rec_overflow,saved);fflush(ra_log);}
 ra_ghost_recording_reset();
}
/* Up to 64 .dghost names of one folder, sorted (zero-padded lap time first =
   fastest first). Returns the count. */
static unsigned ra_lap_list(const char *dir,char names[64][MAX_PATH]){
 char pattern[MAX_PATH];if(snprintf(pattern,sizeof(pattern),"%s\\*.dghost",dir)>=(int)sizeof(pattern))return 0;
 unsigned count=0;WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA(pattern,&fd);
 if(h!=INVALID_HANDLE_VALUE){do{if(!(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)&&count<64)snprintf(names[count++],MAX_PATH,"%s",fd.cFileName);}while(FindNextFileA(h,&fd));FindClose(h);}
 for(unsigned i=1;i<count;i++)for(unsigned j=i;j>0&&strcmp(names[j-1],names[j])>0;j--){char t[MAX_PATH];memcpy(t,names[j],MAX_PATH);memcpy(names[j],names[j-1],MAX_PATH);memcpy(names[j-1],t,MAX_PATH);}
 return count;
}
/* At session setup: the fastest recorded laps of this track - ridden once by
   the pre-wear gate, then kept as live ghost riders if ghost_riders>0. */
static char ra_lap_names[64][MAX_PATH];
static void ra_ghost_load(void){
 ra_ghost_recording_reset();
 if((!ra_ghost_riders&&!ra_prewear_riders)||!ra_session)return;
 char dir[MAX_PATH];unsigned count=0;const char *source=RA_LAPS_DIR;
 if(ra_lap_dir(RA_LAPS_DIR,dir,sizeof(dir),0))count=ra_lap_list(dir,ra_lap_names);
 if(!count&&ra_lap_dir(RA_OLD_LAPS_DIR,dir,sizeof(dir),0)){count=ra_lap_list(dir,ra_lap_names);source=RA_OLD_LAPS_DIR;}
 unsigned want=ra_ghost_riders<MXDIRT_SESSION_MAX_GHOSTS?ra_ghost_riders:MXDIRT_SESSION_MAX_GHOSTS;
 if(ra_prewear_riders&&want<4)want=4; /* pre-wear rides up to the 4 fastest laps */
 for(unsigned i=0;i<count&&ra_ghost_n<want;i++){char path[MAX_PATH];
  if(snprintf(path,sizeof(path),"%s\\%s",dir,ra_lap_names[i])<(int)sizeof(path)&&mxb_dirt_ghost_load(path,ra_event_track,ra_ghosts+ra_ghost_n))ra_ghost_n++;}
 /* A track saved after a ride already holds its pre-wear; wear it again only when fresh
    (more than ~300 m2 moved over 1.5 cm = a ridden track; prewear_fresh_only=0 restores stacking) */
 uint64_t saved=ra_prewear_riders?mxdirt_session_saved_wear(ra_session,.015):0;int skip=ra_prewear_fresh_only&&saved>4000;
 if(ra_prewear_riders&&skip&&ra_log){fprintf(ra_log,"{\"kind\":\"prewear\",\"ok\":0,\"skipped\":\"saved_wear\",\"texels\":%llu}\n",(unsigned long long)saved);fflush(ra_log);}
 if(ra_prewear_riders&&!skip){LARGE_INTEGER a,b,f;QueryPerformanceCounter(&a);uint64_t passes=0;
  int pw=ra_ghost_n&&mxdirt_session_prewear(ra_session,ra_ghosts,ra_ghost_n<4?ra_ghost_n:4,ra_prewear_riders,
     (float)ra_prewear_spread_cm/100.f,(float)ra_prewear_clip_cm/100.f,0x9e3779b9u,&passes);
  QueryPerformanceCounter(&b);QueryPerformanceFrequency(&f);
  if(ra_log){fprintf(ra_log,"{\"kind\":\"prewear\",\"ok\":%d,\"riders\":%u,\"laps\":%u,\"passes\":%llu,\"ms\":%.0f}\n",pw,ra_prewear_riders,ra_ghost_n<4?ra_ghost_n:4,(unsigned long long)passes,(double)(b.QuadPart-a.QuadPart)*1000./(double)f.QuadPart);fflush(ra_log);}}
 unsigned live=ra_ghost_n<ra_ghost_riders?ra_ghost_n:ra_ghost_riders;
 int ok=live?mxdirt_session_set_ghosts(ra_session,ra_ghosts,live):1;
 if(ra_log){fprintf(ra_log,"{\"kind\":\"ghost_riders\",\"requested\":%u,\"folder\":\"%s\",\"files\":%u,\"loaded\":%u,\"accepted\":%d",ra_ghost_riders,source,count,ra_ghost_n,ok);
  for(unsigned i=0;i<ra_ghost_n;i++)fprintf(ra_log,"%s%d",i?",":",\"lap_ms\":[",ra_ghosts[i].h.lap_ms);fprintf(ra_log,"%s}\n",ra_ghost_n?"]":"");fflush(ra_log);}
 if(!ok||!live)ra_ghost_free(); /* laps only used by pre-wear are released now */
}
