#ifndef MXB_SOIL_SIM_H
#define MXB_SOIL_SIM_H
/* Dirt Simulation v1: mass-conserving terramechanics on the deformation field.
   Replaces the GPU brush pass when enabled (CPU only, no GPU readback).

   Per wheel and step, from the tyre contact the host already measures:
   - Sinkage (Bekker pressure-sinkage, n=1, circular-chord contact length):
     z^1.5 = W / (2 b sqrt(2R) k(c)), k rising exponentially with compaction c,
     softened by a loose top layer; more sinkage with wheelspin/skid (slip
     sinkage). The tyre floor is measured from the soil it rolls INTO, so a
     rolling wheel cuts a rut once and repeated passes deepen it only as far as
     the soil still yields (compaction raises k: ruts stabilise like hardpack).
   - Cut volume splits into compaction (loose soil compacts most; raises c) and
     displaced soil. Displaced soil is conserved: bulldozed to the sides
     (berms; a sliding tyre pushes more to the outside), ahead of a skidding
     tyre (braking bumps/pile), behind a spinning tyre.
   - Wheelspin excavates and throws roost: spoil leaves the contact and lands
     0.4..5 m behind (by slip speed), spread like spray; a little is lost as
     dust. Churning loosens the soil it touches (lower c, deeper loose layer).
   - Every deposit is loose. Avalanching relaxes loose soil to its angle of
     repose (~34 deg) and lets compacted soil hold steeper walls; firm soil
     beyond its own limit collapses into loose soil. Mass is conserved.
   The grid is the host's encoded deformation field (relative to the base
   terrain), so tyre-scale effects are cell averages (sub-cell coverage
   weights): volume, not peak shape, is what the grid can carry. */
#include <math.h>
#ifndef MXB_LIP_CARRY
#define MXB_LIP_CARRY 1.f /* 1: grooves run through lips at their run-up depth (1.2: 0) */
#endif
#ifndef MXB_FACE_DEPTH
#define MXB_FACE_DEPTH .2f /* share of the jump-face protection applied to rut depth (1.2: 1) */
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MXB_SS_MAX_RECTS 48
typedef struct {
    float x,z;            /* contact point, native units, terrain-centred */
    float fwd[2],lat[2];  /* unit travel direction and lateral axis (x,z) */
    float speed;          /* wheel-centre ground speed along travel, m/s */
    float slip_long;      /* contact slide velocity along travel, m/s: + skid (braking), - spin (drive) */
    float slip_lat;       /* lateral slide velocity along lat, m/s */
    float load;           /* normal load, N */
    float width,radius;   /* tyre, m */
    float gain;           /* host modulation (soil feedback, bumps), ~1 */
    float dt;int valid;
    int material;         /* MX surface material under the tyre (4/7 soil, 10 soft, 11 compact, 5 sand, 12 gravel) */
    float floor_bias;     /* v7.4: m added to this pass's rut floor (washboard rows) */
    float bias_floor;     /* v7.5: deepest a row trough may sit below the smoothed ground, m (0 = .04) */
    float impact;         /* v7.9: 0..1 just touched down from the air - the hit packs soil instead of shoving it */
    float print_fade;     /* v8.2: 0 = full tyre print (the player), 1 = none (gate lines); pre-wear riders leave faint ones */
    float turn;           /* v8.7: 0..1 how hard the bike is turning - corners keep the deep v7.5 ruts the user likes */
    float brake;          /* v9: 0..1 braking / hard-drive share of the bump stress - those rows run widest across the band */
} MxbSoilSimWheel;
/* Per-surface soil behaviour, read from the track's own material layers via the
   tyre contact: sand ruts deep and wide, barely compacts, no cohesion and
   throws big roost; hardpack resists, compacts little and grows chop/braking
   bumps; soft soil gives more; gravel barely moves. Scales on the base INI. */
/* cmax: how far riding alone can pack each surface (v2: soft soil never turns
   into rock-hard pack; only real hardpack reaches full compaction). */
#ifndef MXB_SIM_BAND
#define MXB_SIM_BAND .7f /* rut band half-width floor (cells); 1.9: .7 (was 1) - ruts .70 m wide vs Pro .60, were .80 */
#endif
typedef struct {uint8_t id;float c0,k,compact,repose_loose,repose_firm,lateral,roost,bump,dust;int sand;float scour,cmax;} MxbSoilMaterial;
static inline MxbSoilMaterial mxb_soil_material(int mx,float base_c0){
    switch(mx){
    case 10:return (MxbSoilMaterial){2,fmaxf(.05f,base_c0-.08f),.75f,1.10f,1.00f,.90f,1.15f,1.2f,.9f,.8f,0,.010f,.78f};
    case 11:return (MxbSoilMaterial){3,fminf(.85f,base_c0+.25f),1.50f,.60f,1.05f,1.20f,.85f,.7f,1.8f,1.6f,0,.045f,1.f};
    case 5: return (MxbSoilMaterial){4,.06f,.60f,.20f,.93f,.45f,1.50f,1.7f,.7f,.3f,1,0.f,.55f};
    case 12:return (MxbSoilMaterial){5,.80f,2.50f,.30f,1.00f,.80f,.80f,.6f,1.2f,1.3f,0,.012f,.95f};
    default:return (MxbSoilMaterial){1,base_c0,1.f,1.f,1.f,1.f,1.f,1.f,1.f,1.f,0,.015f,.88f};
    }
}
typedef struct {
    float k_loose,k_packed;   /* N/m^3 equivalent Bekker modulus at compaction 0 and 1 */
    float compaction0;        /* untouched track */
    float compact_share;      /* fraction of cut that compacts at c=0 (falls to 0 at c=1) */
    float compact_depth;      /* m of soil the compaction acts over */
    float repose_loose,repose_firm; /* tan of angle of repose */
    float roost_gain;         /* m of spoil per m of spin slip (times tyre width) */
    float churn;              /* compaction lost per m of spin slip */
    float dust;               /* fraction of roost lost as dust */
    float depth_scale;        /* overall scale on sinkage */
    float moisture;           /* 0 dry/dusty .. .35 ideal moist .. 1 wet/muddy */
    float breakdown;          /* scale on sliding tyres shearing packed crust into loose soil (0 off) */
    float drying;             /* scale on the track drying out over a ride (0 = stays as watered) */
    float sweep;              /* scale on tyres sweeping loose soil off a ridden line (0 off) */
    float capture;            /* v6: scale on tyres dropping into an existing groove (0 off) */
    float hardpan;            /* v6: m below the built surface where the ripped layer ends and the hard base starts */
    float band;               /* v7: rut band half-width floor, in cells */
    float gain_lo,gain_hi;    /* v7.4: range of the bump/chop sinkage modulation (live .75..1.3) */
    float cohesion;           /* v6: scale on damp soil holding steep rut walls (0 off) */
    float clods;              /* v6: scale on roost landing as scattered clods (0 off) */
} MxbSoilSimParams;
typedef struct {
    uint32_t size,tiles_x,tiles_y;
    float *h;                 /* encoded deformation field (published) */
    float *loose;             /* loose layer depth, m */
    float *base;              /* optional track surface height, m (NULL = flat): loose soil slides on real slopes */
    uint8_t *comp;            /* compaction: 0 = untouched (compaction0), else 1..255 -> 0..1 */
    uint8_t *mat;             /* surface class last worked here (MxbSoilMaterial.id), 0 = untouched */
    int8_t *moist_off;        /* per-cell moisture offset on the track-wide value, 1/255 units:
                                 + freshly cut (damp soil exposed), - broken up / thrown loose (dries) */
    uint8_t *clods;           /* v6: roost clods lying on the cell, 0..255 (~1 clod per 48); tyres crush them */
    /* v7.1 tyre prints (display only): the last tyre line through each texel -
       print: bit7 rear tyre, bits0..6 strength; dir: travel angle mod pi in 1/256
       turns of pi; off: signed lateral offset of the tyre centre line from the
       texel centre, cell/100 units. The shader draws tread where ground is intact. */
    uint8_t *print,*print_dir;int8_t *print_off;
    float texel_off;          /* texel centre offset: .5 (blended publication) or 1 (vertex-exact: texel q sits on vertex q+1) */
    uint32_t rng;
    double time_s;            /* ridden time, s (drives drying) */
    unsigned char *tiles;     /* tiles changed this step (128x32 texels) */
    float ref,per_unit;       /* metres = (h-ref)*per_unit */
    float span_x,span_z;      /* metres */
    float cell_x,cell_z,cell_area;
    float native_m;           /* metres per native unit */
    MxbSoilSimParams p;
    int bx0,bz0,bx1,bz1,box;  /* dirty box this step (union of everything) */
    /* Per-wheel-pass dirty rectangles this step. Relaxation and the soil export
       walk only these: with riders spread around the track the union box above
       covered most of the track and cost ~20 ms a frame. */
    int cur_on,cx0,cz0,cx1,cz1,rect_n;int rect[MXB_SS_MAX_RECTS][4];
    float last_x[2],last_z[2],last_floor[2];int has_last[2]; /* per wheel: where its floor was set */
    double volume_m3,compacted_m3,roost_m3,dust_m3,cut_m3; uint64_t steps,relax_moves;
    /* optional display hook: every rolling tyre pass where the tyre really is
       (metres from the terrain centre, ground speed, drive spin / brake skid 0..1) */
    void (*on_contact)(void *user,unsigned wheel,const MxbSoilSimWheel *w,float X,float Z,float v,float spin,float skid);
    void *on_contact_user;
} MxbSoilSim;
static inline MxbSoilSimParams mxb_soil_sim_defaults(void){
    MxbSoilSimParams p;
    /* Calibrated for a ~1000 N rear tyre (b=.15 m, R=.33 m). v2 range: freshly
       ripped loam (c~.35) sinks ~5 cm, a ridden-in line (c~.7) ~1.6 cm, full
       hardpack (c=1) ~4 mm. v1's k_packed=1.5e8 left ridden lines ~1 mm: lines
       turned to rock after a few laps while untouched dirt stayed 6 cm soft. */
    /* v7 (matched to Maryland Pro vs Ams): compact_share .50 -> .20 - half of every
       cut vanishing as packing made lines sink like digging; real worn tracks
       push the soil into ridges between ruts (median change ~-2 cm, not -10). */
    p.k_loose=2.3e4f;p.k_packed=2.0e7f;p.compaction0=.35f;p.compact_share=.20f;p.compact_depth=.08f;
    /* firm walls stand to ~50 deg (v5: 60 deg kept sharp rut walls and dug lips standing) */
    p.repose_loose=.67f;p.repose_firm=1.19f;p.roost_gain=.011f;p.churn=.06f;p.dust=.05f;p.depth_scale=1.f;p.moisture=.35f;
    p.breakdown=1.f;p.drying=1.f;p.sweep=1.f;p.capture=1.f;p.cohesion=1.f;p.clods=1.f;p.hardpan=.20f;p.band=MXB_SIM_BAND;p.gain_lo=.75f;p.gain_hi=1.3f; /* v7: Pro ruts top out ~22 cm and run ~.55 m wide */
    return p;
}
static inline void mxb_soil_sim_destroy(MxbSoilSim *s){if(s){free(s->h);free(s->loose);free(s->comp);free(s->mat);free(s->moist_off);free(s->clods);free(s->print);free(s->print_dir);free(s->print_off);free(s->tiles);free(s->base);free(s);}}
/* size: encoded texels per side (multiple of 128); span: metres covered;
   per_unit: metres per encoded unit; ref: neutral encoded value. */
