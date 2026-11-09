#ifndef MXDIRT_SOFT_LAYER_H
#define MXDIRT_SOFT_LAYER_H
/* Soft top layer (display only): a few cm of loose soil lying on the track,
   like fresh snow but dirt. Tyres press a trench into it and push the soil out
   into lips beside the tread; spinning throws crumbs back, braking shoves a
   mound ahead, sliding pushes it sideways. It never changes the ridden
   terrain: the render plugin draws it over the ground and the bike rides the
   dirt sim's physical ruts under it.

   Grid: MXD_SOFT_CELL m texels in the terrain's local frame. Texel (a,b) covers
   x in [a,a+1)*cell and z in [b,b+1)*cell from the height grid's vertex (0,0);
   b runs with +z. Sparse MXD_SOFT_TILE x MXD_SOFT_TILE tiles, 2 bytes a texel:
     [0] layer surface relative to the untouched layer top: 128 + h/MXD_SOFT_UNIT (1 mm)
     [1] last tyre over it: bit7 fresh tread (knob pits intact),
         bits0..6 travel angle mod pi in 1/128 of pi
   Untouched = {128,0}. A missing tile is untouched. tile_serial[t] changes
   whenever tile t is written, so a reader can upload only what changed. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MXD_SOFT_CELL .025f
#define MXD_SOFT_TILE 32
#define MXD_SOFT_UNIT .001f /* 1 mm steps: +-127 mm */
#define MXD_SOFT_MAX_TILES 64000u /* 2 KB each: ~130 MB at most */
typedef struct {
    float cell,span_x,span_z,thickness;
    uint32_t tiles_x,tiles_z,allocated,refused,rng,serial;
    uint8_t **tile;uint32_t *tile_serial;
    uint64_t stamps;int loaded;
    float last_x[2],last_z[2];int has_last[2]; /* per wheel: where its last pass ended (local m) */
    int defer_relax; /* 1: passes skip their own slump (pre-wear), mxd_soft_relax_all settles everything after */
} MxdSoft;
static inline void mxd_soft_destroy(MxdSoft *s){
    if(!s)return;
    if(s->tile){size_t n=(size_t)s->tiles_x*s->tiles_z;for(size_t i=0;i<n;i++)free(s->tile[i]);}
    free(s->tile);free(s->tile_serial);free(s);
}
/* span: terrain extent in metres; thickness: untouched layer depth, m (.005...10). */
static inline MxdSoft *mxd_soft_create(float span_x,float span_z,float thickness){
    if(!(span_x>1.f)||!(span_z>1.f)||span_x>4000.f||span_z>4000.f||!(thickness>=.005f)||thickness>.10f)return NULL;
    MxdSoft *s=calloc(1,sizeof(*s));if(!s)return NULL;
    s->cell=MXD_SOFT_CELL;s->span_x=span_x;s->span_z=span_z;s->thickness=thickness;s->rng=0x2545f491u;
    s->tiles_x=(uint32_t)ceilf(span_x/s->cell/MXD_SOFT_TILE)+1;s->tiles_z=(uint32_t)ceilf(span_z/s->cell/MXD_SOFT_TILE)+1;
    size_t n=(size_t)s->tiles_x*s->tiles_z;s->tile=calloc(n,sizeof(*s->tile));s->tile_serial=calloc(n,sizeof(*s->tile_serial));
    if(!s->tile||!s->tile_serial){mxd_soft_destroy(s);return NULL;}
    return s;
}
static inline uint8_t *mxd_soft_new_tile(void){
    uint8_t *p=malloc(MXD_SOFT_TILE*MXD_SOFT_TILE*2);if(!p)return NULL;
    for(int i=0;i<MXD_SOFT_TILE*MXD_SOFT_TILE;i++){p[2*i]=128;p[2*i+1]=0;}
    return p;
}
/* Texel (a,b) for writing (allocates its tile and marks it changed), or NULL. */
static inline uint8_t *mxd_soft_at(MxdSoft *s,int a,int b){
    if(a<0||b<0)return NULL;uint32_t tx=(uint32_t)a/MXD_SOFT_TILE,tz=(uint32_t)b/MXD_SOFT_TILE;
    if(tx>=s->tiles_x||tz>=s->tiles_z)return NULL;
    size_t t=(size_t)tz*s->tiles_x+tx;uint8_t *p=s->tile[t];
    if(!p){if(s->allocated>=MXD_SOFT_MAX_TILES||!(p=mxd_soft_new_tile())){s->refused++;return NULL;}s->tile[t]=p;s->allocated++;}
    s->tile_serial[t]=s->serial;
    return p+(((uint32_t)b%MXD_SOFT_TILE)*MXD_SOFT_TILE+(uint32_t)a%MXD_SOFT_TILE)*2;
}
/* Texel for reading only (NULL = untouched / outside). */
static inline const uint8_t *mxd_soft_peek(const MxdSoft *s,int a,int b){
    if(a<0||b<0)return NULL;uint32_t tx=(uint32_t)a/MXD_SOFT_TILE,tz=(uint32_t)b/MXD_SOFT_TILE;
    if(tx>=s->tiles_x||tz>=s->tiles_z)return NULL;const uint8_t *p=s->tile[(size_t)tz*s->tiles_x+tx];
    return p?p+(((uint32_t)b%MXD_SOFT_TILE)*MXD_SOFT_TILE+(uint32_t)a%MXD_SOFT_TILE)*2:NULL;
}
/* Loose soil slumps to its angle of repose: one pass over a box of texels moves
   half of every excess step between neighbours downhill (mass kept). */
