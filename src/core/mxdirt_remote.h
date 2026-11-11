#ifndef MXDIRT_REMOTE_H
#define MXDIRT_REMOTE_H
/* Online riders (EXPERIMENTAL, untested online): the other bikes of an online
   race wear the track too. The game only tells a plugin where each bike is
   (RaceTrackPosition: race number + world position, at whatever rate it likes),
   so their tyre passes are made up from that track of positions:

     position track -> speed, heading, acceleration, turning -> two wheel
     passes (front / rear) every ~30 cm of travel, with a plausible load and
     drive / brake slip. No pass while a bike is in the air (its height over
     the ground) or crashed.

   Nothing is trusted blindly. The axes of the reported positions are not
   documented, so the mapping is CALIBRATED against the local bike: one of the
   eight axis / sign layouts must put one reported bike on the local bike's own
   wheels (known exactly from the physics) for MXD_REMOTE_LOCK_M metres of
   riding. Until that holds - and again whenever it stops holding - no remote
   rider wears anything. The matching entry is the local bike and is skipped.

   Every client builds its own track from what it sees, so tracks are similar
   between players, never identical. Pure data in, pure data out: no process
   access, no allocation, one owner. All positions in metres. */
#include <math.h>
#include <stdint.h>
#include <string.h>
#define MXD_REMOTE_MAX 48       /* riders tracked */
#define MXD_REMOTE_PASSES 192   /* wheel passes handed out per frame at most */
#define MXD_REMOTE_LOCK_M 60.f  /* metres the local bike must be followed before a mapping is trusted */
typedef struct {int num;float p[3];int crashed;} MxdRemotePos; /* as the game reports a bike */
typedef struct {
    int rider,wheel;            /* rider slot; 0 front, 1 rear */
    float X,Z,fwd[2];           /* contact, terrain-centred metres; unit travel direction */
    float speed,slip_long,load,dt,turn,brake,impact;
    int restart;                /* 1 = not joined to this wheel's previous pass (after a jump, a reset, a gap) */
} MxdRemotePass;
typedef struct {
    int used,num,crashed;
    float p[3];double t;        /* latest reported position, mapped: X, Z terrain-centred, Y up */
    float e[3];double et;int has_e; /* where the passes handed out so far end */
    float v,acc,h[2];int has_h; /* speed, smoothed acceleration, heading */
    float air,landed;           /* s in the air so far (0 = on the ground); s since touching down */
    int joined;
    double seen;
} MxdRemoteRider;
typedef struct {
    int map,self_num;           /* map -1 = not calibrated; else bit0: second ground axis is p[1] (else p[2]), bit1: -x, bit2: -z */
    int cand_num[8];float cand_run[8];
    int bad;                    /* batches in a row in which the local bike's entry was not on the local bike */
    float ref_off,y_off;int has_y; /* reported point ahead of the wheel midpoint, m; its height over the ground, m */
    float self_x,self_z,self_f[2],self_ground,self_step;int self_fresh,self_grounded,has_self;
    float origin[3];
    MxdRemoteRider r[MXD_REMOTE_MAX];
    uint64_t passes,batches,locks;
} MxdRemote;
static inline void mxd_remote_reset(MxdRemote *m,const float origin[3]){
    memset(m,0,sizeof(*m));m->map=-1;m->self_num=-1;for(int k=0;k<8;k++)m->cand_num[k]=-1;
    if(origin)memcpy(m->origin,origin,sizeof(m->origin));
}
static inline void mxd_remote_map(const MxdRemote *m,int map,const float p[3],float out[3]){
    out[0]=((map&2)?-p[0]:p[0])-m->origin[0];out[2]=((map&4)?-1.f:1.f)*p[(map&1)?1:2]-m->origin[2];out[1]=p[(map&1)?2:1];
}
/* The local bike this frame: wheel midpoint X,Z (terrain-centred m), unit travel direction, both wheels on
   the ground?, absolute ground height under it (any fixed zero). Call once per simulated frame. */
