#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include "mxdirt_session.h"
#include "mxb_soil_sim.h"
#include "mxb_soil_prewear.h"
#include "mxb_soil_live_gate.h"
#include "mxdirt_soil_evolution.h"
#include "mxdirt_soft_layer.h"

typedef struct {float y,z,zd;int valid;MxbBumpSit sit;float off,sw,gs,gb,ge;int landed;} MxbBumpState; /* off: seconds without ground contact, sw: filtered swing, g*: this frame's stress/braking/energy for the live gate */
struct MxdirtSession {
    MxdirtSessionConfig c;
    MxdirtContactBridge wheel[2];
    MxdirtHeightField *height;
    float *seed;const float *encoded;int32_t *target,*accepted;
    MxbPubAbsoluteCell *changes;
    uint32_t *pending_begin,*pending_end;
    size_t cells,source_cells,tile_count;
    uint64_t generated_sequence,accepted_sequence,accepted_source_sequence,outer_sequence;
    DWORD owner;int priming,faulted,in_callback,physics_seen;
    int vertex_exact; /* sharp ruts: vertex-exact publication, the dirt sim targets whole vertices */
    float bump_gain;MxbBumpState bump[2];
    float rolling_bumps;           /* bump stress with no braking/drive (rollers from any wheel hop) */
    float bias_floor_m;            /* washboard trough bound for live passes (scales with erode) */
    unsigned extra_riders;         /* extra riders replaying each real contact on spread lines (test/busy-track mode) */
    MxbLiveGate gate;              /* live gate share (mxb_soil_live_gate.h) */
    int force_publish;             /* cells marked for resend (adopted tiles) */
    /* Real tyre contacts of the last completed step, for lap recording. */
    MxbDirtGhostSample live[2];int live_ok[2];
    /* Ghost playback: caller-owned recorded laps, each on its own clock,
       with its own per-wheel floor memory (swapped into the sim around its passes). */
    const MxbDirtGhostLap *ghost_laps;unsigned ghost_count;
    struct {uint32_t cursor;float clock,last_x[2],last_z[2],last_floor[2];int has_last[2];uint64_t passes;} ghost[MXDIRT_SESSION_MAX_GHOSTS];
    MxbSoilSim *sim;
    MxbSoilEvolution *soil; /* work history: packed/loose/brake/drive -> cut gain (mxdirt_soil_evolution.h) */
    unsigned char *prewear_tiles; /* tiles worn by the pre-wear gate, published with the next step */
    MxdSoft *soft;                /* soft top layer, display only (mxdirt_soft_layer.h) */
    /* Per-stage step timing (QPC ticks): contacts+bumps, dirt sim, height conversion. */
    uint64_t timing_frames,timing_ticks[3],timing_max[3];
};
static uint64_t qpc_now(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return (uint64_t)t.QuadPart;}
static void timing_add(MxdirtSession *s,unsigned k,uint64_t ticks){s->timing_ticks[k]+=ticks;if(ticks>s->timing_max[k])s->timing_max[k]=ticks;}
int mxdirt_session_step_timing(const MxdirtSession *s,uint64_t *frames,double total_ms[3],double max_ms[3]){
    if(!s||!frames||!total_ms||!max_ms)return 0;LARGE_INTEGER f;if(!QueryPerformanceFrequency(&f)||f.QuadPart<=0)return 0;
    *frames=s->timing_frames;for(int k=0;k<3;k++){total_ms[k]=(double)s->timing_ticks[k]*1000./(double)f.QuadPart;max_ms[k]=(double)s->timing_max[k]*1000./(double)f.QuadPart;}return 1;
}
/* Bump stress. A per-wheel spring-damper (tyre + unsprung hop, ~8 Hz) is driven
   by the ground height under the contact, which already includes our published
   ruts; hub acceleration / g is the wheel-load swing. When the contact has raw
   MX Bikes wheel loads for this frame, the swing is the measured one instead
   (frame mean / ~0.6 s mean - 1): the bike's own tyre, fork/shock and rider
   response pick the spacing. The spring model is the fallback for frames
   without load samples.
   *bias = this pass's washboard floor offset (m): the rut floor follows the
   wheel's own load swing, so rows grow where the bike actually bounces -
   downstream of landings, rollers and rut crossings. Scaled with erode
   (depth / .5) and braking_bumps. The stress follows the situation
   (mxb_soil_bumps.h, shared with the pre-wear gate): braking rows tallest with
   deeper troughs (*floor), turning / just landed / under power calmer. */