static inline void mxd_soft_relax(MxdSoft *s,int a0,int b0,int a1,int b1,int maxd){
    for(int b=b0;b<=b1;b++)for(int a=a0;a<=a1;a++){
        const uint8_t *p=mxd_soft_peek(s,a,b);int h=p?(int)p[0]-128:0;
        for(int d=0;d<2;d++){int na=a+(d==0),nb=b+(d==1);const uint8_t *q=mxd_soft_peek(s,na,nb);int g=q?(int)q[0]-128:0,diff=h-g;
            if(diff>maxd||-diff>maxd){int m=((diff>0?diff:-diff)-maxd+1)/2;if(diff<0)m=-m;
                uint8_t *P=mxd_soft_at(s,a,b),*Q=mxd_soft_at(s,na,nb);if(!P||!Q)continue;
                h-=m;P[0]=(uint8_t)(h+128);Q[0]=(uint8_t)(g+m+128);}}
    }
}
/* Settles every allocated tile (after pre-wear); moisture sets the repose angle. */
static inline void mxd_soft_relax_all(MxdSoft *s,float moisture,int sweeps){
    if(!s)return;const float repose=.67f+.52f*fminf(1.f,fmaxf(0.f,(moisture-.15f)/.4f));const int maxd=(int)lrintf(repose*s->cell/MXD_SOFT_UNIT);
    for(int k=0;k<sweeps;k++)for(uint32_t tz=0;tz<s->tiles_z;tz++)for(uint32_t tx=0;tx<s->tiles_x;tx++)if(s->tile[(size_t)tz*s->tiles_x+tx])
        mxd_soft_relax(s,(int)(tx*MXD_SOFT_TILE),(int)(tz*MXD_SOFT_TILE),(int)(tx*MXD_SOFT_TILE+MXD_SOFT_TILE-1),(int)(tz*MXD_SOFT_TILE+MXD_SOFT_TILE-1),maxd);
}
static inline float mxd_soft_rand(MxdSoft *s){uint32_t x=s->rng;x^=x<<13;x^=x>>17;x^=x<<5;s->rng=x?x:1u;return (float)(x&0xffffff)/16777216.f;}
/* Adds `units` (height steps, stochastically rounded) to the texel under local point x,z. */
static inline void mxd_soft_add(MxdSoft *s,float x,float z,float units,int cap){
    if(!(units>0.f))return;int q=(int)floorf(units+mxd_soft_rand(s));if(q<=0)return;
    uint8_t *p=mxd_soft_at(s,(int)floorf(x/s->cell),(int)floorf(z/s->cell));if(!p)return;
    int h=(int)p[0]-128+q;if(h>cap)h=cap;p[0]=(uint8_t)(h+128);
}
/* One tyre pass over the ground covered since the last one.
   X,Z: contact, metres from the terrain centre; fwd: travel direction (x,z);
   lat: tyre lateral axis; v: ground speed m/s; dt: s; load: N; spin/skid: 0..1
   drive/brake slip; slip_lat: lateral slide m/s along lat; material: MX Bikes
   surface id; rear: 1 = rear tyre; fresh: 1 = leave crisp knob pits; soil: the dirt
   sim's ground under the tyre (NULL = untouched defaults). */
