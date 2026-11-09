#ifndef MXB_SOIL_PREWEAR_H
#define MXB_SOIL_PREWEAR_H
#ifndef MXB_PW_CAP
#define MXB_PW_CAP .3f /* how hard a rider steers into an existing rut each sample */
#endif
#ifndef MXB_PW_WANDER
#define MXB_PW_WANDER .12f /* each rider's slow weave on its line (m) */
#endif
/* v7 pre-wear: before the first step, a full gate of virtual riders rides the
   recorded laps once, so the track starts worn like a pro moto (measured on
   Maryland Pro vs Ams: 7-8 parallel ruts ~6-7 cm deep, ~.55 m wide, ~1.35 m
   apart across ~5-7 m of track, with rolling bumps) instead of growing one dug
   line in front of the player. Each rider takes a lap with a lateral offset
   N(0,spread) clipped to +-clip, drops into an existing rut within reach (what
   the game's physics does to a real bike) and runs the authored wheel-hop
   spring (5 Hz) for braking bumps / chop. union (tiles_x*tiles_y bytes) gets
   every tile touched; box = touched texel bounds {x0,z0,x1,z1} (x1<0: none). */
#include "mxb_soil_sim.h"
#include "mxb_dirt_ghost.h"
#include "mxb_soil_bumps.h"
static inline float mxb_pw_rand(uint32_t *st){uint32_t x=*st;x^=x<<13;x^=x>>17;x^=x<<5;*st=x?x:1u;return (float)(x&0xffffff)/16777216.f;}
static inline float mxb_pw_normal(uint32_t *st){float a=mxb_pw_rand(st)+1e-7f,b=mxb_pw_rand(st);return sqrtf(-2.f*logf(a))*cosf(6.2831853f*b);}
/* v7.9 a real gate does not all clear every jump: some land short, some case it, a few
   roll it - Pro is worn 2-6 cm under every one of the user's jumps, where our gate,
   copying the user's airtime exactly, never touched down. Each airtime gap of a lap gets
   ground contacts along the chord from take-off to touchdown (only real jumps: < 3 s, < 40 m -
   a reset/respawn in a recorded lap must not draw a line across the infield) (pad[0]=1, gain = fraction
   of the gap); each rider decides per jump where it lands. Returns a new sample array. */