static void bump_reset(MxbBumpState *b,float y){b->y=b->z=y;b->zd=0.f;b->valid=1;}
static float bump_factor(const MxdirtSession *s,const MxdirtContact *i,MxbBumpState *b,float *bias,float *floor){
    *bias=0.f;*floor=s->bias_floor_m;b->gs=b->gb=0.f;b->ge=1.f;b->landed=0;
    float m=(float)s->c.height.native_metres,y=i->point[1]*m,dt=i->dt;
    float speed=hypotf(i->center_velocity[0],i->center_velocity[2])*m;
    if(!isfinite(y)||!isfinite(speed)||!(dt>0.f)||dt>.1f||!i->contact||!i->deformation||speed<2.f){
        if(b->valid)b->off=0.f;b->valid=0;if(isfinite(dt)&&dt>0.f&&dt<.1f)b->off+=dt;return 1.f;}
    int landed=0;if(!b->valid){landed=b->off>.2f;b->off=0.f;}
    if(landed)b->sit.since=0.f;b->landed=landed;
    if(!b->valid||fabsf(y-b->y)>.25f){bump_reset(b,y);return 1.f;}
    const float w0=2.f*3.14159265f*8.f,zeta=.35f;const unsigned n=8;
    float h=dt/n,y0=b->y,yd=(y-y0)/dt,z=b->z,zd=b->zd,a=0.f;
    for(unsigned k=0;k<n;k++){
        float yk=y0+(y-y0)*(float)(k+1)/(float)n;
        a=w0*w0*(yk-z)+2.f*zeta*w0*(yd-zd);zd+=a*h;z+=zd*h;
    }
    if(!isfinite(z)||!isfinite(zd)||fabsf(z-y)>.25f){bump_reset(b,y);return 1.f;}
    b->y=y;b->z=z;b->zd=zd;
    float swing=fmaxf(-1.f,fminf(1.f,a/9.81f));
    if(i->frame_count&&i->frame_slow_n>50.f&&isfinite(i->frame_mean_n))
        swing=fmaxf(-1.f,fminf(1.f,.7f*(i->frame_mean_n/i->frame_slow_n-1.f)));
    /* low-pass (~4.5 Hz): the chassis bounce builds long rollers; the ~10 Hz wheel hop
       in the measured load only cut short chop Pro does not have */
    b->sw+=fminf(1.f,dt/.035f)*(swing-b->sw);swing=b->sw;
    float slip_speed=(i->slip[0]*i->forward[0]+i->slip[2]*i->forward[2])*m;
    float slip=isfinite(slip_speed)?fmaxf(-1.f,fminf(1.f,slip_speed/fmaxf(speed,1.f))):0.f;
    /* Measured on Maryland and Mt Morris Pro vs Ams, bumps cover the whole riding
       line - braking zones are only ~1.5x rougher. Every wheel hop works the
       ground (rollers / washboard), so the floor is rolling_bumps (default .6). */
    float brk,ef,stress=mxb_bump_stress(&b->sit,dt,speed,slip,i->center_velocity[0],i->center_velocity[2],y,0,s->rolling_bumps,&brk,&ef);
    float dn=s->sim?fmaxf(.3f,fminf(2.5f,s->sim->p.depth_scale/.5f)):1.f;
    *bias=-.021f*dn*(s->bump_gain/.30f)*stress*swing*(1.f+2.f*brk); /* .6x - the player's own line kept growing pits */
    /* one pass deepens a bump dip by at most 1.2 cm (x erode): a single hard braking hit on
       fresh dirt dug an instant pothole; real braking bumps grow a little with every pass */
    {const float cap=.012f*dn;if(*bias<-cap)*bias=-cap;if(*bias>cap)*bias=cap;} /* braking hits 3x (see mxb_soil_live_gate.h) */
    *floor=s->bias_floor_m*(1.f+10.5f*brk)*fminf(ef,1.8f); /* live braking troughs up to 3x the pre-wear's depth limit (.04+.42 brk) */
    b->gs=stress*(MXB_PW_ROLLING/.6f);b->gb=brk;b->ge=ef; /* gate lines use the pre-wear rolling stress */
    return 1.f+(s->bump_gain+.3f*brk)*fminf(stress,1.5f)*swing;
}
static int owned(const MxdirtSession *s) {
    return s&&s->owner==GetCurrentThreadId()&&!s->faulted&&!s->in_callback;
}
static int fresh(const MxdirtSession *s){return owned(s)&&!s->generated_sequence&&!s->outer_sequence&&!s->physics_seen;}
void mxdirt_session_destroy(MxdirtSession *s) {
    if(!s||s->in_callback||(s->owner&&s->owner!=GetCurrentThreadId()))return;
    mxdirt_height_field_destroy(s->height);
    mxb_soil_sim_destroy(s->sim);mxb_soil_destroy(s->soil);free(s->prewear_tiles);mxd_soft_destroy(s->soft);
    free(s->seed);free(s->target);free(s->accepted);free(s->changes);
    free(s->pending_begin);free(s->pending_end);free(s);
}
static void clear_candidates(MxdirtSession *s){
    for(uint32_t z=0;z<s->c.height.destination_height;z++){
        s->pending_begin[z]=UINT32_MAX;s->pending_end[z]=0;
    }
}
int mxdirt_session_create(MxdirtSession **out,const MxdirtSessionConfig *c,
    const int16_t *base,const int32_t *initial,size_t count) {
    if(!out)return 0;*out=NULL;
    if(!c||!base||!initial||!c->sparse_capacity||
       c->height.generation!=c->contact.generation||
       c->height.source_width!=c->height.source_height||
       c->height.source_width!=c->height.source_stride||
       c->height.source_width<128||c->height.source_width%128||
       !isfinite(c->height.native_metres)||!isfinite(c->contact.native_length_per_metre)||
       fabs(c->height.native_metres*c->contact.native_length_per_metre-1.)>1.e-6)return 0;
    size_t cells=(size_t)c->height.destination_width*c->height.destination_height;
    if(cells>34000000||count<cells||c->sparse_capacity>cells)return 0;
    MxdirtSession *s=calloc(1,sizeof(*s));if(!s)return 0;
    s->c=*c;s->owner=GetCurrentThreadId();s->cells=cells;s->priming=1;s->rolling_bumps=.6f;mxb_live_gate_init(&s->gate,5,0x51ed270bu);
    s->source_cells=(size_t)c->height.source_width*c->height.source_height;
    s->tile_count=(c->height.source_width/128)*(c->height.source_height/32);
    if(!mxdirt_contact_init(&s->wheel[0],&c->contact,0)||
       !mxdirt_contact_init(&s->wheel[1],&c->contact,1)||
       !mxdirt_height_field_create(&s->height,&c->height,base,initial,count)||
       !mxb_soil_create(&s->soil,c->height.native_span_x,c->height.native_span_z,(float)c->height.native_metres))goto fail;
    s->seed=malloc(s->source_cells*sizeof(float));s->encoded=s->seed;s->target=malloc(cells*sizeof(int32_t));
    s->accepted=malloc(cells*sizeof(int32_t));
    s->changes=malloc(c->sparse_capacity*sizeof(*s->changes));
    s->pending_begin=malloc(c->height.destination_height*sizeof(*s->pending_begin));
    s->pending_end=malloc(c->height.destination_height*sizeof(*s->pending_end));
    if(!s->seed||!s->target||!s->accepted||!s->changes||!s->pending_begin||!s->pending_end)goto fail;
    clear_candidates(s);
    for(size_t j=0;j<s->source_cells;j++)s->seed[j]=c->height.reference_encoded;
    memcpy(s->accepted,initial,cells*sizeof(*initial));
    if(!mxdirt_height_field_convert(s->height,c->height.generation,s->encoded,s->source_cells,s->target,cells,NULL))goto fail;
    /* Neutral source must reproduce the initial DFM exactly. */
    if(memcmp(s->target,initial,cells*sizeof(*initial)))goto fail;
    *out=s;return 1;
fail:mxdirt_session_destroy(s);return 0;
}
int mxdirt_session_set_depth_response(MxdirtSession *s,float response) {
    return fresh(s)&&!s->sim&&mxdirt_height_field_set_depth_response(s->height,response);
}
/* Dirt simulation. depth_percent scales sinkage/roost (10..300);
   firmness_percent sets untouched-track compaction (0 loose..100 hardpack). */
