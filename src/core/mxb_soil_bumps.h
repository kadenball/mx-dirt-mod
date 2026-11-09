#ifndef MXB_SOIL_BUMPS_H
#define MXB_SOIL_BUMPS_H
/* v7.7 bump stress by situation, shared by live riding and the pre-wear gate
   (Maryland Pro catalogue along the user's laps): braking zones grow the tallest
   rows (~2x elsewhere, fork dive), straights and faces get long rollers, while
   turning bikes carve instead of pitching (corners keep the v7.5 ruts and berms),
   landings stay near clean (the suspension soaks the hit) and exits under power
   are calmer than cruising.
   v7.8 (whole-lap comparison): Pro's rows are not even - its 30-38 cm hot spots sit
   where bikes are FAST (19-22 m/s): downhill braking into corners and 10-30 m past
   big landings back on the gas; slow parts stay near clean. Hits scale with impact
   energy (v^2), and braking is read from the bike actually slowing (fork dive),
   not only from tyre slip (the user's laps barely slip under braking). */
#include <math.h>
#ifndef MXB_PW_ROLLING
#define MXB_PW_ROLLING .15f /* pre-wear / gate-line rolling stress (live rolling_bumps is .6); v9.1 .22->.15: Pro is calm off the braking/gas zones */
#endif
#ifndef MXB_BRAKE_K
#define MXB_BRAKE_K 19.f
#endif
typedef struct{float f0,f1,cv,since,v,dec,gy,gs;int has_f;}MxbBumpSit; /* gy/gs: ground height and its smoothed slope along travel */
static inline float mxb_bump_clamp01(float v){return fminf(1.f,fmaxf(0.f,v));}
/* v8.7: 0 straight .. 1 turning hard (path curvature > ~.12 /m), from the same tracker */
static inline float mxb_bump_turn(const MxbBumpSit *b){return mxb_bump_clamp01((b->cv-.04f)/.08f);}
/* dt s, speed m/s, sl = slip_long/speed (+ braking, - driving), (f0,f1) heading,
   landed: back from the air this frame, rolling: stress with no braking.
   Returns the row stress; *brk = braking share (0..1, eased in turns) for the
   deeper braking troughs and dig. */
static inline float mxb_bump_stress(MxbBumpSit *b,float dt,float speed,float sl,float f0,float f1,float ground_y,int landed,float rolling,float *brk,float *energy){
    float fl=hypotf(f0,f1);if(fl>1e-4f){f0/=fl;f1/=fl;}else{f0=b->f0;f1=b->f1;}
    float cr=b->has_f?b->f0*f1-b->f1*f0:0.f;b->f0=f0;b->f1=f1;
    /* deceleration (m/s2), smoothed over ~.25 s */
    if(b->has_f&&dt>0.f)b->dec+=fminf(1.f,dt/.25f)*(fmaxf(-20.f,fminf(20.f,(b->v-speed)/dt))-b->dec);
    /* v8.5 ground slope along travel (smoothed ~.3 s): down a hill a bike brakes hard just to hold
       its speed - braking effort = deceleration + g*downslope. Pro's biggest moguls are all in
       downhill braking zones (5-9 m of drop over 16 m) where riders barely slow down. */
    if(b->has_f&&dt>0.f&&speed>1.f&&isfinite(ground_y)){float gsl=fmaxf(-1.f,fminf(1.f,(ground_y-b->gy)/(speed*dt)));b->gs+=fminf(1.f,dt/.3f)*(gsl-b->gs);}
    b->gy=ground_y;b->v=speed;b->has_f=1;
    /* path curvature (1/m) from the heading change, smoothed over ~.1 s */
    b->cv+=fminf(1.f,dt/.115f)*(fabsf(asinf(fmaxf(-1.f,fminf(1.f,cr))))/fmaxf(.05f,speed*dt)-b->cv);
    if(landed)b->since=0.f;else b->since+=dt;
    float cf=.15f+.85f*mxb_bump_clamp01((speed-7.f)/5.f);           /* slow: turning, not pitching */
    float tf=fmaxf(.1f,fminf(1.f,1.f-(b->cv-.04f)/.08f));           /* turning */
    float lf=fmaxf(.2f,fminf(1.f,(b->since-.15f)/.4f));             /* just landed (v7.9: Pro rows start ~8 m past touchdown) */
    float ds=mxb_bump_clamp01((-sl-.04f)/.15f);                      /* under power */
    float effort=b->dec+9.81f*fmaxf(0.f,-b->gs)*1.f-9.81f*fmaxf(0.f,b->gs); /* v9: and up a climb gravity does the slowing - a bike climbing a jump face is not braking (it grew braking rows there) */ /* coasting downhill would speed up by g*downslope: whatever is missing is braking (the 1.5 m/s2 offset below covers drag) */
    float brs=fmaxf(mxb_bump_clamp01((sl-.04f)/.18f),mxb_bump_clamp01((effort-1.5f)/4.f)); /* braking: slip or effort */
    float ef=fmaxf(.05f,fminf(3.f,speed*speed/(21.f*21.f))); /* impact energy, 1 at 21 m/s */
    *energy=ef;
    *brk=brs*tf;
    /* calm under power only at corner-exit speeds: hard on the gas at 19+ m/s (out of a
       landing, up a fast straight) Pro has some of its biggest rows */
    float dsk=.8f*ds*(1.f-mxb_bump_clamp01((speed-9.f)/4.f)); /* v7.9: only true corner exits (Pro: big rows on the gas out of landings at ~15 m/s) */
    return fmaxf(rolling*(1.f-dsk)*lf,MXB_BRAKE_K*rolling*brs)*cf*tf*ef;
}
#endif