static inline MxbSoilSim *mxb_soil_sim_create(uint32_t size,float span_x,float span_z,float per_unit,float ref,float native_m){
    if(size<128||size%128||size>8192||!(span_x>0)||!(span_z>0)||!(per_unit>0)||!isfinite(ref)||!(native_m>0))return NULL;
    MxbSoilSim *s=calloc(1,sizeof(*s));if(!s)return NULL;
    size_t n=(size_t)size*size;s->size=size;s->tiles_x=size/128;s->tiles_y=size/32;
    s->h=malloc(n*sizeof(float));s->loose=calloc(n,sizeof(float));s->comp=calloc(n,1);s->mat=calloc(n,1);s->moist_off=calloc(n,1);s->clods=calloc(n,1);s->print=calloc(n,1);s->print_dir=calloc(n,1);s->print_off=calloc(n,1);s->tiles=calloc((size_t)s->tiles_x*s->tiles_y,1);
    if(!s->h||!s->loose||!s->comp||!s->mat||!s->moist_off||!s->clods||!s->print||!s->print_dir||!s->print_off||!s->tiles){mxb_soil_sim_destroy(s);return NULL;}
    for(size_t k=0;k<n;k++)s->h[k]=ref;
    s->texel_off=.5f;s->rng=0x9e3779b9u;
    s->ref=ref;s->per_unit=per_unit;s->span_x=span_x;s->span_z=span_z;s->native_m=native_m;
    s->cell_x=span_x/size;s->cell_z=span_z/size;s->cell_area=s->cell_x*s->cell_z;s->p=mxb_soil_sim_defaults();
    return s;
}
static inline float mxb_ss_c(const MxbSoilSim *s,size_t k){return s->comp[k]?(float)s->comp[k]/255.f:s->p.compaction0;}
static inline void mxb_ss_setc(MxbSoilSim *s,size_t k,float c){c=fminf(1.f,fmaxf(0.f,c));int v=(int)lrintf(c*255.f);s->comp[k]=(uint8_t)(v<1?1:v);}
static inline void mxb_ss_mark(MxbSoilSim *s,int col,int row){
    s->tiles[(row/32)*s->tiles_x+(col/128)]=1;
    if(!s->box){s->bx0=s->bx1=col;s->bz0=s->bz1=row;s->box=1;}
    else{if(col<s->bx0)s->bx0=col;if(col>s->bx1)s->bx1=col;if(row<s->bz0)s->bz0=row;if(row>s->bz1)s->bz1=row;}
    if(s->cur_on==1){s->cx0=s->cx1=col;s->cz0=s->cz1=row;s->cur_on=2;}
    else if(s->cur_on==2){if(col<s->cx0)s->cx0=col;if(col>s->cx1)s->cx1=col;if(row<s->cz0)s->cz0=row;if(row>s->cz1)s->cz1=row;}
}
/* Close the current wheel pass: its changed cells become one dirty rectangle
   (merged into the nearest one when the list is full). */
static inline void mxb_ss_rect_push(MxbSoilSim *s){
    if(s->cur_on!=2){s->cur_on=0;return;}s->cur_on=0;
    int r[4]={s->cx0,s->cz0,s->cx1,s->cz1};
    for(int i=0;i<s->rect_n;i++){int *q=s->rect[i]; /* overlapping or touching: grow it */
        if(r[0]<=q[2]+8&&r[2]>=q[0]-8&&r[1]<=q[3]+8&&r[3]>=q[1]-8){
            if(r[0]<q[0])q[0]=r[0];if(r[1]<q[1])q[1]=r[1];if(r[2]>q[2])q[2]=r[2];if(r[3]>q[3])q[3]=r[3];return;}}
    if(s->rect_n<MXB_SS_MAX_RECTS){memcpy(s->rect[s->rect_n++],r,sizeof(r));return;}
    int best=0;long bd=-1;for(int i=0;i<s->rect_n;i++){long dx=(s->rect[i][0]+s->rect[i][2]-r[0]-r[2]),dz=(s->rect[i][1]+s->rect[i][3]-r[1]-r[3]);long d=dx*dx+dz*dz;if(bd<0||d<bd){bd=d;best=i;}}
    int *q=s->rect[best];if(r[0]<q[0])q[0]=r[0];if(r[1]<q[1])q[1]=r[1];if(r[2]>q[2])q[2]=r[2];if(r[3]>q[3])q[3]=r[3];
}
/* moisture 0..1 at cell k: the track-wide value plus what was done to that cell */
static inline float mxb_ss_moist(const MxbSoilSim *s,size_t k){return fminf(1.f,fmaxf(0.f,s->p.moisture+(float)s->moist_off[k]/255.f));}
static inline void mxb_ss_moist_add(MxbSoilSim *s,size_t k,int d){int v=s->moist_off[k]+d;s->moist_off[k]=(int8_t)(v<-90?-90:v>26?26:v);} /* v8.6: exposed subsoil is only a little damper (+.10), not +.24: every cut used to add more, the line went black and ~30 % softer, so the next pass sank deeper - pits fed themselves */
/* metres of deformation at cell k */
static inline float mxb_ss_m(const MxbSoilSim *s,size_t k){return (s->h[k]-s->ref)*s->per_unit;}
static inline void mxb_ss_add(MxbSoilSim *s,int col,int row,float metres,int loose){
    if(col<0||row<0||col>=(int)s->size||row>=(int)s->size||metres==0.f)return;
    size_t k=(size_t)row*s->size+col;s->h[k]+=metres/s->per_unit;
    if(loose&&metres>0.f)s->loose[k]+=metres;
    if(metres<0.f){s->loose[k]=fmaxf(0.f,s->loose[k]+metres);}
    s->volume_m3+=(double)metres*s->cell_area;mxb_ss_mark(s,col,row);
}
/* world metres -> fractional texel (centres at integer) */
static inline void mxb_ss_texel(const MxbSoilSim *s,float X,float Z,float *col,float *row){
    *col=(X/s->span_x+.5f)*s->size-s->texel_off;*row=(.5f-Z/s->span_z)*s->size-s->texel_off;
}
static inline void mxb_ss_world(const MxbSoilSim *s,int col,int row,float *X,float *Z){
    *X=(((float)col+s->texel_off)/s->size-.5f)*s->span_x;*Z=(.5f-((float)row+s->texel_off)/s->size)*s->span_z;
}
/* deformation in metres at a world point, bilinear between texel centres */
static inline float mxb_ss_sample_m(const MxbSoilSim *s,float X,float Z){
    float c,r;mxb_ss_texel(s,X,Z,&c,&r);
    int i=(int)floorf(c),j=(int)floorf(r);if(i<0||j<0||i>=(int)s->size-1||j>=(int)s->size-1)return 0.f;
    float fc=c-(float)i,fr=r-(float)j;size_t k=(size_t)j*s->size+i;
    float a=mxb_ss_m(s,k),b=mxb_ss_m(s,k+1),d=mxb_ss_m(s,k+s->size),e=mxb_ss_m(s,k+s->size+1);
    return (a+(b-a)*fc)*(1.f-fr)+(d+(e-d)*fc)*fr;
}
static inline float mxb_ss_rand(MxbSoilSim *s){uint32_t x=s->rng;x^=x<<13;x^=x>>17;x^=x<<5;s->rng=x?x:0x9e3779b9u;return (float)(x&0xffffff)/16777216.f;}
/* Deposit volume (m^3) along a band: centre X,Z, direction d (unit), from a to b
   metres along d, half-width w, spread over cells with a smooth profile. */