int mxdirt_session_start_sim(MxdirtSession *s,float depth_percent,float firmness_percent){
    if(!fresh(s)||s->sim||!isfinite(depth_percent)||depth_percent<10.f||depth_percent>300.f||
       !isfinite(firmness_percent)||firmness_percent<0.f||firmness_percent>100.f)return 0;
    float m=(float)s->c.height.native_metres;
    float per_unit=2.f*(float)s->c.height.native_height_scale*mxdirt_height_field_depth_response(s->height)*m;
    s->sim=mxb_soil_sim_create(s->c.height.source_width,(float)s->c.height.native_span_x*m,(float)s->c.height.native_span_z*m,per_unit,s->c.height.reference_encoded,m);
    if(!s->sim)return 0;
    s->sim->texel_off=s->vertex_exact?1.f:.5f;
    /* A softer, more erodible track (erode slider) is ripped deeper: the hard base
       sits at .20 m at the calibration (erode .25) and scales with sqrt of the depth
       response (.15 m at erode .09, .27 m at erode .81) */
    s->sim->p.hardpan=.20f*sqrtf(fmaxf(.5f,fminf(2.5f,depth_percent/50.f)));
    s->bias_floor_m=.04f*fmaxf(.3f,fminf(2.5f,depth_percent/50.f)); /* washboard trough bound, same erode scale */
    s->sim->p.depth_scale=depth_percent/100.f;s->sim->p.compaction0=.12f+.35f*firmness_percent/100.f; /* untouched ground is a freshly ripped track (55 % -> .31): first passes cut soft, deep grooves that pack with traffic */
    /* Track surface height (m) at texel centres from the loaded terrain, so loose
       soil slides on real slopes. The track SHAPE is the int16 physics base
       (metres = base*vs/65535); the saved deformation in the DFM is added on top
       (DFM falls as height rises). Destination rows run with +z, texel rows with
       -z. Optional: on failure loose soil relaxes against the deformation only. */
    uint32_t dw=s->c.height.destination_width,dh=s->c.height.destination_height,n=s->sim->size;
    const int16_t *shape=mxdirt_height_field_base(s->height);
    if(dw>=2&&dh>=2&&s->accepted&&shape&&s->c.height.mxb_vertical_scale>0.){
        float *b=malloc((size_t)n*n*sizeof(float));
        if(b){
            double k=s->c.height.mxb_vertical_scale/(65535.*65536.);const float off=s->sim->texel_off;
            for(uint32_t r=0;r<n;r++){
                float v=(1.f-((float)r+off)/n)*(dh-1);int j=(int)v;if(j>(int)dh-2)j=dh-2;float fv=v-j;
                for(uint32_t q=0;q<n;q++){
                    float u=((float)q+off)/n*(dw-1);int i=(int)u;if(i>(int)dw-2)i=dw-2;float fu=u-i;
                    const int32_t *a0=s->accepted+(size_t)j*dw+i,*a1=a0+dw;const int16_t *h0=shape+(size_t)j*dw+i,*h1=h0+dw;
                    double dfm=(1-fu)*(1-fv)*a0[0]+fu*(1-fv)*a0[1]+(1-fu)*fv*a1[0]+fu*fv*a1[1];
                    double hb=(1-fu)*(1-fv)*h0[0]+fu*(1-fv)*h0[1]+(1-fu)*fv*h1[0]+fu*fv*h1[1];
                    b[(size_t)r*n+q]=(float)(hb*65536.*k-dfm*k);
                }
            }
            s->sim->base=b;
        }
    }
    return 1;
}
/* hardness_percent scales how hard a fully ridden-in line gets (log of the
   packed/loose stiffness ratio; 100 = ~1.6 cm sinkage on a ridden line);
   breakdown_percent scales how fast sliding tyres break packed crust loose. */
int mxdirt_session_set_dirt_response(MxdirtSession *s,float hardness_percent,float breakdown_percent){
    if(!fresh(s)||!s->sim||
       !isfinite(hardness_percent)||hardness_percent<25.f||hardness_percent>200.f||
       !isfinite(breakdown_percent)||breakdown_percent<0.f||breakdown_percent>300.f)return 0;
    s->sim->p.k_packed=s->sim->p.k_loose*powf(2.0e7f/2.3e4f,hardness_percent/100.f);
    s->sim->p.breakdown=breakdown_percent/100.f;return 1;
}
int mxdirt_session_set_sharp_ruts(MxdirtSession *s,int enabled){
    if(!fresh(s)||s->sim||(enabled!=0&&enabled!=1)||
       s->c.height.destination_width!=s->c.height.source_width+1||s->c.height.destination_height!=s->c.height.source_height+1)return 0;
    if(!mxdirt_height_field_set_vertex_exact(s->height,enabled))return 0;
    s->vertex_exact=enabled;return 1;
}
/* capture: tyres dropping into existing grooves; cohesion: damp walls holding
   steep; clods: roost landing as scattered clods. Each 0..300 %. */