static inline void mxd_remote_self(MxdRemote *m,float X,float Z,const float fwd[2],int grounded,float ground){
    if(!isfinite(X)||!isfinite(Z)||!isfinite(fwd[0])||!isfinite(fwd[1])||!isfinite(ground))return;
    float st=m->has_self?hypotf(X-m->self_x,Z-m->self_z):0.f;if(st>3.f)st=0.f; /* a reset / respawn is not riding */
    m->self_step+=st; /* metres ridden since the last batch of reports looked at it */
    m->self_x=X;m->self_z=Z;m->self_f[0]=fwd[0];m->self_f[1]=fwd[1];m->self_grounded=grounded;m->self_ground=ground;m->self_fresh=1;m->has_self=1;
}
/* One batch of reported bikes. now: monotonic seconds. */
static inline void mxd_remote_feed(MxdRemote *m,const MxdRemotePos *in,int n,double now){
    if(!m||!in||n<=0||!isfinite(now))return;if(n>256)n=256;m->batches++;
    /* --- calibration against the local bike (only with a position of it from this very frame) */
    if(m->self_fresh){m->self_fresh=0;const float step=m->self_step;m->self_step=0.f;
        if(m->map<0){
            int best=-1,second=-1;
            for(int k=0;k<8;k++){int who=-1;float bd=2.5f;
                for(int i=0;i<n;i++){float q[3];if(!isfinite(in[i].p[0])||!isfinite(in[i].p[1])||!isfinite(in[i].p[2]))continue;
                    mxd_remote_map(m,k,in[i].p,q);float d=hypotf(q[0]-m->self_x,q[2]-m->self_z);if(d<bd){bd=d;who=in[i].num;}}
                if(who<0||who!=m->cand_num[k]){m->cand_num[k]=who;m->cand_run[k]=0.f;}else m->cand_run[k]+=step;
                if(best<0||m->cand_run[k]>m->cand_run[best]){second=best;best=k;}else if(second<0||m->cand_run[k]>m->cand_run[second])second=k;}
            /* one layout follows the bike, no other comes close (a bike on a symmetry line could fit two for a while) */
            if(best>=0&&m->cand_run[best]>=MXD_REMOTE_LOCK_M&&(second<0||m->cand_run[second]<.5f*MXD_REMOTE_LOCK_M)){
                m->map=best;m->self_num=m->cand_num[best];m->bad=0;m->locks++;m->has_y=0;m->ref_off=0.f;
                for(int i=0;i<MXD_REMOTE_MAX;i++)m->r[i].used=0;}
        }else{
            int found=0;
            for(int i=0;i<n;i++)if(in[i].num==m->self_num){float q[3];mxd_remote_map(m,m->map,in[i].p,q);float dx=q[0]-m->self_x,dz=q[2]-m->self_z;
                if(isfinite(dx)&&isfinite(dz)&&hypotf(dx,dz)<4.f){found=1;
                    if(step>.02f){float along=dx*m->self_f[0]+dz*m->self_f[1];m->ref_off+=(fmaxf(-1.5f,fminf(1.5f,along))-m->ref_off)*.02f;
                        if(m->self_grounded){float y=q[1]-m->self_ground;if(!m->has_y){m->y_off=y;m->has_y=1;}else m->y_off+=(y-m->y_off)*.02f;}}}
                break;}
            if(found)m->bad=0;else if(++m->bad>120){ /* the local bike is no longer where its entry says: trust nothing */
                m->map=-1;m->self_num=-1;for(int k=0;k<8;k++){m->cand_num[k]=-1;m->cand_run[k]=0.f;}
                for(int i=0;i<MXD_REMOTE_MAX;i++)m->r[i].used=0;}
        }
    }
    if(m->map<0)return;
    /* --- the other bikes */
    for(int i=0;i<n;i++){if(in[i].num==m->self_num)continue;
        float q[3];mxd_remote_map(m,m->map,in[i].p,q);if(!isfinite(q[0])||!isfinite(q[1])||!isfinite(q[2]))continue;
        int slot=-1,free_slot=-1;
        for(int k=0;k<MXD_REMOTE_MAX;k++){if(m->r[k].used&&m->r[k].num==in[i].num){slot=k;break;}
            if(free_slot<0&&(!m->r[k].used||now-m->r[k].seen>5.))free_slot=k;}
        if(slot<0){if(free_slot<0)continue;slot=free_slot;memset(&m->r[slot],0,sizeof(m->r[slot]));m->r[slot].used=1;m->r[slot].num=in[i].num;}
        MxdRemoteRider *r=&m->r[slot];r->p[0]=q[0];r->p[1]=q[1];r->p[2]=q[2];r->t=now;r->seen=now;r->crashed=in[i].crashed!=0;
    }
}
/* The wheel passes of every other bike since the last call. ground(user,X,Z): absolute ground height there
   (same zero as mxd_remote_self's), or NULL = no height check (bikes over jumps then cut the ground under
   them). Returns the number of passes written (<= cap). */