static MxbDirtGhostSample *mxb_pw_fill_jumps(const MxbDirtGhostLap *g,uint32_t *out_n){
    /* per wheel: recorded laps do not always alternate front/rear (one wheel in the air) */
    uint32_t n=g->h.samples,cap=n+n/2+64,k=0;MxbDirtGhostSample *o=(MxbDirtGhostSample*)malloc((size_t)cap*sizeof(*o));if(!o)return NULL;
    long last[2]={-1,-1};
    for(uint32_t i=0;i<n;i++){
        const MxbDirtGhostSample *p1=g->s+i;unsigned w=p1->wheel>1?1:p1->wheel;
        if(last[w]>=0){const MxbDirtGhostSample *p0=g->s+last[w];float gap=p1->t-p0->t;
            if(gap>.2f&&gap<3.f&&p1->dt>0.f&&hypotf(p1->x-p0->x,p1->z-p0->z)*.3048f<40.f){
                int fr=(int)(gap/p1->dt)-1;if(fr>240)fr=240;
                for(int f=1;f<=fr&&k+1<cap;f++){float a=(float)f/(float)(fr+1);MxbDirtGhostSample q=*p1;
                    q.t=p0->t+gap*a;q.x=p0->x+(p1->x-p0->x)*a;q.z=p0->z+(p1->z-p0->z)*a;q.speed=p0->speed+(p1->speed-p0->speed)*a;
                    q.slip_long=0.f;q.slip_lat=0.f;q.gain=a;q.pad[0]=1;o[k++]=q;}}}
        if(k+1>=cap)break;
        o[k]=*p1;o[k].pad[0]=0;k++;last[w]=(long)i;}
    /* the two wheels' fills were appended at their own touchdowns: restore time order (stable) */
    for(uint32_t i=1;i<k;i++){MxbDirtGhostSample v=o[i];uint32_t j=i;while(j>0&&o[j-1].t>v.t){o[j]=o[j-1];j--;}o[j]=v;}
    *out_n=k;return o;
}/* track shape (m) under a point, bilinear on the sim's base (0 without one) */
static inline float mxb_pw_base(const MxbSoilSim *s,float X,float Z){
    if(!s->base)return 0.f;float c,r;mxb_ss_texel(s,X,Z,&c,&r);int i=(int)floorf(c),j=(int)floorf(r);
    if(i<0||j<0||i>=(int)s->size-1||j>=(int)s->size-1)return 0.f;float fc=c-(float)i,fr=r-(float)j;size_t k=(size_t)j*s->size+i;
    return (s->base[k]+(s->base[k+1]-s->base[k])*fc)*(1.f-fr)+(s->base[k+s->size]+(s->base[k+s->size+1]-s->base[k+s->size])*fc)*fr;
}
static inline uint64_t mxb_soil_prewear(MxbSoilSim *m,const MxbDirtGhostLap *laps,unsigned count,unsigned riders,
    float spread,float clip,float depth_k,uint32_t seed,unsigned char *uni,int box[4]){
    /* v7.6 two-mode suspension (Maryland Pro bump catalogue along the user's laps):
       - body mode: the chassis pitching/bouncing. Pro's main bump is a long roller
         3.4 m apart at 12 m/s, 4.4 m at 21 m/s (spacing grows with speed, ~3.6->4.9 Hz),
         twice as tall in braking zones (fork dive) as when cruising, on exits, landings
         and faces. Rate 1.8+.145 v Hz.
       - wheel-hop mode (~10 Hz): short chop (~1-1.5 m) under power on exits and a little
         under hard braking. Pro's 27 cm grid can barely show it, so it stays small.
       Each tyre's rut floor follows the load swing of both modes on the REAL ground
       under it (track shape + wear), so every bump type grows where it naturally would.
       The recorded lap's own load trace is no longer mixed in: at 60 Hz it carried
       short noise that read as chop on straights and faces. Scales with erode. */
    const float dnk=fmaxf(.3f,fminf(2.5f,m->p.depth_scale/.5f));
    const float kb=.017f*dnk,kh=.012f*dnk;
    /* tuned on Maryland vs Pro: kb/kh body/hop row depth per unit stress; sb0 rolling stress everywhere,
       gb extra dig in braking troughs, bf deeper braking trough bound; situations: mxb_soil_bumps.h
       (shared with live riding) */
    const float bf=.14f,gb=.3f,sb0=MXB_PW_ROLLING;
    /* gate riders cut a fraction of a live pass (session: .14, Pro grooves ~5.7 cm) and
       replay every 2nd recorded frame with double dt (the footprint stretches
       over the gap): half the cost, the same wear */
    const float depth0=m->p.depth_scale,nm=m->native_m,glo0=m->p.gain_lo,ghi0=m->p.gain_hi;m->p.depth_scale=depth0*depth_k;
    m->p.gain_lo=.45f;m->p.gain_hi=1.65f; /* a whole gate builds rows a single pass cannot: wider modulation */
    const float roost0=m->p.roost_gain;m->p.roost_gain=roost0*3.f; /* 80 riders spinning wherever you spun dug holes Pro exits do not have */
    uint32_t st=seed?seed:0x2545f491u;uint64_t n=0;size_t tn=(size_t)m->tiles_x*m->tiles_y;
    box[0]=box[1]=INT32_MAX;box[2]=box[3]=-1;
    MxbDirtGhostLap fl[8];unsigned nf=count>8?8:count;
    for(unsigned c=0;c<nf;c++){fl[c].h=laps[c].h;fl[c].s=mxb_pw_fill_jumps(laps+c,&fl[c].h.samples);if(!fl[c].s)fl[c]=laps[c];}
    for(unsigned r=0;r<riders;r++){
        const MxbDirtGhostLap *g=fl+r%nf;float jl=2.f,lastt[2]={-100.f,-100.f};int in_fill=0;
        float off=spread*mxb_pw_normal(&st);if(off>clip)off=clip;if(off<-clip)off=-clip;
        float guide=0.f,wander=6.2831853f*mxb_pw_rand(&st),aggr=.7f+.5f*mxb_pw_rand(&st),slide_k=.45f+.55f*mxb_pw_rand(&st);
        long shift=2L*(long)lrintf(8.f*mxb_pw_normal(&st)); /* same-wheel samples: even */
        /* v7.9 each rider's own bike and setup (250/450, stiff/soft): its own bounce rate, so
           rows from different riders differ in spacing, cross and break into moguls as on Pro
           (v9: only a little: wide spread made moguls where Pro has rows straight across the band) */
        float fk=1.f+.05f*mxb_pw_normal(&st);if(fk<.75f)fk=.75f;if(fk>1.3f)fk=1.3f;
        struct{float y,zb,zbd,zh,zhd;MxbBumpSit sit;int valid;}b[2];memset(b,0,sizeof(b));b[0].sit.since=b[1].sit.since=100.f;m->has_last[0]=m->has_last[1]=0;
        for(uint32_t i=0;i<g->h.samples;){
            mxb_soil_sim_begin(m);
            for(int k=0;k<32&&i<g->h.samples;k++,i++){ /* 16 frames per settle (relaxation) step */
                const MxbDirtGhostSample *p=g->s+i;unsigned w=p->wheel>1?1:p->wheel;
                if((i>>1)&1u)continue; /* every 2nd frame (samples come as wheel pairs) */
                if(p->pad[0]){ /* under a jump: this rider's touchdown for this jump */
                    if(!in_fill){in_fill=1;float u=mxb_pw_rand(&st);jl=u<.5f?2.f:u<.85f?.45f+.5f*mxb_pw_rand(&st):.05f+.4f*mxb_pw_rand(&st);}
                    if(p->gain<jl||p->gain<.15f)continue;} /* v8.4: nobody loads the lip edge itself (rollers unweight over it; those contacts piled ~35 cm kickers) */
                else in_fill=0;
                float ll=hypotf(p->lat[0],p->lat[1]);if(!(ll>1e-4f)||!(p->dt>0.f))continue;
                float la0=p->lat[0]/ll,la1=p->lat[1]/ll,X=p->x*nm,Z=p->z*nm;
                float lo=off+MXB_PW_WANDER*sinf(.15f*p->t+wander);
                if(w==1){ /* the rear wheel leads the rut search; the front follows the same offset */
                    float h0=mxb_ss_sample_m(m,X+la0*(lo+guide),Z+la1*(lo+guide)),best=0.f,bo=0.f;
                    for(int j=-9;j<=9;j++){float o=.05f*(float)j;float dd=h0-mxb_ss_sample_m(m,X+la0*(lo+guide+o),Z+la1*(lo+guide+o))-.04f*fabsf(o);if(dd>best){best=dd;bo=o;}}
                    guide+=MXB_PW_CAP*(bo*fminf(1.f,fmaxf(0.f,(best-.015f)/.03f)));if(guide>.6f)guide=.6f;if(guide<-.6f)guide=-.6f;}
                float o=lo+guide;X+=la0*o;Z+=la1*o;
                float y=mxb_ss_sample_m(m,X,Z)+mxb_pw_base(m,X,Z),gain=1.f,dt=2.f*p->dt,bias=0.f,tb=0.f,pbrk=0.f;
                /* each rider gets on and off the gas at its own spot (+-~1 m) and with its own
                   aggression - the whole gate spinning at your exact spot dug exit holes */
                const MxbDirtGhostSample *ps=p;{long jj=(long)i+shift;if(jj>=0&&jj<(long)g->h.samples&&g->s[jj].wheel==p->wheel)ps=g->s+jj;}
                float slip_long=ps->slip_long*(ps->slip_long<0.f?aggr:1.f),slip_lat=ps->slip_lat*slide_k;
                int landed=p->t-lastt[w]>.2f&&lastt[w]>-50.f; /* came back from the air */lastt[w]=p->t;
                if(landed)m->has_last[w]=0; /* soft touchdown (sim cuts 30 %) */
                if(landed)b[w].sit.since=0.f;
                if(!b[w].valid||landed){b[w].y=y;b[w].zb=b[w].zh=y;b[w].zbd=b[w].zhd=0.f;b[w].sit.cv=0.f;b[w].valid=1;}
                else{const float fb=fk*fmaxf(2.5f,fminf(6.f,1.8f+.145f*p->speed)),wb=6.2831853f*fb,wh=6.2831853f*10.f*fk;
                    float h=dt/8.f,y0=b[w].y,yd=(y-y0)/dt,zb=b[w].zb,zbd=b[w].zbd,zh=b[w].zh,zhd=b[w].zhd,ab=0.f,ah=0.f;
                    for(int q=0;q<8;q++){float yk=y0+(y-y0)*(float)(q+1)/8.f;
                        ab=wb*wb*(yk-zb)+2.f*.30f*wb*(yd-zbd);zbd+=ab*h;zb+=zbd*h;
                        ah=wh*wh*(yk-zh)+2.f*.20f*wh*(yd-zhd);zhd+=ah*h;zh+=zhd*h;}
                    if(!isfinite(zb)||!isfinite(zh)||fabsf(zb-y)>1.f||fabsf(zh-y)>1.f){b[w].zb=b[w].zh=y;b[w].zbd=b[w].zhd=0.f;}
                    else{b[w].zb=zb;b[w].zbd=zbd;b[w].zh=zh;b[w].zhd=zhd;}
                    b[w].y=y;
                    float sl=slip_long/fmaxf(p->speed,1.f),brake=fmaxf(0.f,fminf(1.f,(sl-.08f)/.45f)),drive=fmaxf(0.f,fminf(1.f,(-sl-.10f)/.65f));
                    float sb=fmaxf(-1.f,fminf(1.f,ab/9.81f)),sh=fmaxf(-1.f,fminf(1.f,ah/9.81f));
                    float brk,ef,stress_b=mxb_bump_stress(&b[w].sit,dt,p->speed,sl,p->fwd[0],p->fwd[1],y,0,sb0,&brk,&ef),stress_h=fmaxf(.05f,fmaxf(drive,.4f*brake));
                    gain=fmaxf(.5f,fminf(1.6f,1.f+(.12f+gb*brk)*stress_b*sb));
                    bias=-(kb*(1.f+0.f*brk)*stress_b*sb+kh*stress_h*sh);
                    /* braking bumps run deep (Pro: ~2x the rollers elsewhere): troughs may sit
                       up to ~9 cm below the smoothed ground under full braking, 4 cm otherwise */
                    tb=(.04f+bf*(1.f+0.f)*brk)*dnk*fminf(ef,1.3f);pbrk=fmaxf(brk,drive);}
                MxbSoilSimWheel q;memset(&q,0,sizeof(q));q.x=X/nm;q.z=Z/nm;q.fwd[0]=p->fwd[0];q.fwd[1]=p->fwd[1];q.lat[0]=p->lat[0];q.lat[1]=p->lat[1];
                q.speed=p->speed;q.slip_long=slip_long;q.slip_lat=slip_lat;q.load=p->load;q.width=p->width;q.radius=p->radius;
                /* v8.9: riders landing short / rolling a jump pack and wear the gap but build no bump rows
                   there (their rows made sharp-edged mogul blocks under jumps; Pro is clean there) */
                if(p->pad[0]){gain=1.f;bias=0.f;}
                q.gain=gain;q.dt=dt;q.valid=1;q.material=p->material;q.floor_bias=bias;q.bias_floor=tb;q.print_fade=.65f;q.turn=3.f*mxb_bump_turn(&b[w].sit); /* pre-wear passes are shallow (.10): corners need more to reach v7.5's ruts */q.brake=p->pad[0]?0.f:pbrk;q.impact=b[w].sit.since<.5f?1.f-b[w].sit.since/.5f:0.f;
                mxb_soil_sim_wheel(m,w,&q);n++;
            }
            mxb_soil_sim_end(m);
            if(uni)for(size_t t=0;t<tn;t++)uni[t]|=m->tiles[t];
            if(m->box){if(m->bx0<box[0])box[0]=m->bx0;if(m->bz0<box[1])box[1]=m->bz0;if(m->bx1>box[2])box[2]=m->bx1;if(m->bz1>box[3])box[3]=m->bz1;}
        }
    }
    /* final tidy: the live spike guard levels half the excess per step, so the gate's last
       passes can leave lone cells ~9 cm proud of their neighbours; level those fully (mass kept) */
    if(box[2]>=0){mxb_soil_sim_begin(m);const int S=(int)m->size;
        for(int it=0;it<3;it++)for(int r=box[1]>1?box[1]:1;r<=box[3]&&r<S-1;r++)for(int q=box[0]>1?box[0]:1;q<=box[2]&&q<S-1;q++){
            size_t k=(size_t)r*S+q;float ex=mxb_ss_m(m,k)-.25f*(mxb_ss_m(m,k-1)+mxb_ss_m(m,k+1)+mxb_ss_m(m,k-S)+mxb_ss_m(m,k+S))-.05f;
            if(ex>0.f){float mv=.8f*ex;mxb_ss_add(m,q,r,-mv,0);mxb_ss_add(m,q-1,r,.25f*mv,1);mxb_ss_add(m,q+1,r,.25f*mv,1);mxb_ss_add(m,q,r-1,.25f*mv,1);mxb_ss_add(m,q,r+1,.25f*mv,1);}}
        mxb_soil_sim_end(m);if(uni)for(size_t t=0;t<tn;t++)uni[t]|=m->tiles[t];}
    for(unsigned c=0;c<nf;c++)if(fl[c].s!=laps[c].s)free(fl[c].s);
    m->p.depth_scale=depth0;m->p.gain_lo=glo0;m->p.gain_hi=ghi0;m->p.roost_gain=roost0;m->has_last[0]=m->has_last[1]=0;return n;
}
#endif