int mxdirt_session_set_dirt_shape(MxdirtSession *s,float capture_percent,float cohesion_percent,float clods_percent){
    if(!fresh(s)||!s->sim||
       !isfinite(capture_percent)||capture_percent<0.f||capture_percent>300.f||
       !isfinite(cohesion_percent)||cohesion_percent<0.f||cohesion_percent>300.f||
       !isfinite(clods_percent)||clods_percent<0.f||clods_percent>300.f)return 0;
    s->sim->p.capture=capture_percent/100.f;s->sim->p.cohesion=cohesion_percent/100.f;s->sim->p.clods=clods_percent/100.f;return 1;
}
/* Soft top layer: every real tyre pass (live, extra riders, ghosts, pre-wear)
   presses it; the live gate lines (print_fade 1) are wear only. Pre-wear
   riders (print_fade .65) leave trenches without crisp knob pits. */
static void soft_contact(void *user,unsigned wheel,const MxbSoilSimWheel *w,float X,float Z,float v,float spin,float skid){
    MxdirtSession *s=user;if(!s||!s->soft||!s->sim||!w||!(w->print_fade<1.f))return;
    /* the dirt sim's ground under the tyre (before this pass cuts it): the layer
       lies deeper on loose soil and a softer track, thinner and ironed flat on
       packed lines, and holds crisp prints and tall lips when damp */
    const MxbSoilSim *m=s->sim;const float c0=m->p.compaction0;
    MxdSoftSoil soil={1.f+(.31f-c0)*1.3f,m->p.moisture,0.f}; /* firmness 55 % (c0 .31) = 1 */
    /* bilinear between sim texel centres: nearest-cell values stepped the trench depth every 27 cm */
    float col,row;mxb_ss_texel(m,X,Z,&col,&row);int q=(int)floorf(col),r=(int)floorf(row);
    if(q>=0&&r>=0&&q<(int)m->size-1&&r<(int)m->size-1){float fu=col-(float)q,fv=row-(float)r,c=0.f,mo=0.f,lo=0.f;
        for(int j=0;j<4;j++){size_t k=(size_t)(r+(j>>1))*m->size+(size_t)(q+(j&1));float wgt=((j&1)?fu:1.f-fu)*((j>>1)?fv:1.f-fv);
            c+=wgt*mxb_ss_c(m,k);mo+=wgt*mxb_ss_moist(m,k);lo+=wgt*fminf(m->loose[k],.06f);}
        soil.moisture=mo;soil.packed=fminf(1.f,fmaxf(0.f,(c-c0)/.35f));
        soil.thick=soil.thick*fminf(1.3f,fmaxf(.35f,1.f-1.1f*(c-c0)))+5.f*lo;}
    mxd_soft_stamp(s->soft,wheel==1,X,Z,w->fwd,w->lat,v,w->dt,w->load,spin,skid,w->slip_lat,w->material,w->print_fade<.3f,&soil);
}
int mxdirt_session_set_soft_layer(MxdirtSession *s,float thickness_m){
    if(!fresh(s)||!s->sim||s->soft||!isfinite(thickness_m))return 0;
    if(thickness_m<=0.f)return 1;
    s->soft=mxd_soft_create(s->sim->span_x,s->sim->span_z,thickness_m);if(!s->soft)return 0;
    s->sim->on_contact=soft_contact;s->sim->on_contact_user=s;return 1;
}
int mxdirt_session_soft_view(const MxdirtSession *s,MxdirtSoftView *v){
    if(!owned(s)||!v||!s->soft)return 0;const MxdSoft *m=s->soft;
    v->tile=(const uint8_t *const*)m->tile;v->serial=m->tile_serial;v->tiles_x=m->tiles_x;v->tiles_z=m->tiles_z;v->tile_side=MXD_SOFT_TILE;
    v->cell=m->cell;v->thickness=m->thickness;v->allocated=m->allocated;v->refused=m->refused;v->stamps=m->stamps;v->loaded=m->loaded;return 1;
}
/* Saved next to the track's laps; written to a temporary file then swapped in. */
int mxdirt_session_soft_save(const MxdirtSession *s,const char *path){
    if(!s||!s->soft||!path||(!s->soft->stamps&&!s->soft->loaded))return 0;
    char tmp[MAX_PATH];if(snprintf(tmp,sizeof(tmp),"%s.tmp",path)>=(int)sizeof(tmp))return 0;
    FILE *f=fopen(tmp,"wb");if(!f)return 0;int ok=mxd_soft_write(s->soft,f);ok=fclose(f)==0&&ok;
    return ok&&MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
}
int mxdirt_session_soft_load(MxdirtSession *s,const char *path){
    if(!owned(s)||!s->soft||!path||s->soft->stamps||s->soft->allocated)return 0;
    FILE *f=fopen(path,"rb");if(!f)return 0;int ok=mxd_soft_read(s->soft,f);fclose(f);return ok;
}
int mxdirt_session_sim_view(const MxdirtSession *s,MxdirtSimView *v){
    if(!owned(s)||!v||!s->sim||s->priming)return 0;
    const MxbSoilSim *m=s->sim;
    v->comp=m->comp;v->loose=m->loose;v->moist=m->moist_off;v->clods=m->clods;v->size=m->size;v->vertex_exact=(uint32_t)s->vertex_exact;
    v->moisture=m->p.moisture;v->compaction0=m->p.compaction0;v->steps=m->steps;
    v->print=m->print;v->print_dir=m->print_dir;v->print_off=m->print_off;return 1;
}
/* drying_percent: how fast the watered track dries over a ride (0 = never);
   sweep_percent: how strongly tyres push loose soil off a ridden line. */