static inline int mxd_remote_emit(MxdRemote *m,MxdRemotePass *out,int cap,float (*ground)(void *,float,float),void *user){
    int n=0;if(!m||!out||m->map<0)return 0;if(cap>MXD_REMOTE_PASSES)cap=MXD_REMOTE_PASSES;
    for(int k=0;k<MXD_REMOTE_MAX;k++){MxdRemoteRider *r=&m->r[k];if(!r->used||(r->has_e&&!(r->t>r->et)))continue;
        if(!r->has_e){r->e[0]=r->p[0];r->e[1]=r->p[1];r->e[2]=r->p[2];r->et=r->t;r->has_e=1;r->joined=0;r->has_h=0;r->v=0.f;r->acc=0.f;r->landed=9.f;continue;}
        float dt=(float)(r->t-r->et),dx=r->p[0]-r->e[0],dz=r->p[2]-r->e[2],dist=hypotf(dx,dz);
        if(!(dt>0.f)||dt>.6f||dist>45.f*dt+3.f){ /* a gap or a jump in the reports (reset, lag): start over from here */
            r->e[0]=r->p[0];r->e[1]=r->p[1];r->e[2]=r->p[2];r->et=r->t;r->joined=0;r->has_h=0;r->v=0.f;r->acc=0.f;continue;}
        if(dist<.03f){if(dt>.4f){r->et=r->t;r->v=0.f;r->acc=0.f;r->has_h=0;}continue;} /* standing (or the report has not moved yet) */
        const float v=dist/dt,h0=dx/dist,h1=dz/dist;float turn=0.f;
        if(r->has_h){float a=fminf(1.f,dt/.25f);r->acc+=((v-r->v)/dt-r->acc)*a;
            float ang=atan2f(r->h[0]*h1-r->h[1]*h0,r->h[0]*h0+r->h[1]*h1);turn=fminf(1.f,fabsf(ang)/dt*v/9.f);}
        int flying=r->crashed;
        if(!flying&&ground&&m->has_y){float over=r->p[1]-ground(user,r->p[0],r->p[2])-m->y_off;flying=over>(r->air>0.f?.35f:.5f);}
        if(flying||v<1.5f){ /* in the air, down, or barely rolling: no wear */
            if(flying){r->air+=dt;r->joined=0;}
            r->e[0]=r->p[0];r->e[1]=r->p[1];r->e[2]=r->p[2];r->et=r->t;r->v=v;r->h[0]=h0;r->h[1]=h1;r->has_h=1;continue;}
        if(r->air>0.f){ /* first report back on the ground: the stretch since the last one was the landing, part of it still in the air */
            if(r->air>.15f)r->landed=0.f;r->air=0.f;
            r->e[0]=r->p[0];r->e[1]=r->p[1];r->e[2]=r->p[2];r->et=r->t;r->v=v;r->h[0]=h0;r->h[1]=h1;r->has_h=1;continue;}
        int steps=(int)ceilf(dist/.3f);if(steps<1)steps=1;if(steps>32)steps=32;
        const float brake=fminf(1.f,fmaxf(0.f,(-r->acc-2.5f)/6.f)),drive=fminf(1.f,fmaxf(0.f,(r->acc-1.5f)/4.f));
        for(int j=0;j<steps&&n+2<=cap;j++){float f=((float)j+1.f)/(float)steps,mx=r->e[0]+dx*f-h0*m->ref_off,mz=r->e[2]+dz*f-h1*m->ref_off;
            float impact=r->landed<.25f?1.f-r->landed/.25f:0.f;
            for(int w=0;w<2;w++){MxdRemotePass *o=out+n++;memset(o,0,sizeof(*o));o->rider=k;o->wheel=w;
                o->X=mx+(w?-.72f:.72f)*h0;o->Z=mz+(w?-.72f:.72f)*h1;o->fwd[0]=h0;o->fwd[1]=h1;o->speed=v;o->dt=dt/(float)steps;
                o->turn=turn;o->brake=fmaxf(brake,w?drive:0.f);o->impact=impact;o->restart=!r->joined;
                if(w){o->load=1050.f*(1.f+.35f*drive)*(1.f+impact);o->slip_long=brake>0.f?(.04f+.10f*brake)*v:drive>0.f?-(.06f+.22f*drive)*fmaxf(v,3.f):0.f;}
                else{o->load=750.f*(1.f+.5f*brake)*(1.f+impact);o->slip_long=brake>0.f?(.04f+.14f*brake)*v:0.f;}}
            r->joined=1;r->landed+=dt/(float)steps;m->passes+=2;}
        r->e[0]=r->p[0];r->e[1]=r->p[1];r->e[2]=r->p[2];r->et=r->t;r->v=v;r->h[0]=h0;r->h[1]=h1;r->has_h=1;
    }
    return n;
}
static inline int mxd_remote_riders(const MxdRemote *m,double now){
    int c=0;if(m&&m->map>=0)for(int k=0;k<MXD_REMOTE_MAX;k++)if(m->r[k].used&&now-m->r[k].seen<5.)c++;return c;
}
#endif