typedef struct {
    float thick;    /* factor on the layer depth here: firmness, packed lines (<1), loose soil piles (>1) */
    float moisture; /* 0 dry .. .35 ideal .. 1 wet: damp soil holds crisp prints and tall narrow lips, dry crumbles */
    float packed;   /* 0..1 how ridden-in the ground is: tyres iron the layer flat on packed lines */
} MxdSoftSoil;
static inline void mxd_soft_stamp(MxdSoft *s,int rear,float X,float Z,const float fwd[2],const float lat[2],
    float v,float dt,float load,float spin,float skid,float slip_lat,int material,int fresh,const MxdSoftSoil *soil){
    if(!s||!(s->thickness>0.f)||!isfinite(X)||!isfinite(Z)||!(dt>0.f)||dt>.1f||!isfinite(v)||!isfinite(load))return;
    float fl=hypotf(fwd[0],fwd[1]);if(!(fl>1e-4f))return;
    /* join this pass to where this wheel's last one ended, so the trench runs on
       continuously even when the line steers between samples (pre-wear riders) */
    const int wi=rear?1:0;const float px=X+.5f*s->span_x,pz=Z+.5f*s->span_z;float g0=fwd[0]/fl,g1=fwd[1]/fl,seg=-1.f;
    if(s->has_last[wi]){float dx=px-s->last_x[wi],dz=pz-s->last_z[wi],d=hypotf(dx,dz);if(d>.005f&&d<1.5f&&dx*g0+dz*g1>.3f*d){g0=dx/d;g1=dz/d;seg=d;}}
    s->last_x[wi]=px;s->last_z[wi]=pz;s->has_last[wi]=1;
    const float f0=g0,f1=g1,n0=-f1,n1=f0;
    /* sand lies deep and loose, hard-pack and gravel carry only a thin skin */
    const float mf=material==5?1.8f:material==10?1.25f:material==11?.55f:material==12?.3f:1.f;
    const float sf=soil&&isfinite(soil->thick)?fminf(1.6f,fmaxf(.3f,soil->thick)):1.f,mo=soil&&isfinite(soil->moisture)?fminf(1.f,fmaxf(0.f,soil->moisture)):.35f;
    const float pk=soil&&isfinite(soil->packed)?fminf(1.f,fmaxf(0.f,soil->packed)):0.f,md=fmaxf(-1.f,fminf(1.f,(mo-.35f)/.3f));
    const float T=fminf(.10f,s->thickness*mf*sf);
    const float len=seg>=0.f?seg:fminf(1.f,fmaxf(0.f,v)*dt),hw=rear?.075f:.058f,patch=.05f,st=s->cell*.7f;
    /* damp: tall narrow lips; dry: low wide crumbly ones */
    const float lipw=.06f-.015f*md,lip_peak=.015f-.006f*md,level=.5f*pk;
    /* angle of repose: dry loose soil ~34 deg, damp holds ~50 deg */
    const float repose=.67f+.52f*fminf(1.f,fmaxf(0.f,(mo-.15f)/.4f));
    spin=fminf(1.f,fmaxf(0.f,isfinite(spin)?spin:0.f));skid=fminf(1.f,fmaxf(0.f,isfinite(skid)?skid:0.f));if(!isfinite(slip_lat))slip_lat=0.f;
    /* the tread presses the layer down by its load; a spinning tyre digs through it */
    float press=fminf(.88f,fmaxf(.45f,.45f+.30f*load/1200.f));if(spin>.15f)press=fmaxf(press,.75f+.25f*spin);
    const float floor_u=-T*press/MXD_SOFT_UNIT;const int cap=(int)fminf(127.f,T*(1.3f+.3f*md)/MXD_SOFT_UNIT);
    const uint8_t dir=(uint8_t)((int)lrintf(atan2f(f1,f0)/3.14159265f*128.f)&127);
    const int knobs=fresh&&mo>.12f&&spin<.15f&&skid<.3f&&fabsf(slip_lat)<1.5f; /* bone-dry dust keeps no knob pits */const uint8_t tag=(uint8_t)((knobs?0x80:0)|dir);
    /* share of the pushed soil going to the +n side: a sideways slide bulldozes it */
    float ln=0.f;if(lat){float ll=hypotf(lat[0],lat[1]);if(ll>1e-4f)ln=(lat[0]*n0+lat[1]*n1)/ll;}
    const float wr=.5f+.4f*fmaxf(-1.f,fminf(1.f,slip_lat*ln/2.f));
    const float kc=(material==5?.25f:.5f)+.15f*fmaxf(0.f,md); /* pressed soil that packs instead of moving (mud packs more) */
    const float roost=spin>.15f?fminf(.8f,1.2f*spin):0.f,push=skid>.15f?fminf(.6f,skid):0.f;
    s->serial++;if(!s->serial)s->serial=1;s->stamps++;
    /* trench walls stand at the repose angle, centred on the tread edge (no vertical texel steps) */
    const float wall=fmaxf(1.5f*s->cell,T*press/repose),ho=hw+.5f*wall,ha=ho+.5f*s->cell;
    const int na=(int)ceilf(2.f*ha/st),nl=(int)ceilf(lipw/st);float lw[16],lsum=0.f;
    for(int k=0;k<nl&&k<16;k++){float a=((float)k+.5f)*lipw/nl;lw[k]=a<lip_peak?a/lip_peak:(lipw-a)/(lipw-lip_peak);lsum+=lw[k];}
    float roost_u=0.f,push_u=0.f;
    for(float u=-(len+patch);u<=patch;u+=st){
        const float cx=px+f0*u,cz=pz+f1*u;float removed=0.f;
        for(int k=0;k<na;k++){
            /* the profile is taken at the texel's own centre, not the sample, so edges stay clean */
            float a=-ha+((float)k+.5f)*2.f*ha/na;int ta=(int)floorf((cx+n0*a)/s->cell),tb=(int)floorf((cz+n1*a)/s->cell);
            float tx=((float)ta+.5f)*s->cell-px,tz=((float)tb+.5f)*s->cell-pz,ac=fabsf(tx*n0+tz*n1);
            float cov=fminf(1.f,(ho-ac)/wall);if(cov<=0.f)continue;float r=fminf(1.f,ac/hw);
            uint8_t *p=mxd_soft_at(s,ta,tb);if(!p)continue;
            int cur=(int)p[0]-128,tgt=(int)lrintf(floor_u*(1.f-.35f*r*r*r*r)*cov); /* rounded tyre shoulders */
            if(cur>tgt){removed+=(float)(cur-tgt);p[0]=(uint8_t)(tgt+128);cur=tgt;}
            else if(level>0.f&&cur<tgt){int fill=(int)lrintf((float)(tgt-cur)*level); /* packed line: old trenches ironed flat */
                if(fill>0){cur+=fill;p[0]=(uint8_t)(cur+128);removed-=(float)fill;}}
            if(cur>=tgt-2)p[1]=tag; /* the tread touched it */
        }
        if(!(removed>0.f))continue;
        float R=removed*(1.f-kc),Rr=R*roost,Rp=(R-Rr)*push,L=R-Rr-Rp;roost_u+=Rr;push_u+=Rp;
        for(int k=0;k<nl&&k<16;k++){float a=ho+((float)k+.5f)*lipw/nl,w=lw[k]/lsum;
            mxd_soft_add(s,cx+n0*a,cz+n1*a,L*wr*w,cap);mxd_soft_add(s,cx-n0*a,cz-n1*a,L*(1.f-wr)*w,cap);}
    }
    /* braking: a mound shoved ahead of the tyre (pressed again on the next pass) */
    if(push_u>0.f){float tot=0.f;const float ml=.07f;
        for(float u=patch+st*.5f;u<=patch+ml;u+=st)for(int k=0;k<na;k++)tot+=1.f-(u-patch)/ml;
        if(tot>0.f)for(float u=patch+st*.5f;u<=patch+ml;u+=st){float w=(1.f-(u-patch)/ml)/tot;
            for(int k=0;k<na;k++){float a=(-hw+((float)k+.5f)*2.f*hw/na)*1.1f;mxd_soft_add(s,px+f0*u+n0*a,pz+f1*u+n1*a,push_u*w,cap);}}}
    /* lips and overlapping trenches settle to the repose angle */
    if(!s->defer_relax){const float e=ho+lipw+s->cell,x0=px-f0*(len+patch),z0=pz-f1*(len+patch),x1=px+f0*(patch+.08f),z1=pz+f1*(patch+.08f);
     mxd_soft_relax(s,(int)floorf((fminf(x0,x1)-e)/s->cell),(int)floorf((fminf(z0,z1)-e)/s->cell),(int)floorf((fmaxf(x0,x1)+e)/s->cell),(int)floorf((fmaxf(z0,z1)+e)/s->cell),
         (int)lrintf(repose*s->cell/MXD_SOFT_UNIT));}
    /* drive spin: crumbs thrown back over the ground behind the tyre */
    for(int c=0;c<32&&roost_u>2.f;c++){
        float q=4.f+8.f*mxd_soft_rand(s);if(q>roost_u)q=roost_u;roost_u-=q; /* 4..12 mm crumbs */
        float u=-(len+patch)-(.15f+1.1f*mxd_soft_rand(s))*fminf(1.f,.3f+v/8.f),a=(mxd_soft_rand(s)-.5f)*.35f;
        float x=px+f0*u+n0*a,z=pz+f1*u+n1*a;
        mxd_soft_add(s,x,z,q*.5f,cap);
        for(int j=0;j<4;j++){static const float o[4][2]={{1,0},{-1,0},{0,1},{0,-1}};mxd_soft_add(s,x+o[j][0]*s->cell,z+o[j][1]*s->cell,q*.125f,cap);}
    }
}
/* Loose soil the dirt sim drops on the ground (berms beside the line, soil shoved
   ahead into bump rows, roost spray behind a spinning tyre). It lands in the layer's
   trenches first and buries the prints there (no crisp knobs left); what is left lies
   as a thin fluffy cover of at most MXD_SOFT_COVER_UP above the layer top - the sim
   already raises the ridden ground by the same volume, so the layer must not stack it
   twice. Band as the sim's mxb_ss_deposit: centre X,Z (m from the terrain centre),
   direction d, from a to b m along d, half-width w, volume m^3, with the same tent
   profile. Bands bigger than cover_samples texels are sampled at random (each sample
   carries its share), so a 2 m row crest or a 1 m roost spray costs the same. */