int mxdirt_session_set_dirt_weather(MxdirtSession *s,float drying_percent,float sweep_percent){
    if(!fresh(s)||!s->sim||
       !isfinite(drying_percent)||drying_percent<0.f||drying_percent>400.f||
       !isfinite(sweep_percent)||sweep_percent<0.f||sweep_percent>300.f)return 0;
    s->sim->p.drying=drying_percent/100.f;s->sim->p.sweep=sweep_percent/100.f;return 1;
}
int mxdirt_session_set_ghosts(MxdirtSession *s,const MxbDirtGhostLap *laps,unsigned count){
    if(!owned(s)||!s->sim||count>MXDIRT_SESSION_MAX_GHOSTS||(count&&!laps))return 0;
    for(unsigned i=0;i<count;i++)if(!laps[i].s||!laps[i].h.samples)return 0;
    memset(s->ghost,0,sizeof(s->ghost));s->ghost_laps=count?laps:NULL;s->ghost_count=count;
    /* Stagger: ghost i starts i/count of the way through its own lap, so the
       gate is spread around the track instead of riding in one bunch. */
    for(unsigned i=0;i<count;i++){
        const MxbDirtGhostLap *g=laps+i;float len=g->s[g->h.samples-1].t,start=len*(float)i/(float)count;
        uint32_t lo=0,hi=g->h.samples;while(lo<hi){uint32_t mid=(lo+hi)/2;if(g->s[mid].t<=start)lo=mid+1;else hi=mid;}
        s->ghost[i].cursor=lo<g->h.samples?lo:0;s->ghost[i].clock=lo<g->h.samples?start:0.f;
    }
    return 1;
}
int mxdirt_session_live_contacts(const MxdirtSession *s,MxbDirtGhostSample out[2],int ok[2]){
    if(!s||!out||!ok)return 0;for(int w=0;w<2;w++){out[w]=s->live[w];ok[w]=s->live_ok[w];}return 1;
}
uint64_t mxdirt_session_ghost_passes(const MxdirtSession *s){
    uint64_t n=0;if(s)for(unsigned i=0;i<s->ghost_count;i++)n+=s->ghost[i].passes;return n;
}
/* Advance every ghost by one outer frame: replay its recorded contacts whose
   time falls in this frame, wrapping at the end of its lap. Each ghost wheel
   keeps its own floor memory so it cuts like a separate bike. */
static void ghosts_step(MxdirtSession *s,float dt){
    if(!s->sim||!s->ghost_count||!(dt>0.f)||dt>.25f)return;
    MxbSoilSim *m=s->sim;
    for(unsigned i=0;i<s->ghost_count;i++){
        const MxbDirtGhostLap *g=s->ghost_laps+i;float end=g->s[g->h.samples-1].t,target=s->ghost[i].clock+dt;
        for(unsigned guard=0;guard<4096;guard++){
            if(s->ghost[i].cursor>=g->h.samples){ /* lap done: start it again */
                s->ghost[i].cursor=0;target-=end;s->ghost[i].has_last[0]=s->ghost[i].has_last[1]=0;
                if(target<0.f)target=0.f;
            }
            const MxbDirtGhostSample *p=g->s+s->ghost[i].cursor;if(p->t>target)break;
            MxbSoilSimWheel w;memset(&w,0,sizeof(w));
            w.x=p->x;w.z=p->z;w.fwd[0]=p->fwd[0];w.fwd[1]=p->fwd[1];w.lat[0]=p->lat[0];w.lat[1]=p->lat[1];
            w.speed=p->speed;w.slip_long=p->slip_long;w.slip_lat=p->slip_lat;w.load=p->load;w.width=p->width;w.radius=p->radius;
            w.gain=p->gain;w.dt=p->dt;w.valid=1;w.material=p->material;
            unsigned k=p->wheel;
            float lx=m->last_x[k],lz=m->last_z[k],lf=m->last_floor[k];int lh=m->has_last[k];
            m->last_x[k]=s->ghost[i].last_x[k];m->last_z[k]=s->ghost[i].last_z[k];m->last_floor[k]=s->ghost[i].last_floor[k];m->has_last[k]=s->ghost[i].has_last[k];
            mxb_soil_sim_wheel(m,k,&w);
            s->ghost[i].last_x[k]=m->last_x[k];s->ghost[i].last_z[k]=m->last_z[k];s->ghost[i].last_floor[k]=m->last_floor[k];s->ghost[i].has_last[k]=m->has_last[k];
            m->last_x[k]=lx;m->last_z[k]=lz;m->last_floor[k]=lf;m->has_last[k]=lh;
            s->ghost[i].cursor++;s->ghost[i].passes++;
        }
        s->ghost[i].clock=target;
    }
}
/* The work-history grid's packed/loose follow the dirt sim's own state where it
   changed: compacted lines read packed, berms/roost/churned soil read loose.
   Soil cell rows run with +z; simulation rows run with -z. */
static void sim_to_soil_rect(MxdirtSession *s,int x0,int x1,int z0,int z1){
    MxbSoilSim *m=s->sim;MxbSoilEvolution *e=s->soil;
    float sz=(float)m->size;
    int i0=(int)floorf((float)x0/sz*(e->width-1)),i1=(int)ceilf((float)(x1+1)/sz*(e->width-1));
    int j0=(int)floorf((1.f-(float)(z1+1)/sz)*(e->height-1)),j1=(int)ceilf((1.f-(float)z0/sz)*(e->height-1));
    if(i0<0)i0=0;if(j0<0)j0=0;if(i1>(int)e->width-1)i1=e->width-1;if(j1>(int)e->height-1)j1=e->height-1;
    for(int j=j0;j<=j1;j++)for(int i=i0;i<=i1;i++){
        float u=(float)i/(e->width-1),v=(float)j/(e->height-1);
        int col=(int)lrintf(u*sz-m->texel_off),row=(int)lrintf((1.f-v)*sz-m->texel_off);
        float c=0,L=0;int n=0,touched=0;
        for(int r=row;r<=row;r++)for(int q=col;q<=col;q++){ /* nearest cell: a 3x3 mean mixes rut with its berms */
            if(q<0||r<0||q>=(int)m->size||r>=(int)m->size)continue;size_t k=(size_t)r*m->size+q;
            c+=mxb_ss_c(m,k);L+=m->loose[k];n++;touched|=m->comp[k]!=0||m->loose[k]>0.f;
        }
        if(!n||!touched)continue;c/=n;L/=n;
        MxbSoilCell *cell=e->cell+(size_t)j*e->width+i;
        float loose=fminf(1.f,L/.04f);
        cell->loose=loose;cell->packed=fminf(1.f,fmaxf(0.f,(c-.30f)/.50f))*(1.f-loose);
    }
}
/* Only the patches the wheels (and ghosts) changed this step, not their union box. */
static void sim_to_soil(MxdirtSession *s){
    MxbSoilSim *m=s->sim;MxbSoilEvolution *e=s->soil;if(!m||!e||!m->rect_n||e->width<2||e->height<2)return;
    const int pad=6;for(int i=0;i<m->rect_n;i++){const int *q=m->rect[i];sim_to_soil_rect(s,q[0]-pad,q[2]+pad,q[1]-pad,q[3]+pad);}
}
/* Pre-wear (mxb_soil_prewear.h): a gate of virtual riders on the recorded laps
   before the first step; the worn tiles are published with the next step. */