static inline void mxb_ss_deposit(MxbSoilSim *s,float X,float Z,const float d[2],float a,float b,float w,float volume){
    if(!(volume>0.f))return;
    float n[2]={-d[1],d[0]},ext=fmaxf(fabsf(a),fabsf(b))+w+s->cell_x*2;
    float c0,r0;mxb_ss_texel(s,X,Z,&c0,&r0);
    int span_c=(int)ceilf(ext/s->cell_x)+1,span_r=(int)ceilf(ext/s->cell_z)+1;
    float weights[2048];int cols[2048],rows[2048];int count=0;double total=0;
    float mid=.5f*(a+b),half=.5f*fabsf(b-a)+.5f*s->cell_x;
    for(int r=(int)r0-span_r;r<=(int)r0+span_r;r++)for(int c=(int)c0-span_c;c<=(int)c0+span_c;c++){
        if(c<0||r<0||c>=(int)s->size||r>=(int)s->size||count>=2048)continue;
        float cx,cz;mxb_ss_world(s,c,r,&cx,&cz);float px=cx-X,pz=cz-Z;
        float u=px*d[0]+pz*d[1],v=px*n[0]+pz*n[1];
        float fu=1.f-fabsf(u-mid)/(half+s->cell_x*.5f),fv=1.f-fabsf(v)/(w+s->cell_x*.5f);
        if(fu<=0.f||fv<=0.f)continue;float wt=fu*fv;
        weights[count]=wt;cols[count]=c;rows[count]=r;count++;total+=wt;
    }
    if(!count||!(total>0)){int c=(int)lrintf(c0),r=(int)lrintf(r0);mxb_ss_add(s,c,r,volume/s->cell_area,1);return;}
    for(int k=0;k<count;k++)mxb_ss_add(s,cols[k],rows[k],(float)(volume*weights[k]/total)/s->cell_area,1);
}
/* One wheel, one step (body; mxb_soil_sim_wheel below records its dirty rectangle). */
static inline void mxb_soil_sim_wheel_pass(MxbSoilSim *s,unsigned wheel,const MxbSoilSimWheel *w){
    if(wheel>1)return;
    if(!w->valid){s->has_last[wheel]=0;return;}
    if(!(w->dt>0.f)||w->dt>.1f||!(w->load>0.f)||!isfinite(w->x)||!isfinite(w->z))return;
    const MxbSoilSimParams *p=&s->p;
    float X=w->x*s->native_m,Z=w->z*s->native_m;
    float v=fmaxf(0.f,w->speed),slip=w->slip_long,surface=v-slip; /* tyre surface speed */
    float spin=surface>v+.05f?(surface-v)/fmaxf(surface,.5f):0.f;      /* drive slip 0..1 */
    float skid=slip>.05f&&v>.3f?fminf(1.f,slip/fmaxf(v,.5f)):0.f;        /* braking slip 0..1 */
    float spin_speed=fmaxf(0.f,-slip);
    if(v<.25f&&spin_speed<.3f){s->has_last[wheel]=0;return;} /* parked: no rolling cut, no spin */
    /* v7.4: sinkage load capped at ~2.2x a rider's static rear load - hard landings
       pack the ground rather than digging craters (Pro landings: ~-1 cm, ours -8 cm) */
    float b=fmaxf(.05f,w->width),R=fmaxf(.15f,w->radius),W=fminf(w->load,2600.f);
    if(s->on_contact)s->on_contact(s->on_contact_user,wheel,w,X,Z,v,spin,skid);
    /* v7.1 tyre print where the tyre really is (before rut capture moves the cut):
       the texels within .75 cell of its centre line, over the ground covered
       since the last step, remember that line. */
    {float pf0=w->fwd[0],pf1=w->fwd[1],pl=hypotf(pf0,pf1);int pst=(int)lrintf(127.f*(1.f-fminf(1.f,fmaxf(0.f,w->print_fade))));
     if(pl>1e-4f&&pst>0){pf0/=pl;pf1/=pl;float pn0=-pf1,pn1=pf0,half_len=.5f*v*w->dt+.6f*s->cell_x,pc,pr;mxb_ss_texel(s,X,Z,&pc,&pr);
      int ci=(int)lrintf(pc),ri=(int)lrintf(pr),span=(int)ceilf(half_len/s->cell_x)+1;
      uint8_t dir=(uint8_t)((int)lrintf(atan2f(pf1,pf0)/3.14159265f*256.f)&255),tag=(uint8_t)((wheel?128:0)|pst);
      for(int r=ri-span;r<=ri+span;r++)for(int q=ci-span;q<=ci+span;q++){
          if(q<0||r<0||q>=(int)s->size||r>=(int)s->size)continue;
          float cx,cz;mxb_ss_world(s,q,r,&cx,&cz);float dx=cx-X,dz=cz-Z,u=dx*pf0+dz*pf1,vv=dx*pn0+dz*pn1;
          if(fabsf(u)>half_len||fabsf(vv)>.75f*s->cell_x)continue;
          size_t kk=(size_t)r*s->size+q;int o=(int)lrintf(-vv/s->cell_x*100.f); /* line = texel centre + off along n */
          s->print[kk]=tag;s->print_dir[kk]=dir;s->print_off[kk]=(int8_t)(o<-127?-127:o>127?127:o);}}}
    /* v6 rut capture: a tyre running on the wall of (or just beside) an existing
       groove drops into it, so riders on roughly the same line deepen ONE narrow
       rut instead of widening a trench - how real rut fields form: a few
       parallel slots ~40-60 cm apart, each line reinforced lap after lap. Looks
       across the tyre +-.2 m for the lowest ground and moves the cut there. */
    if(p->capture>0.f){
        float ll=hypotf(w->lat[0],w->lat[1]);
        if(ll>1e-4f){
            float la0=w->lat[0]/ll,la1=w->lat[1]/ll,h0=mxb_ss_sample_m(s,X,Z),best=0.f,best_off=0.f;
            for(int i=-4;i<=4;i++){if(!i)continue;float off=.05f*(float)i; /* +-20 cm: the groove's own rim */
                float drop=h0-mxb_ss_sample_m(s,X+la0*off,Z+la1*off)-.05f*fabsf(off); /* nearer groove wins a tie */
                if(drop>best){best=drop;best_off=off;}}
            /* v6.1: the shift is capped at half the rut band, so the cut still covers
               the real tyre position. An uncapped pull moved the whole cut off a
               ridge the tyre was riding on: the ridge was never flattened, berms
               from both grooves piled onto it and grew into spikes that flung riders. */
            float pull=fminf(1.f,p->capture*fmaxf(0.f,(best-.012f)/.04f)),lim=.5f*fmaxf(.5f*b,.85f*s->cell_x);
            float shift=best_off*pull;shift=fmaxf(-lim,fminf(lim,shift));
            X+=la0*shift;Z+=la1*shift;
        }
    }
    /* soil under the tyre: average compaction and loose depth over a cell window */
    float cc,rr;mxb_ss_texel(s,X,Z,&cc,&rr);int c0=(int)lrintf(cc),r0=(int)lrintf(rr);
    if(c0<1||r0<1||c0>=(int)s->size-1||r0>=(int)s->size-1)return;
    MxbSoilMaterial M=mxb_soil_material(w->material,p->compaction0);
    size_t k0=(size_t)r0*s->size+c0;float c=s->comp[k0]?mxb_ss_c(s,k0):M.c0,L=s->loose[k0];
    float soft=fminf(1.f,L/.05f);float ce=c*(1.f-.55f*soft);
    float k=p->k_loose*powf(p->k_packed/p->k_loose,ce);
    /* Moisture (per cell): wetter soil is softer (lower bearing capacity); dry soil stiffer. */
    const float mo=mxb_ss_moist(s,k0);
    k*=M.k*(M.sand?expf(1.0f*(mo-.35f)):expf(-1.6f*(mo-.35f))); /* wet sand is FIRMER */
    /* Bekker sinkage with circular chord contact length, n=1 */
    float z=powf(W/(2.f*b*sqrtf(2.f*R)*k),2.f/3.f);
    /* Slip sinkage (Lyasko): z_slip = z0 (1+i)/(1-0.5 i) for drive slip i; a
       locked/skidding tyre ploughs less than a spinning one. */
    float gain=1.f+(w->gain-1.f)*M.bump; /* braking-bump/soil modulation strength by surface */
    /* bump/chop modulation .75..1.3 (v5: .5..1.5 let bumps feed on themselves within minutes) */
    z*=((1.f+spin)/(1.f-.5f*spin))*(1.f+.4f*skid)*p->depth_scale*fmaxf(p->gain_lo,fminf(p->gain_hi,gain));
    /* Scour: hard wheel-hop impacts while braking/driving abrade firm ground even
       where the tyre barely sinks - why hardpack braking zones get beaten into
       bumps. gain-1 carries the load swing (and soil wear) from the host. */
    z+=M.scour*fmaxf(0.f,gain-1.f)*p->depth_scale;
    /* One pass can cut at most 12 cm (v5: 25 cm let a single hard landing or
       bump hit carve a step the next wheel skipped off). */
    z=fminf(z,.12f);
    /* v7.5: a wheel just back on the ground (landing, or a fresh replay) rolls through
       its touchdown spot - cutting its full sinkage there left a crater and a spoil
       ring at every landing point */
    if(!s->has_last[wheel])z*=.3f+.5f*w->impact;
    /* Rut band at least ~1.7 cells wide: MX samples heights at cell corners, so
       with blended publication a one-cell groove is averaged with its own
       berms. Vertex-exact publication keeps the same band but cuts it with a
       rounded floor (below): a ~3-cell smooth U, the narrowest groove the grid
       can show without stair-stepping on diagonals (a ~1-cell band did). */
    float l=2.f*sqrtf(2.f*R*z),A=fmaxf(.06f,.5f*l)+.5f*v*w->dt,B=fmaxf(.5f*b,p->band*s->cell_x);
    /* floor from the soil the tyre rolls into (front edge, a little ahead) */
    float fx=X+w->fwd[0]*(A+.5f*s->cell_x),fz=Z+w->fwd[1]*(A+.5f*s->cell_x),fc,fr;mxb_ss_texel(s,fx,fz,&fc,&fr);
    int ic=(int)lrintf(fc),ir=(int)lrintf(fr);if(ic<0||ir<0||ic>=(int)s->size||ir>=(int)s->size)return;
    float front=mxb_ss_m(s,(size_t)ir*s->size+ic),here=mxb_ss_m(s,k0);
    /* Jump faces: on a steep built upslope (track shape, not ruts) the rear wheel
       spinning up the face must not dig a kicker or shove a pile onto the lip;
       that turned faces into launch ramps (v5 ride: an unprovoked backflip).
       face 0 = flat .. 1 = steeper than ~35 deg up the direction of travel. */
    /* v9 row drive: braking and hard acceleration hit rows hardest, but rollers, landings and
       plain rolling build them too (user: not all of Pro's rows come from braking) */
    const float rowd=fmaxf(.4f*(1.f-fminf(1.f,fmaxf(0.f,w->turn))),w->brake); /* not in turns: corners keep the v7.5 ruts and berms */
    float face=0.f,uphill=0.f,lipz=0.f; /* lipz: 0..1 within ~7 m of a real lip */
    if(s->base){ /* steeper of the 0.6 m slopes just behind and just ahead of the contact, so
                    the lip and the foot of a face count as face (a one-cell difference flickers 0..1;
                    a centred 1.2 m slope eased off at the lip, which then trenched into a kicker) */
        float ac,ar,bc,br;mxb_ss_texel(s,X+.6f*w->fwd[0],Z+.6f*w->fwd[1],&ac,&ar);mxb_ss_texel(s,X-.6f*w->fwd[0],Z-.6f*w->fwd[1],&bc,&br);
        int ai=(int)lrintf(ac),aj=(int)lrintf(ar),bi=(int)lrintf(bc),bj=(int)lrintf(br);
        if(ai>=0&&aj>=0&&bi>=0&&bj>=0&&ai<(int)s->size&&aj<(int)s->size&&bi<(int)s->size&&bj<(int)s->size){
            float here_b=s->base[k0];
            float rise=fmaxf(s->base[(size_t)aj*s->size+ai]-here_b,here_b-s->base[(size_t)bj*s->size+bi]);
            float t=(rise/.6f-.36f)/.34f;face=t<0.f?0.f:t>1.f?1.f:t*t*(3.f-2.f*t);
            uphill=fminf(1.f,fmaxf(0.f,(rise/.6f-fminf(.30f,.18f+.3f*w->brake))/.15f)); /* v9: rows stop on slopes over ~18 %, braking ones up to 30 % (Pro's braking rows run up climbs into corners); jump faces stay clean */
            /* v7.9 take-off faces: the last ~10 m up to a lip (ground falls away >.5 m within
               3 m past it) stays crew-smooth on Pro (measured: faces 9 m before take-off clean
               while ours wore 4-11 cm). Found from the track shape alone, so live riding and
               every track get it. */
            /* v8.5: a lip has a climb into it - the ground 3 m back is lower than here. Without
               this every steep downhill (ground dropping ahead) read as a lip and lost its bumps,
               which is exactly where Pro has its biggest moguls (downhill braking zones). */
            float climb=0.f;{float bc0,br0;mxb_ss_texel(s,X-3.f*w->fwd[0],Z-3.f*w->fwd[1],&bc0,&br0);int i0=(int)lrintf(bc0),j0=(int)lrintf(br0);
                if(i0>=0&&j0>=0&&i0<(int)s->size&&j0<(int)s->size)climb=here_b-s->base[(size_t)j0*s->size+i0];}
            if(climb>-.15f){float lip=0.f;int dfirst=99; /* v8.9: flat run-ups to drop-offs count too; only an ongoing descent (downhill braking) is excluded */
                for(int dd=0;dd<=11&&lip<1.f;dd++){float bc,brr,cc2,cr2;
                    mxb_ss_texel(s,X+w->fwd[0]*(float)dd,Z+w->fwd[1]*(float)dd,&bc,&brr);mxb_ss_texel(s,X+w->fwd[0]*(float)(dd+3),Z+w->fwd[1]*(float)(dd+3),&cc2,&cr2);
                    int i1=(int)lrintf(bc),j1=(int)lrintf(brr),i2=(int)lrintf(cc2),j2=(int)lrintf(cr2);
                    if(i1<0||j1<0||i2<0||j2<0||i1>=(int)s->size||j1>=(int)s->size||i2>=(int)s->size||j2>=(int)s->size)break;
                    float drop=s->base[(size_t)j1*s->size+i1]-s->base[(size_t)j2*s->size+i2];
                    if(drop>.5f){lip=fminf(1.f,(drop-.5f)/.4f);if(dfirst==99)dfirst=dd;}}
                /* v8.4: the face itself may take light ruts (.5f), but the last ~2-3 m to the lip
                   (and the lip top, where the slope flattens) stay crew-smooth, the protection easing
                   in over ~7 m: a groove that ends abruptly short of the lip is a kicker too */
                float st=fminf(1.f,fmaxf(0.f,(rise/.6f-.10f)/.15f));face=fmaxf(face,.5f*lip*st);uphill=fmaxf(uphill,lip); /* v8.9: no bump rows / shoved crests anywhere on the ~11 m run-up to a real lip (they made sharp-edged mogul blocks on the approach; Pro is clean there) */
                {float lipnear=fminf(1.f,fmaxf(0.f,1.f-((float)dfirst-1.f)/6.f)); /* 1 within ~2 m of the lip, 0 by ~7 m: face ruts fade out, no groove ends at a step */
                 face=fmaxf(face,.95f*lip*lipnear);uphill=fmaxf(uphill,lip*lipnear);lipz=lip*lipnear;}}}}
    /* Faces are track-crew packed and get a fresh, compacting pass every lap: they
       cut ~30 % as deep (halving alone also halved packing, so faces stayed soft
       and still trenched as deep as flat ground in tests). */
    /* v8.4: faces and lips are crew-packed: fresh (soft) soil there sinks like packed dirt */
    /* v10 (1.3): ruts route through faces up to the lip as on Pro (faces rut ~6.6 cm there, deeper
       than the run-up): the old fade toward the lip left a hump = rollers right before every take-off.
       face (kicker guard) still keeps shove, roost digging and rows off the face. */
    const float faced=MXB_FACE_DEPTH*face;
    z*=1.f-.7f*fminf(1.f,faced)*(1.f-ce);
    z*=1.f+1.f*fminf(3.f,fmaxf(0.f,w->turn));
    /* v8.9: on a lip run-up (uphill = 1) the bump pulse in the dig strength is flattened too:
       it alone built sharp-edged mogul blocks right before take-offs */
    {float gc=fmaxf(p->gain_lo,fminf(p->gain_hi,gain));if(gc>0.f)z*=(1.f+(gc-1.f)*(1.f-uphill))/gc;} /* v8.7: corner ruts cut like v7.5 (deeper, defined) */
    float surface_m=fmaxf(front,here);
    /* v6 hardpan: tracks are ripped ~20-50 cm deep over a hard base, so ruts
       stop deepening there (sand deeper, hardpack shallower). Sinkage tapers
       over the last 10 cm instead of stopping dead. */
    const float pan=p->hardpan*(M.sand?1.7f:M.id==2?1.1f:M.id==3?.6f:M.id==5?.4f:1.f);
    if(pan>0.f){z*=fminf(1.f,fmaxf(.05f,(surface_m+pan)/.10f));}
    /* v8.1 rut saturation: a groove much deeper than the ground beside it (+-1.2 m) has a
       packed bottom and walls that carry the bike - it stops deepening (one line ridden for
       a long session otherwise sank toward the hardpan while Pro's grooves stay ~4-6 cm).
       Rows (floor_bias / row troughs) are not affected. */
    {float sx=-w->fwd[1]*1.2f,sz=w->fwd[0]*1.2f;
        float side=.5f*(mxb_ss_sample_m(s,X+sx,Z+sz)+mxb_ss_sample_m(s,X-sx,Z-sz)),deep=side-mxb_ss_m(s,k0);
        {float sat=-.03f*w->brake+.065f+(.025f-.065f)*fminf(1.f,1.5f*faced)+.04f*fminf(3.f,fmaxf(0.f,w->turn));z*=fminf(1.f,fmaxf(.1f,1.f-(deep-sat)/.06f));}}
    float floor_m=surface_m-z*(1.f-.7f*faced);
    /* v7.5 washboard rows: the floor follows the wheel's load swing (floor_bias), but a
       trough may never go more than 4 cm below the smoothed local ground (+-1.6 m along
       travel) - unbounded, every pass cut the same trough deeper and a bike feeding its
       own bounce back into it would run away. */
    float row_ref=surface_m,row_mean=surface_m;
    const float fbias=.6f*w->floor_bias;
    if(fbias!=0.f){
        float ref=0.f;for(int k=-2;k<=2;k++)ref+=mxb_ss_sample_m(s,X+w->fwd[0]*.8f*(float)k,Z+w->fwd[1]*.8f*(float)k);ref*=.2f;
        floor_m=fmaxf(floor_m+fbias*(1.f-face)*(1.f-MXB_LIP_CARRY*lipz)*(1.f+1.5f*(1.f-ce)),ref-z*(1.f-.7f*faced)-(w->bias_floor>0.f?w->bias_floor:.04f));
        row_ref=ref-(w->bias_floor>0.f?w->bias_floor:.04f);row_mean=ref;
    }
    /* v10 (1.3) routed lips: up the last ~7 m to a lip the bike compresses at the foot of the face
       (deeper cut) and unloads over the lip (shallower cut), so each pass left the lip standing
       ~9 cm above its own groove - a kicker (Pro: ~3 cm). The groove floor may not rise along
       travel there: the rut runs on through the lip, the way bikes route lips out. */
    if(MXB_LIP_CARRY>0.f&&lipz>.3f&&s->has_last[wheel])floor_m=fminf(floor_m,s->last_floor[wheel]+.005f);
    if(pan>0.f&&floor_m<-pan)floor_m=fminf(surface_m,-pan);
#ifdef MXB_SOIL_SIM_DEBUG
    MXB_SOIL_SIM_DEBUG(X,z,face,surface_m,floor_m);
#endif
    /* A tyre that has not rolled onto new soil keeps its floor: no endless
       sinking in place (digging in place comes only from wheelspin below). */
    if(s->has_last[wheel]){float dx=X-s->last_x[wheel],dz=Z-s->last_z[wheel];
        if(dx*dx+dz*dz<.25f*A*A)floor_m=fmaxf(floor_m,s->last_floor[wheel]);
        else{s->last_x[wheel]=X;s->last_z[wheel]=Z;s->last_floor[wheel]=floor_m;}}
    else{s->last_x[wheel]=X;s->last_z[wheel]=Z;s->last_floor[wheel]=floor_m;s->has_last[wheel]=1;}
    /* cut the footprint down to the floor (coverage weighted) */
    double cut=0,compact=0;float n[2]={-w->fwd[1],w->fwd[0]};
    /* v6 (vertex-exact): a rounded, tyre-shaped floor across the band instead of
       a flat box. Repeated passes drive every touched cell to its floor, so a
       box floor turned diagonal ruts into a one-cell staircase; with the
       rounded floor an edge cell only ever reaches a partial depth and ruts
       stay smooth at any angle to the grid. */
    const int rounded=s->texel_off>.75f;const float half_w=B+.5f*s->cell_x;
    int sc=(int)ceilf((A+B)/s->cell_x)+1,sr=(int)ceilf((A+B)/s->cell_z)+1;
    for(int r=r0-sr;r<=r0+sr;r++)for(int q=c0-sc;q<=c0+sc;q++){
        if(q<0||r<0||q>=(int)s->size||r>=(int)s->size)continue;
        float cx,cz;mxb_ss_world(s,q,r,&cx,&cz);float px=cx-X,pz=cz-Z;
        float u=px*w->fwd[0]+pz*w->fwd[1],vv=px*n[0]+pz*n[1];
        float cu=fminf(1.f,fmaxf(0.f,(A+.5f*s->cell_x-fabsf(u))/s->cell_x));
        float cv=fminf(1.f,fmaxf(0.f,(B+.5f*s->cell_x-fabsf(vv))/s->cell_x));
        float cov=cu*cv;if(cov<=0.f)continue;
        size_t kk=(size_t)r*s->size+q;
        if(s->clods[kk])s->clods[kk]=(uint8_t)((float)s->clods[kk]*(1.f-.8f*cov)); /* tyre crushes clods (display only: no dirty mark) */
        float fl=floor_m,wc=cov;
        if(rounded){float t=fabsf(vv)/half_w;if(t>=1.f)continue;fl=surface_m-(surface_m-floor_m)*(1.f-t*t);wc=cu*fminf(1.f,4.f*(1.f-t));}
        float hm=mxb_ss_m(s,kk),d=(hm-fl)*wc;if(d<=0.f)continue;
        float cq=s->comp[kk]?mxb_ss_c(s,kk):M.c0,Lq=s->loose[kk];
        float share=1.f*p->compact_share*M.compact*(1.f-cq)*(Lq>.002f?1.f:.7f);s->mat[kk]=M.id;
        /* v7.9 landings pack the ground down (Pro sits ~2 cm lower under every jump and
           where riders land short): most of a touchdown's cut is compaction */
        if(w->impact>0.f)share=fmaxf(share,.9f*w->impact*M.compact*(1.f-.5f*cq));
        /* Proctor-like: compaction is most effective near optimum moisture */
        share*=fmaxf(.45f,1.f-2.2f*fabsf(mxb_ss_moist(s,kk)-.38f));
        mxb_ss_add(s,q,r,-d,0);
        /* cutting a rut exposes the damper soil underneath (dark rut bottoms) */
        if(d>.015f)mxb_ss_moist_add(s,kk,(int)(d*600.f));
        cut+=(double)d*s->cell_area;compact+=(double)d*share*s->cell_area;
        float cn=cq+d*share/p->compact_depth;if(cn>M.cmax)cn=fmaxf(cq,M.cmax);
        mxb_ss_setc(s,kk,cn);
    }
    /* v7.9 rows across the track: a bike slamming into a row trough works a patch wider
       than its tyre (the hit, the shoved soil, riders a little either side of the line),
       so the trough reaches across neighbouring ruts. Neighbouring lines then bounce on
       the same trough and the rows lock in phase across the whole band, as on Pro, instead
       of every rut growing its own pockets between unbroken rut walls. */
    double rowcut=0;float rb=B+.6f+1.4f*rowd;
    if(fbias<0.f&&uphill<1.f){
        /* v8.4: soft (fresh/ripped) dirt bumps up faster than hardpack, as its grooves do */
        float depth=1.5f*(-fbias)*(1.f-uphill)*(1.f+1.5f*(1.f-ce));
        const float RL=fmaxf(A,.9f); /* v8.5: a bump trough is the wheel's whole compression stroke, not one tyre print:
           cut over +-RL along travel with a cosine taper (same volume as before), so troughs are smooth
           dips between broad humps (Pro moguls ~9 m2) instead of tyre-sized pits */
        depth*=2.f*A/RL;int rc=(int)ceilf((RL+rb)/s->cell_x)+1;
        for(int r=r0-rc;r<=r0+rc;r++)for(int q=c0-rc;q<=c0+rc;q++){
            if(q<0||r<0||q>=(int)s->size||r>=(int)s->size)continue;
            float cx,cz;mxb_ss_world(s,q,r,&cx,&cz);float px=cx-X,pz=cz-Z;
            float u=px*w->fwd[0]+pz*w->fwd[1],av=fabsf(px*n[0]+pz*n[1]);
            float cu=fabsf(u)>=RL?0.f:.5f*(1.f+cosf(3.14159265f*fabsf(u)/RL));if(cu<=0.f||av>=rb)continue;
            float wv=av<B?.5f:.5f*(1.f+cosf(3.14159265f*(av-B)/(rb-B))); /* v9: smooth taper over the whole row width (the deposit is that wide too) */size_t kk=(size_t)r*s->size+q;float hm=mxb_ss_m(s,kk);
            float lim=fmaxf(row_ref,pan>0.f?-pan:-1e9f),d=fminf(depth*wv*cu,hm-lim);if(d<=0.f)continue;
            mxb_ss_add(s,q,r,-d,0);rowcut+=(double)d*s->cell_area;}
    }
    /* Breakdown: a sliding tyre (braking skid, wheelspin, sideways slide)
       shears packed crust into a loose top layer instead of packing it harder,
       so lines never turn to rock: braking zones and take-off lips get chewed
       up, grow loose "marbles" over hardpack, and the next passes rut and bump
       them further. Converts packing to looseness; no height change. */
    float slide=(fabsf(w->slip_long)+.6f*fabsf(w->slip_lat))*w->dt;
    if(p->breakdown>0.f&&slide>.002f){
        for(int r=r0-1;r<=r0+1;r++)for(int q=c0-1;q<=c0+1;q++){
            size_t kk=(size_t)r*s->size+q;float cq=mxb_ss_c(s,kk);if(cq<=.30f)continue;
            float wgt=(q==c0&&r==r0)?1.f:.35f;
            float dc=fminf(cq-.30f,2.5f*p->breakdown*M.bump*slide*(cq-.30f)*wgt*(1.f-.8f*face));
            if(!(dc>0.f))continue;
            mxb_ss_setc(s,kk,cq-dc);s->loose[kk]=fminf(.06f,s->loose[kk]+dc*p->compact_depth*.6f);mxb_ss_mark(s,q,r);
            mxb_ss_moist_add(s,kk,-(int)ceilf(dc*40.f)); /* broken-up crust is aerated: dries faster */
        }
    }
    /* Sweep: a rolling tyre pushes loose marbles off its line, so ridden lines
       go slick and hard with loose soil heaped beside them instead of staying
       buried. Moves only loose depth (height and loose layer drop together). */
    double swept=0;
    if(p->sweep>0.f&&v>1.f){
        float frac=fminf(.6f,.6f*p->sweep*fminf(1.f,v*w->dt/(2.f*A)));
        for(int r=r0-sr;r<=r0+sr;r++)for(int q=c0-sc;q<=c0+sc;q++){
            if(q<0||r<0||q>=(int)s->size||r>=(int)s->size)continue;
            size_t kk=(size_t)r*s->size+q;float Lq=s->loose[kk];if(Lq<.002f)continue;
            float cx,cz;mxb_ss_world(s,q,r,&cx,&cz);float px=cx-X,pz=cz-Z;
            float u=px*w->fwd[0]+pz*w->fwd[1],vv=px*n[0]+pz*n[1];
            float cov=fminf(1.f,fmaxf(0.f,(A+.5f*s->cell_x-fabsf(u))/s->cell_x))*fminf(1.f,fmaxf(0.f,(B+.5f*s->cell_x-fabsf(vv))/s->cell_x));
            if(cov<=0.f)continue;float take=Lq*frac*cov;
            mxb_ss_add(s,q,r,-take,0);swept+=(double)take*s->cell_area;
        }
    }
    /* compaction removes volume (density rises); the rest is displaced */
    s->cut_m3+=cut;s->compacted_m3+=compact;s->volume_m3+=compact;
    double moved=cut-compact;
    if(moved>0||swept>0||rowcut>0){
        float side=fmaxf(-1.f,fminf(1.f,w->slip_lat/3.f*M.lateral));
        /* on a face nothing is shoved up onto the lip or dumped just behind: it goes to the sides */
        float front_share=fminf(.6f,.12f+.6f*skid)*(1.f-face),back_share=fminf(.4f,.05f+.4f*spin)*(1.f-face);
        /* v7.8 braking-bump crests: a tyre slamming into a row trough (floor_bias < 0) shoves
           the soil it cuts forward onto the next crest instead of out to the berms - rows
           grow up as well as down (Pro hot spots run 30-38 cm crest to trough) */
        float shove=fbias<0.f?fminf(.7f,-fbias/.03f*.7f)*(1.f-uphill):0.f; /* none up faces: no kickers */
        if(front_share+back_share+shove>.9f)shove=fmaxf(0.f,.9f-front_share-back_share);
        /* where the shoved soil would land: never up the foot of a face (a bike compressing
           into a transition pushed a pile onto it every pass - 0.6 m in pre-wear, 1 m live) and
           never onto a crest already standing 20 cm proud of its surroundings or 20 cm above the
           original ground (tyres knock the top off; a broad pile lifts its own surroundings). The rest
           goes to the sides like ordinary spoil. */
        float room=1.f;
        if(shove>0.f||rowcut>0){float dx=w->fwd[0]*(A+.5f*(.8f+2.8f)),dz=w->fwd[1]*(A+.5f*(.8f+2.8f));
            float ha=mxb_ss_sample_m(s,X+dx,Z+dz); /* m above the original ground */
            {const float f0=(A+.8f)/(A+1.8f),f1=(A+2.8f)/(A+1.8f);ha=fmaxf(ha,fmaxf(mxb_ss_sample_m(s,X+f0*dx,Z+f0*dz),mxb_ss_sample_m(s,X+f1*dx,Z+f1*dz)));
             if(rb>B+.7f){const float lx=n[0]*.6f*rb,lz=n[1]*.6f*rb;ha=fmaxf(ha,fmaxf(mxb_ss_sample_m(s,X+dx+lx,Z+dz+lz),mxb_ss_sample_m(s,X+dx-lx,Z+dz-lz)));}} /* v9: highest point of the deposit patch (ends and, for wide braking rows, sides) - a crest anywhere under it stacked on (piles to ~1 m) */
            room=fminf(1.f,fmaxf(0.f,fminf(1.f-(ha-row_mean-.15f)/.10f,1.f-(ha-.15f)/.10f)));
            if(s->base){float c1,r1,c2,r2;mxb_ss_texel(s,X,Z,&c1,&r1);mxb_ss_texel(s,X+w->fwd[0]*(A+1.4f),Z+w->fwd[1]*(A+1.4f),&c2,&r2);
                int i1=(int)lrintf(c1),j1=(int)lrintf(r1),i2=(int)lrintf(c2),j2=(int)lrintf(r2);
                if(i1>=0&&j1>=0&&i2>=0&&j2>=0&&i1<(int)s->size&&j1<(int)s->size&&i2<(int)s->size&&j2<(int)s->size){
                    float up=(s->base[(size_t)j2*s->size+i2]-s->base[(size_t)j1*s->size+i1])/(A+1.4f);room*=1.f-fminf(1.f,fmaxf(0.f,(up-fminf(.30f,.18f+.3f*w->brake))/.15f));}}}
        shove*=room;
        float lat_share=1.f-front_share-back_share-shove;
        float off=B+.9f*s->cell_x,wd=fmaxf(.12f,.9f*s->cell_x)*sqrtf(M.lateral); /* berm ~2 cells wide: survives corner sampling */
        /* sides along the tyre's own lateral axis, the axis slip_lat is measured on:
           a tyre sliding toward +lat bulldozes soil onto the +lat side */
        float ll=hypotf(w->lat[0],w->lat[1]);float la[2]={ll>1e-4f?w->lat[0]/ll:n[0],ll>1e-4f?w->lat[1]/ll:n[1]};
        float plus[2]={la[0],la[1]},minus[2]={-la[0],-la[1]};
        /* berms: bands just outside both tyre edges, outside of a slide gets more */
        float wl=.5f*(1.f+.8f*side),wr=1.f-wl;
        if(moved<0)moved=0;
        mxb_ss_deposit(s,X+plus[0]*off,Z+plus[1]*off,w->fwd,-A,A,wd,(float)((moved*lat_share+swept)*wl));
        mxb_ss_deposit(s,X+minus[0]*off,Z+minus[1]*off,w->fwd,-A,A,wd,(float)((moved*lat_share+swept)*wr));
        mxb_ss_deposit(s,X,Z,w->fwd,A,A+.22f,B+.04f,(float)(moved*front_share));
        mxb_ss_deposit(s,X,Z,w->fwd,-A-.25f,-A,B+.04f,(float)(moved*back_share));
        if(shove>0.f)mxb_ss_deposit(s,X,Z,w->fwd,A+.8f,A+2.8f,B+.04f+.5f,(float)(moved*shove));
        if(rowcut>0){mxb_ss_deposit(s,X,Z,w->fwd,A+.8f,A+2.8f,rb,(float)(rowcut*room));
            if(room<1.f)mxb_ss_deposit(s,X,Z,w->fwd,-fmaxf(A,.9f),fmaxf(A,.9f),rb,(float)(rowcut*(1.f-room)));} /* no room ahead: the trough is not dug (soil stays, spread back over the whole trough - over the tyre print alone it stacked a ridge, up to ~1 m in tests) */
    }
    /* wheelspin: excavate under the tyre and throw it back as roost */
    if(spin>0.f&&spin_speed>.3f){
        double want=(double)p->roost_gain*M.roost*spin_speed*w->dt*b*(.5+spin)*p->depth_scale; /* m^3 */
        /* Acceleration chop: a hopping rear wheel under power digs hardest as it
           lands, so excavation pulses with the same load swing as braking bumps
           and exits get beaten into evenly spaced chop lap after lap. */
        want*=fmaxf(.6f,fminf(1.4f,1.f+1.0f*(w->gain-1.f)*M.bump));
        want*=1.f-fminf(1.f,1.3f*face); /* spinning up a face (or at the lip) must not dig a kicker into it */
        double got=0;
        for(int r=r0-1;r<=r0+1;r++)for(int q=c0-1;q<=c0+1;q++){
            size_t kk=(size_t)r*s->size+q;float cq=mxb_ss_c(s,kk);
            float wgt=(q==c0&&r==r0)?.5f:.0625f;float d=(float)(want*wgt)/s->cell_area;
            d*=1.f+(s->loose[kk]>.005f?.5f:0.f)-.4f*cq;
            if(pan>0.f)d=fminf(d,fmaxf(0.f,mxb_ss_m(s,kk)+pan)); /* the hard base does not roost */
            if(d<=0.f)continue;
            mxb_ss_add(s,q,r,-d,0);got+=(double)d*s->cell_area;
            mxb_ss_setc(s,kk,cq-p->churn*spin_speed*w->dt);   /* churned: looser */
            s->loose[kk]+=.3f*d;                               /* torn surface */
        }
        s->cut_m3+=got;double dust=got*p->dust*M.dust*fmaxf(0.f,1.f-mo/.6f)*2.f; /* dry: dusty, wet: none */s->dust_m3+=dust;s->volume_m3+=dust;
        float throw_m=fminf(5.f,fmaxf(.4f,.35f*spin_speed))*(1.f-.45f*fmaxf(0.f,mo-.5f)/.5f); /* heavy wet clods fly short */
        /* Spoil leaves along the contact patch's slip over the ground: backward
           under power, and toward the outside when the tyre also slides, so
           powerslides out of a turn stack roost into the outside berm. */
        float ll2=hypotf(w->lat[0],w->lat[1]),lx=ll2>1e-4f?w->lat[0]/ll2:0.f,lz=ll2>1e-4f?w->lat[1]/ll2:0.f;
        float ex=-w->fwd[0]*spin_speed+lx*w->slip_lat,ez=-w->fwd[1]*spin_speed+lz*w->slip_lat,el=hypotf(ex,ez);
        float back[2]={el>1e-4f?ex/el:-w->fwd[0],el>1e-4f?ez/el:-w->fwd[1]};
        mxb_ss_deposit(s,X,Z,back,.6f*throw_m,1.4f*throw_m,.30f+.10f*throw_m,(float)(got-dust)); /* spray width grows with throw */
        s->roost_m3+=got-dust;
        /* v6 roost clods: part of the spoil lands as lumps (damp soil throws more
           and bigger lumps, dry soil mostly crumbs and dust). Lumps leave the
           tyre near its surface speed, so they fly well past the spoil band and
           fan out to the sides - real roost lies scattered beside and behind the
           line, not in it where the next tyre crushes it at once. Display only:
           the volume is already in the deposit above. */
        if(p->clods>0.f){
            float lumps=(float)((got-dust)/1.2e-4)*p->clods*(.35f+1.3f*fminf(1.f,mo/.5f));
            int n=(int)lumps;if(mxb_ss_rand(s)<lumps-(float)n)n++;if(n>16)n=16;
            float side[2]={-back[1],back[0]},reach=fminf(7.f,fmaxf(1.f,.55f*spin_speed+.12f*v))*(1.f-.35f*fmaxf(0.f,mo-.5f)/.5f);
            for(int i=0;i<n;i++){
                float t=reach*(.35f+.9f*mxb_ss_rand(s)),o=(.25f+.35f*t)*(2.f*mxb_ss_rand(s)-1.f);
                float lc,lr;mxb_ss_texel(s,X+back[0]*t+side[0]*o,Z+back[1]*t+side[1]*o,&lc,&lr);
                int q=(int)lrintf(lc),r=(int)lrintf(lr);if(q<0||r<0||q>=(int)s->size||r>=(int)s->size)continue;
                /* no dirty mark: clods change no height, and marking a landing up to
                   7 m away grew this pass's relaxation rectangle ~10x (v7 profile) */
                size_t kk=(size_t)r*s->size+q;int cv2=s->clods[kk]+48;s->clods[kk]=(uint8_t)(cv2>255?255:cv2);
            }
        }
    }
}
/* Avalanche relaxation inside each dirty rectangle (+margin). Loose soil holds
   repose_loose; compacted soil up to repose_firm; beyond, firm collapses. */