#define MXD_SOFT_COVER_UP 6 /* units (mm) */
#ifndef MXD_SOFT_COVER_SHARE
#define MXD_SOFT_COVER_SHARE .5f /* of the dropped volume that lands in the layer (the rest is the sim's broad mound) */
#endif
static inline void mxd_soft_cover(MxdSoft *s,float X,float Z,const float d[2],float a,float b,float w,float volume){
    if(!s||!(s->thickness>0.f)||!(volume>0.f)||!isfinite(volume)||!isfinite(X)||!isfinite(Z)||!d)return;
    float dl=hypotf(d[0],d[1]);if(!(dl>1e-4f)||!(w>0.f))return;
    const float f0=d[0]/dl,f1=d[1]/dl,n0=-f1,n1=f0,px=X+.5f*s->span_x,pz=Z+.5f*s->span_z;
    const float mid=.5f*(a+b),half=.5f*fabsf(b-a)+s->cell,ww=w+s->cell;
    const float k=MXD_SOFT_COVER_SHARE*volume/(half*ww)/MXD_SOFT_UNIT; /* units at tent weight 1: the tent integrates to half*ww */
    const int nu=(int)ceilf(2.f*half/s->cell),nv=(int)ceilf(2.f*ww/s->cell),cells=nu*nv;
    const int maxs=s->defer_relax?16:384; /* pre-wear: 280k passes, coarser sampling (~+.8 s) */const int sampled=cells>maxs;const int n=sampled?maxs:cells;
    const float share=sampled?(float)cells/(float)n:1.f;
    s->serial++;if(!s->serial)s->serial=1;
    for(int i=0;i<n;i++){
        float u,v;
        if(sampled){u=mid-half+2.f*half*mxd_soft_rand(s);v=-ww+2.f*ww*mxd_soft_rand(s);}
        else{u=mid-half+((float)(i/nv)+.5f)*s->cell;v=-ww+((float)(i%nv)+.5f)*s->cell;}
        float wt=(1.f-fabsf(u-mid)/half)*(1.f-fabsf(v)/ww);if(wt<=0.f)continue;
        float add=k*wt*share;int q=(int)floorf(add+mxd_soft_rand(s));if(q<=0)continue;
        const int ta=(int)floorf((px+f0*u+n0*v)/s->cell),tb=(int)floorf((pz+f1*u+n1*v)/s->cell);
        {const uint8_t *r=mxd_soft_peek(s,ta,tb);if(!r||(r[0]==128&&r[1]==0))continue;} /* untouched layer stays clean (no crumbs sprinkled over it) */
        uint8_t *p=mxd_soft_at(s,ta,tb);if(!p)continue;
        int cur=(int)p[0]-128;
        if(cur<0){int fill=q<-cur?q:-cur;cur+=fill;q-=fill;if(fill>=2)p[1]&=0x7f;} /* buried: the knob pits are gone */
        if(q>0&&cur<MXD_SOFT_COVER_UP){cur+=q;if(cur>MXD_SOFT_COVER_UP)cur=MXD_SOFT_COVER_UP;p[1]&=0x7f;}
        p[0]=(uint8_t)(cur+128);
    }
}
/* Saved layer (version 2: 1 mm steps; version 1 files, .5 mm, are converted on load): "MXSL", version, tiles_x, tiles_z, tile, cell, span_x, span_z,
   thickness, count, then count x {uint32 tile index, tile bytes}. */
