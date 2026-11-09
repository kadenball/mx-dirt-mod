#ifndef MXDIRT_SOIL_EVOLUTION_H
#define MXDIRT_SOIL_EVOLUTION_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mxdirt_contact.h"

/* Bounded terrain-work history (authored model, "work_history_v1"). A coarse
   0.5 m grid over the track remembers how packed / loose each spot is and the
   signed braking / drive work of past passes (measured load variation). Its
   feedback.cut scales the dirt sim's cut gain of the next contacts (packed
   lines cut less, churned loose soil more). No procedural wave, no tyre-force
   change. State lasts for this terrain session, not saved in the DFM. Updates
   are staged until the corresponding dirt step succeeds. Owner-only; at most
   16 MiB per field, no allocation or whole-grid scan per tick. */
/* The model's load thresholds (10, 100, 350) are in pound-force. */
#define MXDIRT_SOIL_LOAD_PER_NEWTON ((float)(1.0/4.4482216152605))
typedef struct {float packed,loose,brake,drive;} MxbSoilCell;
typedef struct {float x,z,load;int valid;} MxbSoilWheel;
typedef struct {float cut,deposit,particles,packed,loose,brake,drive;} MxbSoilFeedback;
typedef struct {uint32_t index;MxbSoilCell cell;} MxbSoilEdit;
typedef struct {
    MxbSoilEdit edit[264];unsigned count;
    MxbSoilWheel wheel[2];MxbSoilFeedback feedback[2];
} MxbSoilStep;
typedef struct {
    MxbSoilCell *cell;uint32_t width,height;
    float span_x,span_z,metres;MxbSoilWheel wheel[2];
    MxbSoilFeedback feedback[2];uint64_t committed;
} MxbSoilEvolution;
static inline float mxb_soil_clamp(float v,float lo,float hi){return fminf(hi,fmaxf(lo,v));}
/* MX Bikes surfaces: 1 soil (4,7), 2 soft/sand (5,10), 3 hard-pack/gravel (11,12). */
static inline int mxb_soil_class(int material){
    if(material==4||material==7)return 1;
    if(material==5||material==10)return 2;
    if(material==11||material==12)return 3;
    return 0;
}
static inline MxbSoilFeedback mxb_soil_neutral(void){
    return (MxbSoilFeedback){1,1,1,0,0,0,0};
}
/* sx/sz: terrain span in native units, metres: metres per native unit. */
static inline int mxb_soil_create(MxbSoilEvolution **out,float sx,float sz,float metres){
    if(!out||!isfinite(sx)||!isfinite(sz)||!isfinite(metres)||sx<=0||sz<=0||metres<=0||
       !isfinite(sx*metres)||!isfinite(sz*metres))return 0;
    MxbSoilEvolution *s=calloc(1,sizeof(*s));if(!s)return 0;
    s->span_x=sx;s->span_z=sz;s->metres=metres;
    s->width=(uint32_t)mxb_soil_clamp(ceilf(sx*metres/.5f)+1,2,1024);
    s->height=(uint32_t)mxb_soil_clamp(ceilf(sz*metres/.5f)+1,2,1024);
    s->cell=calloc((size_t)s->width*s->height,sizeof(*s->cell));
    if(!s->cell){free(s);return 0;}
    s->feedback[0]=s->feedback[1]=mxb_soil_neutral();*out=s;return 1;
}
static inline void mxb_soil_destroy(MxbSoilEvolution *s){if(s){free(s->cell);free(s);}}
static inline MxbSoilCell mxb_soil_staged(const MxbSoilEvolution *s,const MxbSoilStep *t,uint32_t k){
    for(unsigned j=0;j<t->count;j++)if(t->edit[j].index==k)return t->edit[j].cell;
    return s->cell[k];
}
static inline int mxb_soil_store(MxbSoilStep *t,uint32_t k,MxbSoilCell c){
    for(unsigned j=0;j<t->count;j++)if(t->edit[j].index==k){t->edit[j].cell=c;return 1;}
    if(t->count>=264)return 0;t->edit[t->count++]=(MxbSoilEdit){k,c};return 1;
}
static inline int mxb_soil_weights(const MxbSoilEvolution *s,float x,float z,uint32_t k[4],float a[4]){
    float u=x/s->span_x+.5f,v=z/s->span_z+.5f;
    if(!isfinite(u)||!isfinite(v)||u<0||v<0||u>1||v>1)return 0;
    u*=s->width-1;v*=s->height-1;
    uint32_t ix=(uint32_t)u,iz=(uint32_t)v;
    if(ix==s->width-1)ix--;if(iz==s->height-1)iz--;
    float fx=u-ix,fz=v-iz;k[0]=iz*s->width+ix;k[1]=k[0]+1;k[2]=k[0]+s->width;k[3]=k[2]+1;
    a[0]=(1-fx)*(1-fz);a[1]=fx*(1-fz);a[2]=(1-fx)*fz;a[3]=fx*fz;return 1;
}
/* Stage this frame's work for both wheels and compute their feedback. prime:
   first frame after (re)priming, no path continuity with the last contacts. */
