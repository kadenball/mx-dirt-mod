#ifndef MXB_SOIL_LIVE_GATE_H
#define MXB_SOIL_LIVE_GATE_H
/* v7.9 live gate share (pre-wear off). One bike on one line only digs a groove: in
   offline tests 96 of the user's real laps gave 84-95 % rut-wall ridges and no rows,
   while Pro (a whole gate, many lines) is ~26 % rows across the track, ~25 % moguls.
   A live pass already cuts as deep as ~7 gate bikes, so instead of one full-depth
   groove its wear is shared out over n lines riding with the player: line 0 is the
   player's own tyre, the others sit at their own offsets around it (sd 1.2 m, a new
   offset every ~40 s as traffic changes) and bounce on the ground under them with
   their own suspension rate - the same body-mode spring and situational stress as the
   pre-wear gate. Total wear per lap is unchanged; nothing ever appears ahead of the
   player. n = 1 restores the single groove. */
#include "mxb_soil_bumps.h"
#include "mxb_soil_prewear.h"
#define MXB_LIVE_GATE_MAX 8
typedef struct {float off,fk,y[2],zb[2],zbd[2],lx[2],lz[2],lfl[2];int valid[2],lh[2];} MxbGateLine; /* l*: the line's own tyre floor memory */
typedef struct {unsigned n;uint32_t st;float clock;unsigned next;MxbGateLine l[MXB_LIVE_GATE_MAX];} MxbLiveGate;
static inline void mxb_live_gate_line(MxbLiveGate *g,unsigned k){
    MxbGateLine *L=g->l+k;memset(L,0,sizeof(*L));
    float o=1.2f*mxb_pw_normal(&g->st);L->off=o>3.f?3.f:o<-3.f?-3.f:o;
    float fk=1.f+.05f*mxb_pw_normal(&g->st);L->fk=fk<.75f?.75f:fk>1.3f?1.3f:fk;
}
static inline void mxb_live_gate_init(MxbLiveGate *g,unsigned n,uint32_t seed){
    memset(g,0,sizeof(*g));g->n=n<1?1:n>MXB_LIVE_GATE_MAX?MXB_LIVE_GATE_MAX:n;g->st=seed?seed:0x51ed270bu;g->next=1;
    for(unsigned k=1;k<g->n;k++)mxb_live_gate_line(g,k);
}
/* call once per step with the step dt: traffic changes, one line at a time */
static inline void mxb_live_gate_advance(MxbLiveGate *g,float dt){
    if(g->n<2)return;g->clock+=dt;
    if(g->clock>40.f/(float)(g->n-1)){g->clock=0.f;mxb_live_gate_line(g,g->next);g->next=g->next+1<g->n?g->next+1:1;}
}
/* the other lines of this wheel's pass. real = the player's wheel as just submitted (its
   depth already shared), stress/brk/ef from mxb_bump_stress for this wheel, landed: the
   player's wheel just touched down. Caller sets the sim's depth_scale to the player's share (mxb_live_gate_share(g,1)). */
/* v8.2: the player's own line keeps 30 % of the pass (the rest shared by the others), so
   the line just ridden reads clearly; returns the depth multiplier for each share */
static inline float mxb_live_gate_share(const MxbLiveGate *g,int own){return g->n<2?.42f:own?.3f:.7f/(float)(g->n-1);}
static inline void mxb_live_gate_wheel(MxbSoilSim *s,MxbLiveGate *g,unsigned w,const MxbSoilSimWheel *real,float stress,float brk,float ef,int landed){
    if(g->n<2||!real->valid)return;const float d_own=s->p.depth_scale,d_line=d_own/mxb_live_gate_share(g,1)*mxb_live_gate_share(g,0);s->p.depth_scale=d_line;
    const float nm=s->native_m,dnk=fmaxf(.3f,fminf(2.5f,d_own/mxb_live_gate_share(g,1)/.5f)),kb=.017f*dnk;
    float ll=hypotf(real->lat[0],real->lat[1]);if(!(ll>1e-4f)){s->p.depth_scale=d_own;return;}float la0=real->lat[0]/ll,la1=real->lat[1]/ll;
    float lx=s->last_x[w],lz=s->last_z[w],lf=s->last_floor[w];int lh=s->has_last[w];
    for(unsigned k=1;k<g->n;k++){
        MxbGateLine *L=g->l+k;MxbSoilSimWheel q=*real;
        float X=real->x*nm+la0*L->off,Z=real->z*nm+la1*L->off;q.x=X/nm;q.z=Z/nm;
        float y=mxb_ss_sample_m(s,X,Z)+mxb_pw_base(s,X,Z),dt=real->dt,bias=0.f,gain=1.f,tb=.04f*dnk;
        if(!L->valid[w]||landed||!(dt>0.f)){L->y[w]=L->zb[w]=y;L->zbd[w]=0.f;L->valid[w]=1;}
        else{const float wb=6.2831853f*L->fk*fmaxf(2.5f,fminf(6.f,1.8f+.145f*real->speed));
            float h=dt/8.f,y0=L->y[w],yd=(y-y0)/dt,zb=L->zb[w],zbd=L->zbd[w],ab=0.f;
            for(int i=0;i<8;i++){float yk=y0+(y-y0)*(float)(i+1)/8.f;ab=wb*wb*(yk-zb)+2.f*.30f*wb*(yd-zbd);zbd+=ab*h;zb+=zbd*h;}
            if(!isfinite(zb)||fabsf(zb-y)>1.f){L->zb[w]=y;L->zbd[w]=0.f;}else{L->zb[w]=zb;L->zbd[w]=zbd;}
            L->y[w]=y;float sb=fmaxf(-1.f,fminf(1.f,ab/9.81f));
            /* v8.3: live passes kept flattening the braking rows (pre-wear 14.6 cm -> 12.9 after 4 laps;
               live-only levelled off ~13 vs Pro 17.4): braking troughs may go 3x deeper and braking
               hits work the ground 3x harder, braking only (cruise/corners unchanged) */
            float st2=stress*(1.f+2.f*brk);gain=fmaxf(.5f,fminf(1.6f,1.f+(.12f+.3f*brk)*st2*sb));bias=-kb*st2*sb;tb=(.04f+.42f*brk)*dnk*fminf(ef,1.3f);}
        q.gain=gain;q.floor_bias=bias;q.bias_floor=tb;q.brake=brk;q.print_fade=1.f; /* the player's own tyre leaves the fresh print */
        s->last_x[w]=L->lx[w];s->last_z[w]=L->lz[w];s->last_floor[w]=L->lfl[w];s->has_last[w]=landed?0:L->lh[w];
        mxb_soil_sim_wheel(s,w,&q);
        L->lx[w]=s->last_x[w];L->lz[w]=s->last_z[w];L->lfl[w]=s->last_floor[w];L->lh[w]=s->has_last[w];
    }
    s->last_x[w]=lx;s->last_z[w]=lz;s->last_floor[w]=lf;s->has_last[w]=lh;s->p.depth_scale=d_own;
}
#endif