int mxdirt_session_prewear(MxdirtSession *s,const MxbDirtGhostLap *laps,unsigned count,unsigned riders,float spread,float clip,uint32_t seed,uint64_t *passes){
    if(passes)*passes=0;
    if(!owned(s)||!s->sim||!laps||!count||!riders||riders>400||s->generated_sequence||s->physics_seen||
       !(spread>=0.f)||spread>5.f||!(clip>=0.f)||clip>6.f)return 0;
    for(unsigned i=0;i<count;i++)if(!laps[i].s||!laps[i].h.samples)return 0;
    unsigned char *uni=calloc((size_t)s->sim->tiles_x*s->sim->tiles_y,1);if(!uni)return 0;
    if(s->soft){s->soft->defer_relax=1;s->soft->has_last[0]=s->soft->has_last[1]=0;} /* soft layer settles once at the end */
    int box[4];uint64_t n=mxb_soil_prewear(s->sim,laps,count,riders,spread,clip,.13f,seed,uni,box); /* .13 of a live pass */
    if(s->soft){s->soft->defer_relax=0;s->soft->has_last[0]=s->soft->has_last[1]=0;mxd_soft_relax_all(s->soft,s->sim->p.moisture,2);}
    free(s->prewear_tiles);s->prewear_tiles=uni;
    if(s->soil&&box[2]>=0)sim_to_soil_rect(s,box[0]-6,box[2]+6,box[1]-6,box[3]+6); /* work history follows the worn area */
    if(passes)*passes=n;return 1;
}
/* Vertices of the loaded (saved) deformation deeper/higher than min_m. A track the
   game saved after a ride already carries the pre-wear of that ride, so pre-wear
   can run only on a fresh track instead of stacking on every load. */
uint64_t mxdirt_session_saved_wear(const MxdirtSession *s,double min_m){
    if(!s||!s->accepted||!(s->c.height.mxb_vertical_scale>0.))return 0;
    double k=s->c.height.mxb_vertical_scale/(65535.*65536.);size_t n=(size_t)s->c.height.destination_width*s->c.height.destination_height;uint64_t c=0;
    for(size_t i=0;i<n;i++)if(fabs((double)s->accepted[i]*k)>min_m)c++;
    return c;
}
int mxdirt_session_set_live_gate(MxdirtSession *s,unsigned lines){
    if(!owned(s)||!s->sim||lines<1||lines>MXB_LIVE_GATE_MAX)return 0;
    mxb_live_gate_init(&s->gate,lines,0x51ed270bu);return 1;
}
int mxdirt_session_set_extra_riders(MxdirtSession *s,unsigned riders){
    if(!fresh(s)||!s->sim||riders>15)return 0;
    s->extra_riders=riders;return 1;
}
/* Current track-wide moisture 0..1 (watered value minus drying so far). */
int mxdirt_session_dirt_moisture_now(const MxdirtSession *s,float *out){
    if(!s||!out||!s->sim)return 0;*out=s->sim->p.moisture;return 1;
}
int mxdirt_session_set_dirt_moisture(MxdirtSession *s,float percent){
    if(!fresh(s)||!s->sim||!isfinite(percent)||percent<0.f||percent>100.f)return 0;
    s->sim->p.moisture=percent/100.f;return 1;
}
int mxdirt_session_dirt_sim_stats(const MxdirtSession *s,double out[6]){
    if(!s||!out||!s->sim)return 0;
    out[0]=s->sim->volume_m3;out[1]=s->sim->cut_m3;out[2]=s->sim->compacted_m3;out[3]=s->sim->roost_m3;out[4]=s->sim->dust_m3;out[5]=(double)s->sim->relax_moves;return 1;
}
/* Contact -> physical wheel input for the soil model (metres, newtons). The
   effective tyre widths include knobs + sidewall bulge in soft soil. */