static inline int mxd_soft_write(const MxdSoft *s,FILE *f){
    if(!s||!f)return 0;uint32_t hdr[3]={s->tiles_x,s->tiles_z,MXD_SOFT_TILE},ver=2,count=s->allocated;float g[4]={s->cell,s->span_x,s->span_z,s->thickness};
    if(fwrite("MXSL",1,4,f)!=4||fwrite(&ver,4,1,f)!=1||fwrite(hdr,4,3,f)!=3||fwrite(g,4,4,f)!=4||fwrite(&count,4,1,f)!=1)return 0;
    size_t n=(size_t)s->tiles_x*s->tiles_z;uint32_t written=0;
    for(size_t t=0;t<n;t++)if(s->tile[t]){uint32_t i=(uint32_t)t;
        if(fwrite(&i,4,1,f)!=1||fwrite(s->tile[t],1,MXD_SOFT_TILE*MXD_SOFT_TILE*2,f)!=MXD_SOFT_TILE*MXD_SOFT_TILE*2)return 0;written++;}
    return written==count;
}
/* Into an empty layer of the same terrain; 1 loaded, 0 not this terrain / bad file (layer left empty). */
static inline int mxd_soft_read(MxdSoft *s,FILE *f){
    if(!s||!f||s->allocated)return 0;char m[4];uint32_t ver,hdr[3],count;float g[4];
    if(fread(m,1,4,f)!=4||memcmp(m,"MXSL",4)||fread(&ver,4,1,f)!=1||(ver!=1&&ver!=2)||fread(hdr,4,3,f)!=3||fread(g,4,4,f)!=4||fread(&count,4,1,f)!=1)return 0;
    if(hdr[0]!=s->tiles_x||hdr[1]!=s->tiles_z||hdr[2]!=MXD_SOFT_TILE||g[0]!=s->cell||fabsf(g[1]-s->span_x)>.01f||fabsf(g[2]-s->span_z)>.01f||count>MXD_SOFT_MAX_TILES)return 0;
    size_t n=(size_t)s->tiles_x*s->tiles_z;int ok=1;
    for(uint32_t k=0;k<count&&ok;k++){uint32_t i;uint8_t *p=NULL;
        if(fread(&i,4,1,f)!=1||i>=n||s->tile[i]||!(p=malloc(MXD_SOFT_TILE*MXD_SOFT_TILE*2))||
           fread(p,1,MXD_SOFT_TILE*MXD_SOFT_TILE*2,f)!=MXD_SOFT_TILE*MXD_SOFT_TILE*2){free(p);ok=0;break;}
        if(ver==1)for(int j=0;j<MXD_SOFT_TILE*MXD_SOFT_TILE;j++){int h=(int)p[2*j]-128;p[2*j]=(uint8_t)(128+(h>=0?(h+1)/2:-((1-h)/2)));} /* v1: .5 mm steps */
        s->tile[i]=p;s->allocated++;s->tile_serial[i]=1;}
    if(!ok){for(size_t t=0;t<n;t++){free(s->tile[t]);s->tile[t]=NULL;s->tile_serial[t]=0;}s->allocated=0;return 0;}
    s->serial=1;s->loaded=1;return 1;
}
#endif