static inline void mxb_soil_sim_relax_region(MxbSoilSim *s,int sweeps,int x0,int x1,int z0,int z1,
    float loose_rep,float firm_rep,const float rep_loose_id[6],const float rep_firm_id[6],float coh){
    if(x0<0)x0=0;if(z0<0)z0=0;if(x1>(int)s->size-1)x1=s->size-1;if(z1>(int)s->size-1)z1=s->size-1;
    for(int it=0;it<sweeps;it++)for(int r=z0;r<=z1;r++)for(int q=x0;q<=x1;q++){
        size_t k=(size_t)r*s->size+q;
        /* v6.1: diagonal neighbours too - with only the two axis directions a
           one-cell berm crest running diagonally never slumped and stood as a
           sharp 20 cm wall between ruts */
        for(int dir=0;dir<4;dir++){
            int q2=q+(dir==0||dir==2)-(dir==3),r2=r+(dir!=0);if(q2>x1||q2<x0||r2>z1)continue;
            size_t k2=(size_t)r2*s->size+q2;float dist=dir==0?s->cell_x:dir==1?s->cell_z:hypotf(s->cell_x,s->cell_z);
            float a=mxb_ss_m(s,k),bb=mxb_ss_m(s,k2),dd=a-bb;
            /* Loose soil feels the whole surface (track shape + deformation);
               firm collapse only the deformation, so the track's built shape
               (jump faces, designed berms) is never eroded. */
            float tot=dd+(s->base?s->base[k]-s->base[k2]:0.f);
            /* nothing to move unless some slope beats the loosest angle anywhere */
            if(fabsf(dd)<=.5f*loose_rep*dist&&fabsf(tot)<=.5f*loose_rep*dist)continue;
            /* each surface slumps at its own angles (sand: almost no cohesion) */
            unsigned idf=s->mat[dd>0?k:k2]%6u,idl=s->mat[tot>0?k:k2]%6u;
            float lrep_f=loose_rep*rep_loose_id[idf],lrep_l=loose_rep*rep_loose_id[idl];
            float firm=(lrep_f+(firm_rep*rep_firm_id[idf]-lrep_f)*mxb_ss_c(s,dd>0?k:k2))*coh;if(firm<lrep_f)firm=lrep_f;
            /* v6.1: soil piled ABOVE the built surface is spoil - it never stands
               steeper than loose repose, so no pile can grow into a spike. Firm and
               cohesive walls are only the cut walls of ruts. */
            if((dd>0?a:bb)>.01f)firm=lrep_f;
            /* v6.1: neither can a lone column: a cell standing above all four
               neighbours (left between cuts that reached the hardpan) is no wall,
               it slumps like loose soil. Cohesion holds one-sided rut walls only. */
            else{int pq=dd>0?q:q2,pr=dd>0?r:r2;float ph=dd>0?a:bb;
                if(pq>0&&pr>0&&pq<(int)s->size-1&&pr<(int)s->size-1){size_t pk=(size_t)pr*s->size+pq;
                    if(ph-.01f>mxb_ss_m(s,pk-1)&&ph-.01f>mxb_ss_m(s,pk+1)&&ph-.01f>mxb_ss_m(s,pk-s->size)&&ph-.01f>mxb_ss_m(s,pk+s->size))firm=lrep_f;}}
            float excess_firm=fabsf(dd)-firm*dist,move=0.f;size_t hi;int hq,hr,lq,lr;
            if(excess_firm>0.f){
                hi=dd>0?k:k2;hq=dd>0?q:q2;hr=dd>0?r:r2;lq=dd>0?q2:q;lr=dd>0?r2:r;
                move=.25f*excess_firm+fminf(s->loose[hi],.25f*fmaxf(0.f,fabsf(dd)-lrep_f*dist-excess_firm));
            }else{
                hi=tot>0?k:k2;hq=tot>0?q:q2;hr=tot>0?r:r2;lq=tot>0?q2:q;lr=tot>0?r2:r;
                float excess_loose=fabsf(tot)-lrep_l*dist;
                if(excess_loose>0.f)move=fminf(s->loose[hi],.25f*excess_loose);
            }
            if(move<=1e-5f)continue;
            mxb_ss_add(s,hq,hr,-move,0);mxb_ss_add(s,lq,lr,move,1);
            s->relax_moves++;
        }
        /* v6.1 spike guard: whatever built it, no cell may stand more than 6 cm
           above the mean of its four neighbours - the excess spreads to them
           (mass kept). A one-cell spike in a rut flung riders or stopped them. */
        if(q>0&&r>0&&q<(int)s->size-1&&r<(int)s->size-1){
            float h=mxb_ss_m(s,k),nb[4]={mxb_ss_m(s,k-1),mxb_ss_m(s,k+1),mxb_ss_m(s,k-s->size),mxb_ss_m(s,k+s->size)};
            float ex=h-.25f*(nb[0]+nb[1]+nb[2]+nb[3])-.06f;
            if(ex>0.f){float m=.5f*ex;mxb_ss_add(s,q,r,-m,0);
                mxb_ss_add(s,q-1,r,.25f*m,1);mxb_ss_add(s,q+1,r,.25f*m,1);mxb_ss_add(s,q,r-1,.25f*m,1);mxb_ss_add(s,q,r+1,.25f*m,1);s->relax_moves++;}
        }
    }
}
static inline void mxb_soil_sim_relax(MxbSoilSim *s,int sweeps){
    if(!s->rect_n)return;
    /* Moisture: dry loose soil slumps at ~34 deg, moist soil is cohesive (walls
       hold), mud flows. */
    /* v6.1: moist loose soil to ~38 deg, not 42 (loose crests stood too sharp) */
    float mo=s->p.moisture,loose_rep=s->p.repose_loose*(mo<.6f?(1.f+.18f*fminf(1.f,mo/.35f)):(1.18f-.93f*(mo-.6f)/.4f));
    float firm_rep=s->p.repose_firm*(mo<.7f?1.f:1.f-.5f*(mo-.7f)/.3f);
    /* v6 cohesion: damp soil holds near-vertical cut walls (narrow, sharp ruts),
       dry soil crumbles and mud slumps; peaks around .3-.55 moisture
       (x1.38 at the default .35: ~55 deg walls in untouched dirt). */
    float coh=1.f+.5f*s->p.cohesion*fmaxf(0.f,1.f-fabsf(mo-.42f)/.30f);
    /* per-surface repose factors once per step instead of per cell pair */
    static const int mx_of_id[6]={7,7,10,11,5,12};float rl[6],rf[6];
    for(int i=0;i<6;i++){MxbSoilMaterial M=mxb_soil_material(mx_of_id[i],s->p.compaction0);rl[i]=M.repose_loose;rf[i]=M.repose_firm;}
    const int m=4;
    for(int i=0;i<s->rect_n;i++){const int *q=s->rect[i];
        mxb_soil_sim_relax_region(s,sweeps,q[0]-m,q[2]+m,q[1]-m,q[3]+m,loose_rep,firm_rep,rl,rf,coh);}
}
/* One wheel, one step: the pass plus its dirty rectangle. */
static inline void mxb_soil_sim_wheel(MxbSoilSim *s,unsigned wheel,const MxbSoilSimWheel *w){
    s->cur_on=1;mxb_soil_sim_wheel_pass(s,wheel,w);mxb_ss_rect_push(s);
}
/* Ridden time advances the weather: a watered track dries out over a ride
   (~0.12 moisture per 20 min at drying=1), down to a dusty floor. Riding
   time only (outer physics dt), never paused or menu time. */
static inline void mxb_soil_sim_advance(MxbSoilSim *s,float dt){
    if(!s||!(dt>0.f)||dt>.5f)return;s->time_s+=dt;
    s->p.moisture=fmaxf(.08f,s->p.moisture-s->p.drying*1.0e-4f*dt);
}
static inline void mxb_soil_sim_begin(MxbSoilSim *s){memset(s->tiles,0,(size_t)s->tiles_x*s->tiles_y);s->box=0;s->rect_n=0;s->cur_on=0;}
static inline void mxb_soil_sim_end(MxbSoilSim *s){mxb_soil_sim_relax(s,2);s->steps++;}
#endif