static void sim_wheel(const MxdirtSession *s,const MxdirtContact *i,int w,float gain,MxbSoilSimWheel *o){
    memset(o,0,sizeof(*o));float m=(float)s->c.height.native_metres;
    float fl=hypotf(i->forward[0],i->forward[2]),ll=hypotf(i->lateral[0],i->lateral[2]);
    if(!i->contact||!i->deformation||!(fl>1e-4f)||!(ll>1e-4f))return;
    o->x=i->point[0];o->z=i->point[2];
    o->fwd[0]=i->forward[0]/fl;o->fwd[1]=i->forward[2]/fl;o->lat[0]=i->lateral[0]/ll;o->lat[1]=i->lateral[2]/ll;
    o->speed=fmaxf(0.f,(i->center_velocity[0]*o->fwd[0]+i->center_velocity[2]*o->fwd[1])*m);
    o->slip_long=i->slip[2]*(i->direction<0.f?-1.f:1.f)*m;o->slip_lat=i->slip[0]*m;
    o->load=i->load_n;
    o->width=w?.15f:.11f;o->radius=w?.33f:.35f;o->gain=gain;o->dt=i->dt;
    o->valid=isfinite(o->x)&&isfinite(o->z)&&isfinite(o->speed)&&isfinite(o->slip_long)&&isfinite(o->slip_lat)&&isfinite(o->load)&&o->load>0.f;
}
int mxdirt_session_set_bumps(MxdirtSession *s,float percent){
    if(!fresh(s)||!isfinite(percent)||percent<0.f||percent>200.f)return 0;
    s->bump_gain=.30f*percent/100.f;memset(s->bump,0,sizeof(s->bump));return 1;
}
int mxdirt_session_set_rolling_bumps(MxdirtSession *s,float percent){
    if(!fresh(s)||!isfinite(percent)||percent<0.f||percent>100.f)return 0;
    s->rolling_bumps=fmaxf(.1f,percent/100.f);return 1;
}
int mxdirt_session_physics(MxdirtSession *s,int wheel,const MxdirtPhysicsSample *v) {
    if(!owned(s)||wheel<0||wheel>=2||!mxdirt_contact_physics(&s->wheel[wheel],v))return 0;
    s->physics_seen=1;return 1;
}
int mxdirt_session_step(MxdirtSession *s,const MxdirtControllerFrame frames[2],
    const int material[2],MxdirtHeightStats *stats) {
    if(!owned(s)||!s->sim||!frames||!material||frames[0].sequence!=frames[1].sequence||
       frames[0].sequence<=s->outer_sequence||frames[0].dt!=frames[1].dt||
       frames[0].terrain_span_native!=s->c.height.native_span_x||
       frames[1].terrain_span_native!=s->c.height.native_span_x||s->generated_sequence==UINT64_MAX)return 0;
    uint64_t t0=qpc_now();
    MxdirtContactBridge staged[2]={s->wheel[0],s->wheel[1]};MxdirtContact input[2];
    for(int w=0;w<2;w++)if(!mxdirt_contact_frame(&staged[w],&frames[w],&input[w]))return 0;
    MxbSoilStep soil_step;
    if(s->soil&&!mxb_soil_prepare(s->soil,input,material,s->priming,&soil_step))goto fault;
    MxbBumpState bumps[2]={s->bump[0],s->bump[1]};float gains[2]={1.f,1.f},biases[2]={0.f,0.f},floors[2]={0.f,0.f};
    for(int w=1;w>=0;w--){
        float cut=s->soil?soil_step.feedback[w].cut:1.f;
        if(s->bump_gain>0.f){
            if(s->priming)bumps[w].valid=0;
            else cut=fmaxf(.75f,fminf(1.3f,cut*bump_factor(s,&input[w],&bumps[w],&biases[w],&floors[w])));
        }
        gains[w]=cut;
    }
    uint64_t t1=qpc_now();
    if(!s->priming)mxb_soil_sim_advance(s->sim,frames[0].dt);
    mxb_soil_sim_begin(s->sim);
    for(int w=0;w<2;w++){
        s->live_ok[w]=0;
        if(s->priming){s->sim->has_last[w]=0;continue;}
        MxbSoilSimWheel sw;sim_wheel(s,&input[w],w,gains[w],&sw);sw.material=material[w];sw.floor_bias=biases[w];sw.bias_floor=floors[w]>0.f?floors[w]:s->bias_floor_m;sw.turn=bumps[w].valid?mxb_bump_turn(&bumps[w].sit):0.f;sw.brake=bumps[w].valid?fmaxf(bumps[w].gb,fminf(1.f,fmaxf(0.f,(-sw.slip_long/fmaxf(sw.speed,1.f)-.10f)/.65f))):0.f; /* braking or hard on the gas */sw.impact=bumps[w].valid&&bumps[w].sit.since<.25f?1.f-bumps[w].sit.since/.25f:0.f;
        {const float d0=s->sim->p.depth_scale;s->sim->p.depth_scale=d0*(s->bump_gain>0.f?mxb_live_gate_share(&s->gate,1):1.f); /* this pass's wear shared over the gate lines */
         mxb_soil_sim_wheel(s->sim,(unsigned)w,&sw);
         if(s->bump_gain>0.f)mxb_live_gate_wheel(s->sim,&s->gate,(unsigned)w,&sw,bumps[w].gs,bumps[w].gb,bumps[w].ge,bumps[w].landed);
         s->sim->p.depth_scale=d0;}
        if(sw.valid){MxbDirtGhostSample *l=s->live+w;memset(l,0,sizeof(*l));
            l->x=sw.x;l->z=sw.z;l->fwd[0]=sw.fwd[0];l->fwd[1]=sw.fwd[1];l->lat[0]=sw.lat[0];l->lat[1]=sw.lat[1];
            l->speed=sw.speed;l->slip_long=sw.slip_long;l->slip_lat=sw.slip_lat;l->load=sw.load;l->width=sw.width;l->radius=sw.radius;
            l->gain=sw.gain;l->dt=sw.dt;l->wheel=(uint8_t)w;l->material=(uint8_t)(sw.material>=0&&sw.material<256?sw.material:7);s->live_ok[w]=1;}
        /* Extra riders: the same contact replayed by riders spread across the
           line (+-~0.4 m), each cutting from the current surface like a separate
           bike, so one real lap wears the track like a full gate. The real
           wheel's floor memory is saved and restored around them. */
        if(s->extra_riders&&sw.valid){
            float lx=s->sim->last_x[w],lz=s->sim->last_z[w],lf=s->sim->last_floor[w];int lh=s->sim->has_last[w];
            float nm=s->sim->native_m;
            for(unsigned g=1;g<=s->extra_riders;g++){
                float off=((float)(g&1u?1:-1))*(.09f*(float)((g+1)/2))*(.85f+.3f*(float)((g*2654435761u>>28)&15u)/15.f);
                if(off>.45f)off=.45f;if(off<-.45f)off=-.45f;
                MxbSoilSimWheel gw=sw;gw.x+=sw.lat[0]*off/nm;gw.z+=sw.lat[1]*off/nm;
                s->sim->has_last[w]=0;mxb_soil_sim_wheel(s->sim,(unsigned)w,&gw);
            }
            s->sim->last_x[w]=lx;s->sim->last_z[w]=lz;s->sim->last_floor[w]=lf;s->sim->has_last[w]=lh;
        }
    }
    if(!s->priming){ghosts_step(s,frames[0].dt);mxb_live_gate_advance(&s->gate,frames[0].dt);}
    mxb_soil_sim_end(s->sim);
    if(s->prewear_tiles){size_t tn=(size_t)s->sim->tiles_x*s->sim->tiles_y;for(size_t t=0;t<tn;t++)s->sim->tiles[t]|=s->prewear_tiles[t];free(s->prewear_tiles);s->prewear_tiles=NULL;}
    uint64_t t2=qpc_now();
    if(s->sim->size!=s->c.height.source_width||s->sim->tiles_x!=s->sim->size/128||
       s->sim->tiles_y!=s->sim->size/32||(size_t)s->sim->tiles_x*s->sim->tiles_y!=s->tile_count||
       !s->sim->h||!s->sim->tiles)goto fault;
    s->encoded=s->sim->h;free(s->seed);s->seed=NULL;
    MxdirtHeightStats hs;
    if(!mxdirt_height_field_convert_changed_tiles(s->height,s->c.height.generation,s->encoded,
       s->source_cells,s->sim->tiles,s->tile_count,s->target,s->cells,&hs))goto fault;
    uint64_t t3=qpc_now();timing_add(s,0,t1-t0);timing_add(s,1,t2-t1);timing_add(s,2,t3-t2);s->timing_frames++;
    const uint32_t *begin,*end;size_t rows;
    if(!mxdirt_height_field_changed_rows(s->height,s->c.height.generation,&begin,&end,&rows))goto fault;
    for(size_t z=0;z<rows;z++){
        if(begin[z]<s->pending_begin[z])s->pending_begin[z]=begin[z];
        if(end[z]>s->pending_end[z])s->pending_end[z]=end[z];
    }
    s->wheel[0]=staged[0];s->wheel[1]=staged[1];s->outer_sequence=frames[0].sequence;
    if(s->soil){mxb_soil_commit(s->soil,&soil_step);sim_to_soil(s);}
    s->bump[0]=bumps[0];s->bump[1]=bumps[1];
    ++s->generated_sequence;s->priming=0;
    if(stats)*stats=hs;return 1;
fault:s->faulted=1;return 0;
}
int mxdirt_session_publish(MxdirtSession *s,uint64_t next_outer,MxdirtAcceptTarget accept,void *context) {
    if(!owned(s)||!accept)return -1;
    if(!s->generated_sequence||(s->accepted_source_sequence==s->generated_sequence&&!s->force_publish))return 1;
    if(next_outer<=s->outer_sequence)return 0;
    size_t n=0;int partial=0;
    /* Union every converted row interval since the last accepted target.
       Backpressure retains the entire candidate union. A union larger than the
       sparse capacity (the pre-wear gate changes ~140k cells at once) is sent in
       capacity-sized pieces over the next frames - absolute cells, so a piece is
       always valid. Rejecting it instead would block publication for good. */
    for(uint32_t z=0;z<s->c.height.destination_height&&!partial;z++){
        size_t row=(size_t)z*s->c.height.destination_width;
        for(uint32_t x=s->pending_begin[z];x<s->pending_end[z];x++){
            size_t j=row+x;if(s->accepted[j]==s->target[j])continue;
            if(n==s->c.sparse_capacity){partial=1;break;}
            s->changes[n++]=(MxbPubAbsoluteCell){(uint32_t)j,s->target[j]};
        }
    }
    if(!n){s->accepted_source_sequence=s->generated_sequence;s->force_publish=0;clear_candidates(s);return 1;}
    if(s->accepted_sequence==UINT64_MAX){s->faulted=1;return -1;}
    const uint64_t next_sequence=s->accepted_sequence+1;
    s->in_callback=1;int result=accept(context,s->c.contact.generation,s->accepted_sequence,
        next_sequence,s->changes,n);s->in_callback=0;
    if(result==0)return 0;
    if(result!=1){s->faulted=1;return -1;}
    for(size_t j=0;j<n;j++)s->accepted[s->changes[j].index]=s->changes[j].absolute;
    s->accepted_sequence=next_sequence;
    if(partial)return 1; /* the rest stays pending; the next frame sends the next piece */
    s->accepted_source_sequence=s->generated_sequence;s->force_publish=0;
    clear_candidates(s);return 1;
}
/* The transport adopted these cells from another native writer and dropped our
   in-flight updates for them - the game no longer holds our heights there. Mark
   them unsent so the next publish resends the sim's target (a pre-wear tile
   adopted mid-upload stayed flat under painted ruts for the whole ride). */
