#ifndef MXB_DIRT_GHOST_H
#define MXB_DIRT_GHOST_H
/* Dirt ghosts: recorded tyre contacts of a real lap, replayed later by the
   dirt simulation as invisible riders that work the soil exactly as that lap
   did (line, load, braking/drive slip, bump gain). Coordinates are the
   terrain-centred native units the soil sim already uses, so a ghost is only
   valid on the track it was recorded on (track id in the header).
   File (little endian): MxbDirtGhostHeader, then `samples` MxbDirtGhostSample
   in time order. Header-only; no allocation outside the load helper. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define MXB_DIRT_GHOST_MAGIC 0x54484744u /* "DGHT" */
#define MXB_DIRT_GHOST_VERSION 1u
#define MXB_DIRT_GHOST_MAX_SAMPLES 400000u /* ~55 min at 60 fps x 2 wheels */
typedef struct {
    uint32_t magic,version,sample_bytes,samples;
    int32_t lap_ms,lap_num;uint32_t reserved[2];
    char track[96];char bike[64];
} MxbDirtGhostHeader;
typedef struct {
    float t;                       /* s since the lap started */
    float x,z,fwd[2],lat[2];       /* native contact point and axes (MxbSoilSimWheel) */
    float speed,slip_long,slip_lat,load,width,radius,gain,dt;
    uint8_t wheel,material,pad[2];
} MxbDirtGhostSample;
typedef struct {MxbDirtGhostHeader h;MxbDirtGhostSample *s;} MxbDirtGhostLap;
static inline int mxb_dirt_ghost_sample_ok(const MxbDirtGhostSample *g){
    return g->wheel<2&&isfinite(g->t)&&g->t>=0.f&&isfinite(g->x)&&isfinite(g->z)&&isfinite(g->speed)&&
           isfinite(g->slip_long)&&isfinite(g->slip_lat)&&isfinite(g->load)&&g->load>0.f&&g->load<60000.f&&
           isfinite(g->gain)&&isfinite(g->dt)&&g->dt>0.f&&g->dt<=.1f&&g->width>0.f&&g->width<1.f&&g->radius>0.f&&g->radius<1.f;
}
static inline int mxb_dirt_ghost_write(const char *path,const MxbDirtGhostHeader *h,const MxbDirtGhostSample *s){
    FILE *f=fopen(path,"wb");if(!f)return 0;
    int ok=fwrite(h,sizeof(*h),1,f)==1&&(!h->samples||fwrite(s,sizeof(*s),h->samples,f)==h->samples);
    return fclose(f)==0&&ok;
}
/* Loads and validates one ghost; returns 1 with out->s malloc'd (free with mxb_dirt_ghost_free). */
static inline int mxb_dirt_ghost_load(const char *path,const char *track,MxbDirtGhostLap *out){
    memset(out,0,sizeof(*out));FILE *f=fopen(path,"rb");if(!f)return 0;
    MxbDirtGhostHeader h;int ok=0;MxbDirtGhostSample *s=NULL;
    if(fread(&h,sizeof(h),1,f)!=1||h.magic!=MXB_DIRT_GHOST_MAGIC||h.version!=MXB_DIRT_GHOST_VERSION||
       h.sample_bytes!=sizeof(MxbDirtGhostSample)||!h.samples||h.samples>MXB_DIRT_GHOST_MAX_SAMPLES)goto end;
    h.track[sizeof(h.track)-1]=0;h.bike[sizeof(h.bike)-1]=0;if(track&&strcmp(h.track,track))goto end;
    s=malloc((size_t)h.samples*sizeof(*s));if(!s||fread(s,sizeof(*s),h.samples,f)!=h.samples)goto end;
    for(uint32_t i=0;i<h.samples;i++)if(!mxb_dirt_ghost_sample_ok(s+i)||(i&&s[i].t<s[i-1].t))goto end;
    out->h=h;out->s=s;s=NULL;ok=1;
end:free(s);fclose(f);return ok;
}
static inline void mxb_dirt_ghost_free(MxbDirtGhostLap *g){if(g){free(g->s);g->s=NULL;}}
#endif