static inline int mxb_soil_prepare(const MxbSoilEvolution *s,const MxdirtContact in[2],
        const int material[2],int prime,MxbSoilStep *out){
    if(!s||!in||!material||!out||s->committed==UINT64_MAX)return 0;
    MxbSoilStep t={0};memcpy(t.wheel,s->wheel,sizeof(t.wheel));
    for(unsigned w=0;w<2;w++){
        const MxdirtContact *i=in+w;MxbSoilWheel old=t.wheel[w];t.feedback[w]=mxb_soil_neutral();
        float x=i->point[0],z=i->point[2],dt=i->dt,load=i->load_n*MXDIRT_SOIL_LOAD_PER_NEWTON;
        if(!isfinite(x)||!isfinite(z)||!isfinite(dt)||dt<0||dt>.1f||!isfinite(load)||load<0)return 0;
        float speed=hypotf(i->center_velocity[0],i->center_velocity[2])*s->metres;
        float slip_speed=(i->slip[0]*i->forward[0]+i->slip[2]*i->forward[2])*s->metres;
        if(!isfinite(speed)||!isfinite(slip_speed))return 0;
        int cls=mxb_soil_class(material[w]);uint32_t ids[4];float weights[4];
        if(!cls||!i->contact||!i->deformation||!mxb_soil_weights(s,x,z,ids,weights)){
            t.wheel[w].valid=0;continue;
        }
        float distance=old.valid&&!prime?hypotf(x-old.x,z-old.z)*s->metres:0;
        float max_distance=fminf(8.f,speed*dt*2.f+.25f);
        int contiguous=old.valid&&!prime&&distance<=max_distance;
        float mean=contiguous?old.load:load;
        float variation=mean>10?mxb_soil_clamp((load-mean)/fmaxf(mean,100),-.8f,.8f):0;
        t.wheel[w]=(MxbSoilWheel){x,z,mean+(load-mean)*(dt/(.18f+dt)),1};
        float slip=mxb_soil_clamp(slip_speed/fmaxf(speed,1.f),-1.f,1.f);
        float brake=mxb_soil_clamp((slip-.08f)/.45f,0,1);
        float drive=w?mxb_soil_clamp((-slip-.10f)/.65f,0,1):0;
        float churn=fmaxf(brake*.55f,drive);
        if(contiguous&&dt>0&&speed>.5f&&distance>0){
            float spacing=fminf(s->span_x/(s->width-1),s->span_z/(s->height-1))*s->metres;
            unsigned samples=(unsigned)ceilf(distance/fmaxf(.125f,spacing*.5f));
            if(samples>32)samples=32;if(!samples)samples=1;
            float work=distance/samples*mxb_soil_clamp(load/350.f,0,2);
            float pack_rate=cls==2?.13f:cls==3?.30f:.22f;
            float loosen_rate=cls==2?.50f:cls==3?.12f:.32f;
            for(unsigned sample=0;sample<samples;sample++){
                float fraction=((float)sample+.5f)/samples;
                if(!mxb_soil_weights(s,old.x+(x-old.x)*fraction,old.z+(z-old.z)*fraction,ids,weights))continue;
                for(unsigned j=0;j<4;j++){
                    if(weights[j]==0)continue;
                    MxbSoilCell c=mxb_soil_staged(s,&t,ids[j]);float amount=fminf(.5f,work*weights[j]);
                    c.packed=mxb_soil_clamp(c.packed+amount*(pack_rate*(1-c.packed)*(1-.75f*churn)-loosen_rate*churn*c.packed),0,1);
                    c.loose=mxb_soil_clamp(c.loose+amount*(loosen_rate*churn*(1-c.loose)-pack_rate*(1-churn)*c.loose),0,1);
                    /* High-load contacts excavate more; unloaded contacts leave
                       crests. Repeated stressed passes reinforce that pattern.
                       Smooth rolling slowly consolidates loose irregularities. */
                    float settle=1-amount*.05f*(1-fmaxf(brake,drive));
                    c.brake=mxb_soil_clamp(c.brake*settle+amount*brake*variation*.50f,-1,1);
                    c.drive=mxb_soil_clamp(c.drive*settle+amount*drive*variation*.50f,-1,1);
                    if(!mxb_soil_store(&t,ids[j],c))return 0;
                }
            }
        }
        if(!mxb_soil_weights(s,x,z,ids,weights))return 0;
        MxbSoilCell c={0};for(unsigned j=0;j<4;j++){
            MxbSoilCell v=mxb_soil_staged(s,&t,ids[j]);
            c.packed+=v.packed*weights[j];c.loose+=v.loose*weights[j];
            c.brake+=v.brake*weights[j];c.drive+=v.drive*weights[j];
        }
        MxbSoilFeedback *f=t.feedback+w;f->packed=c.packed;f->loose=c.loose;f->brake=c.brake;f->drive=c.drive;
        float resistance=1-.26f*c.packed+.07f*c.loose;
        f->cut=mxb_soil_clamp(resistance*(1+.28f*(c.brake+c.drive)),.65f,1.25f);
        f->deposit=mxb_soil_clamp(1-.30f*c.packed+.12f*c.loose,.65f,1.12f);
        f->particles=mxb_soil_clamp(1-.45f*c.packed+.20f*c.loose,.5f,1);
    }
    *out=t;return 1;
}
static inline void mxb_soil_commit(MxbSoilEvolution *s,const MxbSoilStep *t){
    for(unsigned j=0;j<t->count;j++)s->cell[t->edit[j].index]=t->edit[j].cell;
    memcpy(s->wheel,t->wheel,sizeof(s->wheel));memcpy(s->feedback,t->feedback,sizeof(s->feedback));++s->committed;
}
#endif