int mxdirt_session_resend_rect(MxdirtSession *s,uint32_t x,uint32_t z,uint32_t w,uint32_t h){
    if(!owned(s))return 0;const uint32_t W=s->c.height.destination_width,H=s->c.height.destination_height;
    if(x>=W||z>=H)return 0;if(w>W-x)w=W-x;if(h>H-z)h=H-z;
    for(uint32_t r=z;r<z+h;r++){size_t row=(size_t)r*W;
        for(uint32_t c=x;c<x+w;c++)s->accepted[row+c]=s->target[row+c]^1;
        if(x<s->pending_begin[r])s->pending_begin[r]=x;if(x+w>s->pending_end[r])s->pending_end[r]=x+w;}
    s->force_publish=1;return 1;
}
int mxdirt_session_reprime(MxdirtSession *s) {
    if(!owned(s))return 0;
    for(int w=0;w<2;w++)if(!mxdirt_contact_init(&s->wheel[w],&s->c.contact,w))return 0;
    s->priming=1;return 1;
}
int mxdirt_session_state(const MxdirtSession *s,MxdirtSessionState *out) {
    if(!s||!out||s->owner!=GetCurrentThreadId())return 0;
    *out=(MxdirtSessionState){s->generated_sequence,s->accepted_sequence,s->accepted_source_sequence,s->outer_sequence,
        s->cells,s->c.sparse_capacity,s->priming,s->faulted};return 1;
}
const int32_t *mxdirt_session_target(const MxdirtSession *s){return owned(s)?s->target:NULL;}
