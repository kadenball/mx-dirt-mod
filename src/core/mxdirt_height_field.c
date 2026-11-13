#include "mxdirt_height_field.h"
#include "mxdirt_height_bridge.h"
#include <stdlib.h>
#include <string.h>
/* Absolute DFM limits that keep a target inside the encoded [0,1] range of the
   default (response 1) height scale bc. Collision uses floor(DFM/65536);
   preserve its whole fixed-point bin, intersecting with the per-cell signed
   terrain bounds downstream. */
static int visual_bounds(float reference,float bc,double native_metres,double vertical_scale,
        int32_t *lower,int32_t *upper) {
    if(!lower||!upper||!isfinite(reference)||reference<0.f||reference>1.f||
       !isfinite(bc)||bc<=0.f||!isfinite(native_metres)||native_metres<=0.||
       !isfinite(vertical_scale)||vertical_scale<=0.)return 0;
    double factor=(vertical_scale/65535.)/(2.*bc*native_metres);
    if(!isfinite(factor)||factor<=0.)return 0;
    double lo=ceil(((double)reference-1.)/factor)*65536.;
    double hi=(floor((double)reference/factor)+1.)*65536.-1.;
    if(!isfinite(lo)||!isfinite(hi)||lo>INT32_MAX||hi<INT32_MIN||lo>hi)return 0;
    *lower=(int32_t)(lo<INT32_MIN?INT32_MIN:lo);
    *upper=(int32_t)(hi>INT32_MAX?INT32_MAX:hi);return 1;
}
typedef struct {float low,high,fraction;int valid;} Axis;
struct MxdirtHeightField {
    MxdirtHeightConfig c;
    size_t cells;
    int16_t *base;
    int32_t *initial,*staged,*cached;
    uint32_t *begin,*end,*committed_begin,*committed_end;
    int has_cache;
    int32_t *last_target; /* target that already mirrors cached (see convert_changed_tiles) */
    float depth_response;
    int32_t visual_lower,visual_upper;
    Axis *x,*z;
    /* Fine incremental conversion (perf): the source as of the last conversion, so a
       dirty 128x32 tile is narrowed to the texels that really changed, and for each
       source column / row the destination vertices that read it. Without them
       (allocation failed) whole tiles are converted as before. */
    float *shadow;uint32_t *xfirst,*xend,*zfirst,*zend;int fine;
};
/* Which destination vertices read each source column (x) and row (z). */
static void build_lookup(MxdirtHeightField *p){
    p->fine=0;if(!p->shadow||!p->xfirst||!p->xend||!p->zfirst||!p->zend)return;
    for(int axis=0;axis<2;axis++){
        uint32_t n=axis?p->c.destination_height:p->c.destination_width,m=axis?p->c.source_height:p->c.source_width;
        const Axis *a=axis?p->z:p->x;uint32_t *first=axis?p->zfirst:p->xfirst,*end=axis?p->zend:p->xend;
        for(uint32_t c=0;c<m;c++){first[c]=UINT32_MAX;end[c]=0;}
        for(uint32_t k=0;k<n;k++){
            if(!a[k].valid)continue;
            if(!(a[k].low>=0.f)||!(a[k].high>=a[k].low)||!(a[k].high<(float)m))return;
            for(uint32_t c=(uint32_t)a[k].low;c<=(uint32_t)a[k].high;c++){if(k<first[c])first[c]=k;if(k+1>end[c])end[c]=k+1;}
        }
    }
    p->fine=1;
}
void mxdirt_height_field_destroy(MxdirtHeightField *p){
    if(p){free(p->base);free(p->initial);free(p->staged);free(p->cached);free(p->begin);free(p->end);free(p->committed_begin);free(p->committed_end);free(p->x);free(p->z);
        free(p->shadow);free(p->xfirst);free(p->xend);free(p->zfirst);free(p->zend);free(p);}
}
int mxdirt_height_field_create(MxdirtHeightField **out,const MxdirtHeightConfig *c,
    const int16_t *base,const int32_t *initial,size_t count){
    if(!out)return 0;*out=NULL;
    if(!c||!base||!initial||!c->generation||c->source_width<4||c->source_height<4||
       c->source_width>8192||c->source_height>8192||c->source_stride<c->source_width||c->source_stride>8192||
       (uint64_t)c->source_stride*c->source_height>16777216||
       c->destination_width<2||c->destination_height<2||c->destination_width>8193||c->destination_height>8193||
       (uint64_t)c->destination_width*c->destination_height>34000000||
       !isfinite(c->native_span_x)||!isfinite(c->native_span_z)||c->native_span_x<=0||c->native_span_z<=0||
       c->native_span_x>1.e20f||c->native_span_z>1.e20f||
       !isfinite(c->native_height_scale)||c->native_height_scale<0||c->native_height_scale>1.e4f||
       !isfinite(c->reference_encoded)||c->reference_encoded<0||c->reference_encoded>1||
       !isfinite(c->native_metres)||c->native_metres<1.e-6||c->native_metres>1.e6||
       !isfinite(c->mxb_vertical_scale)||c->mxb_vertical_scale<1.e-6||c->mxb_vertical_scale>1.e6)return 0;
    size_t cells=(size_t)c->destination_width*c->destination_height;
    if(count<cells)return 0;
    for(size_t k=0;k<cells;k++){
        int32_t lo,hi;mxb_height_fixed_bounds(base[k],&lo,&hi);
        if(initial[k]<lo||initial[k]>hi)return 0;
    }
    MxdirtHeightField *p=calloc(1,sizeof(*p));if(!p)return 0;
    p->c=*c;p->cells=cells;p->depth_response=1.f;p->visual_lower=INT32_MIN;p->visual_upper=INT32_MAX;
    p->base=malloc(cells*sizeof(*base));p->initial=malloc(cells*sizeof(*initial));
    p->staged=malloc(cells*sizeof(*p->staged));
    p->cached=malloc(cells*sizeof(*p->cached));
    p->begin=malloc(c->destination_height*sizeof(*p->begin));p->end=malloc(c->destination_height*sizeof(*p->end));
    p->committed_begin=malloc(c->destination_height*sizeof(*p->committed_begin));
    p->committed_end=malloc(c->destination_height*sizeof(*p->committed_end));
    p->x=malloc(c->destination_width*sizeof(Axis));p->z=malloc(c->destination_height*sizeof(Axis));
    if(!p->base||!p->initial||!p->staged||!p->cached||!p->begin||!p->end||!p->committed_begin||!p->committed_end||!p->x||!p->z){mxdirt_height_field_destroy(p);return 0;}
    memcpy(p->base,base,cells*sizeof(*base));memcpy(p->initial,initial,cells*sizeof(*initial));
    for(int axis=0;axis<2;axis++){
        uint32_t n=axis?c->destination_height:c->destination_width;
        float span=axis?c->native_span_z:c->native_span_x;Axis *a=axis?p->z:p->x;
        for(uint32_t k=0;k<n;k++){
            float world=((float)k/(float)(n-1))*span-span*.5f;
            MxdirtHeightFootprint f;
            int valid=mxdirt_height_prepare(axis?0.f:world,axis?world:0.f,c->native_span_x,c->native_span_z,
                                           c->source_width,c->source_height,&f);
            a[k]=(Axis){f.corners[axis],f.corners[axis+2],f.fractions[axis],valid};
        }
    }
    /* optional: a failed allocation only means whole-tile conversion */
    p->shadow=malloc((size_t)c->source_stride*c->source_height*sizeof(float));
    p->xfirst=malloc(c->source_width*sizeof(uint32_t));p->xend=malloc(c->source_width*sizeof(uint32_t));
    p->zfirst=malloc(c->source_height*sizeof(uint32_t));p->zend=malloc(c->source_height*sizeof(uint32_t));
    build_lookup(p);
    *out=p;return 1;
}
float mxdirt_height_field_depth_response(const MxdirtHeightField *p){return p?p->depth_response:0.f;}
const int16_t *mxdirt_height_field_base(const MxdirtHeightField *p){return p?p->base:NULL;}
int mxdirt_height_field_set_depth_response(MxdirtHeightField *p,float response){
    if(!p||!isfinite(response)||response<1.f||response>1.75f||
       (p->has_cache&&memcmp(p->cached,p->initial,p->cells*sizeof(*p->initial))))return 0;
    int32_t lo=INT32_MIN,hi=INT32_MAX;
    if(response!=1.f){
        if(!visual_bounds(p->c.reference_encoded,p->c.native_height_scale,
           p->c.native_metres,p->c.mxb_vertical_scale,&lo,&hi))return 0;
        for(size_t k=0;k<p->cells;k++)if(p->initial[k]<lo||p->initial[k]>hi)return 0;
    }
    p->depth_response=response;p->visual_lower=lo;p->visual_upper=hi;return 1;
}
/* Vertex-exact publication: each destination vertex takes ONE source texel (the
   lower lookup corner) instead of blending 2x2. The lookup's fraction
   drifts from 0 at the edges to .5 mid-track, so blending smeared every rut
   over two cells there. With 2048 texels and 2049 vertices the lower corner is
   texel k-1 for vertex k (x) and texel 2047-k for row k (z): a whole-texel
   mapping the dirt sim can target exactly (only the far edge repeats a texel). */
int mxdirt_height_field_set_vertex_exact(MxdirtHeightField *p,int enabled){
    if(!p||(enabled!=0&&enabled!=1)||
       (p->has_cache&&memcmp(p->cached,p->initial,p->cells*sizeof(*p->initial))))return 0;
    for(int axis=0;axis<2;axis++){
        uint32_t n=axis?p->c.destination_height:p->c.destination_width;
        float span=axis?p->c.native_span_z:p->c.native_span_x;Axis *a=axis?p->z:p->x;
        for(uint32_t k=0;k<n;k++){
            float world=((float)k/(float)(n-1))*span-span*.5f;
            MxdirtHeightFootprint f;
            int valid=mxdirt_height_prepare(axis?0.f:world,axis?world:0.f,p->c.native_span_x,p->c.native_span_z,
                                           p->c.source_width,p->c.source_height,&f);
            a[k]=enabled?(Axis){f.corners[axis],f.corners[axis],0.f,valid}:(Axis){f.corners[axis],f.corners[axis+2],f.fractions[axis],valid};
        }
    }
    build_lookup(p);
    return 1;
}
/* target = initial DFM - (sim height - reference) in DFM units, published 1:1
   (the dirt sim sets its own depth limits: hardpan, rut saturation). */
static int height_target(const MxdirtHeightField *p,size_t k,float e,int32_t *out,int *clipped){
    int32_t v;int limited;
    float bc=p->c.native_height_scale*p->depth_response;
    double metres=((double)mxdirt_height_decode(e,bc)-mxdirt_height_decode(p->c.reference_encoded,bc))*p->c.native_metres;
    double absolute=(double)p->initial[k]-metres*(65535.*65536.)/p->c.mxb_vertical_scale;
    if(!isfinite(absolute))return 0;
    absolute=absolute>=0.?floor(absolute+.5):ceil(absolute-.5);
    int32_t lo,hi;mxb_height_fixed_bounds(p->base[k],&lo,&hi);
    limited=absolute<lo||absolute>hi;v=absolute<lo?lo:absolute>hi?hi:(int32_t)absolute;
    if(v<p->visual_lower){v=p->visual_lower;limited=1;}
    if(v>p->visual_upper){v=p->visual_upper;limited=1;}
    *out=v;*clipped=limited;return 1;
}
int mxdirt_height_field_convert(MxdirtHeightField *p,uint64_t generation,
    const float *source,size_t source_count,int32_t *target,size_t target_count,MxdirtHeightStats *stats){
    if(!p||generation!=p->c.generation||!source||!target||target_count<p->cells||
       source_count<(size_t)p->c.source_stride*p->c.source_height)return 0;
    /* Ignore padding values: only logical native pixels participate in lookup. */
    for(uint32_t z=0;z<p->c.source_height;z++)for(uint32_t x=0;x<p->c.source_width;x++){
        float e=source[(size_t)z*p->c.source_stride+x];
        if(!isfinite(e)||e<0.f||e>1.f)return 0;
    }
    MxdirtHeightStats s={0};
    for(uint32_t z=0;z<p->c.destination_height;z++)for(uint32_t x=0;x<p->c.destination_width;x++){
        size_t k=(size_t)z*p->c.destination_width+x;
        Axis ax=p->x[x],az=p->z[z];
        if(!ax.valid||!az.valid){p->staged[k]=p->initial[k];s.retained_edge++;continue;}
        MxdirtHeightFootprint f={{ax.low,az.low,ax.high,az.high},{ax.fraction,az.fraction,0,0}};
        float e=mxdirt_height_sample(source,p->c.source_stride,&f);int clipped;
        if(!height_target(p,k,e,&p->staged[k],&clipped))return 0;
        s.sampled++;s.clipped+=(clipped!=0);s.changed_from_initial+=(p->staged[k]!=p->initial[k]);
    }
    memcpy(target,p->staged,p->cells*sizeof(*target));
    memcpy(p->cached,p->staged,p->cells*sizeof(*target));p->has_cache=1;p->last_target=target;
    if(p->shadow)memcpy(p->shadow,source,(size_t)p->c.source_stride*p->c.source_height*sizeof(float));
    for(uint32_t z=0;z<p->c.destination_height;z++){
        p->committed_begin[z]=0;p->committed_end[z]=p->c.destination_width;
    }
    if(stats)*stats=s;return 1;
}

int mxdirt_height_field_convert_changed_tiles(MxdirtHeightField *p,uint64_t generation,
    const float *source,size_t source_count,const unsigned char *tiles,size_t tile_count,
    int32_t *target,size_t target_count,MxdirtHeightStats *stats){
    if(!p||!p->has_cache||generation!=p->c.generation||!source||!tiles||!target||target_count<p->cells||
       source_count<(size_t)p->c.source_stride*p->c.source_height)return 0;
    uint32_t nx=(p->c.source_width+127)/128,nz=(p->c.source_height+31)/32;
    if(tile_count<(size_t)nx*nz)return 0;
    for(size_t k=0;k<(size_t)nx*nz;k++)if(tiles[k]>1)return 0;
    for(uint32_t z=0;z<p->c.destination_height;z++){p->begin[z]=UINT32_MAX;p->end[z]=0;}
    for(uint32_t tz=0;tz<nz;tz++)for(uint32_t tx=0;tx<nx;tx++){
        if(!tiles[(size_t)tz*nx+tx])continue;
        /* source texels to account for: the tile, or (fine) the box of its texels that differ
           from the source last converted - a wheel changes a few dozen texels of a 4096-texel tile */
        uint32_t c0=tx*128,c1=c0+128,r0=tz*32,r1=r0+32,xs=0,xe=p->c.destination_width,zs=0,ze=p->c.destination_height;
        if(c1>p->c.source_width)c1=p->c.source_width;if(r1>p->c.source_height)r1=p->c.source_height;
        if(p->fine){
            uint32_t bc0=UINT32_MAX,bc1=0,br0=UINT32_MAX,br1=0;const size_t bytes=(size_t)(c1-c0)*sizeof(float);
            for(uint32_t r=r0;r<r1;r++){
                const float *a=source+(size_t)r*p->c.source_stride+c0,*b=p->shadow+(size_t)r*p->c.source_stride+c0;
                if(!memcmp(a,b,bytes))continue;
                uint32_t f=0,l=c1-c0;while(!memcmp(a+f,b+f,sizeof(float)))f++;while(!memcmp(a+l-1,b+l-1,sizeof(float)))l--;
                if(c0+f<bc0)bc0=c0+f;if(c0+l>bc1)bc1=c0+l;if(br0==UINT32_MAX)br0=r;br1=r+1;
            }
            if(br0==UINT32_MAX)continue; /* marked, but nothing in it changed */
            c0=bc0;c1=bc1;r0=br0;r1=br1;xs=zs=UINT32_MAX;xe=ze=0;
            for(uint32_t c=c0;c<c1;c++){if(p->xfirst[c]<xs)xs=p->xfirst[c];if(p->xend[c]>xe)xe=p->xend[c];}
            for(uint32_t r=r0;r<r1;r++){if(p->zfirst[r]<zs)zs=p->zfirst[r];if(p->zend[r]>ze)ze=p->zend[r];}
        }
        uint32_t start=UINT32_MAX,end=0;float low=(float)c0,high=(float)c1;
        /* A destination lookup is affected if either of its source columns
           intersects the span. Include both neighbours across its borders. */
        for(uint32_t x=xs;x<xe;x++){
            Axis a=p->x[x];if(!a.valid||a.high<low||a.low>=high)continue;
            if(start==UINT32_MAX)start=x;end=x+1;
        }
        if(start==UINT32_MAX)continue;
        low=(float)r0;high=(float)r1;
        for(uint32_t z=zs;z<ze;z++){
            Axis a=p->z[z];if(!a.valid||a.high<low||a.low>=high)continue;
            if(start<p->begin[z])p->begin[z]=start;if(end>p->end[z])p->end[z]=end;
        }
    }
    MxdirtHeightStats s={0};
    for(uint32_t z=0;z<p->c.destination_height;z++)for(uint32_t x=p->begin[z];x<p->end[z];x++){
        size_t k=(size_t)z*p->c.destination_width+x;Axis ax=p->x[x],az=p->z[z];
        MxdirtHeightFootprint f={{ax.low,az.low,ax.high,az.high},{ax.fraction,az.fraction,0,0}};
        /* Constructor's <=2^24 storage bound makes these integer indices
           exactly equal to the binary32 address arithmetic of the lookup. */
        size_t row0=(size_t)az.low*p->c.source_stride,row1=(size_t)az.high*p->c.source_stride;
        size_t indices[4]={row0+(size_t)ax.low,row0+(size_t)ax.high,row1+(size_t)ax.low,row1+(size_t)ax.high};
        for(unsigned j=0;j<4;j++){float e=source[indices[j]];if(!isfinite(e)||e<0.f||e>1.f)return 0;}
        float e=mxdirt_height_sample(source,p->c.source_stride,&f);int clipped;
        if(!height_target(p,k,e,&p->staged[k],&clipped))return 0;
        s.sampled++;s.clipped+=(clipped!=0);s.changed_from_initial+=(p->staged[k]!=p->initial[k]);
    }
    for(uint32_t z=0;z<p->c.destination_height;z++)if(p->begin[z]!=UINT32_MAX){
        size_t k=(size_t)z*p->c.destination_width+p->begin[z];
        memcpy(p->cached+k,p->staged+k,(p->end[z]-p->begin[z])*sizeof(*target));
    }
    /* The same target buffer already mirrors cached from the previous call,
       so only the rows changed this call are copied (was 16 MiB per frame on
       a 2049-square grid). A different buffer still receives the full image. */
    if(target==p->last_target){
        for(uint32_t z=0;z<p->c.destination_height;z++)if(p->begin[z]!=UINT32_MAX){
            size_t k=(size_t)z*p->c.destination_width+p->begin[z];
            memcpy(target+k,p->cached+k,(p->end[z]-p->begin[z])*sizeof(*target));
        }
    }else{memcpy(target,p->cached,p->cells*sizeof(*target));p->last_target=target;}
    memcpy(p->committed_begin,p->begin,p->c.destination_height*sizeof(*p->begin));
    memcpy(p->committed_end,p->end,p->c.destination_height*sizeof(*p->end));
    /* the conversion succeeded: these tiles' source is now the source last converted */
    if(p->fine)for(uint32_t tz=0;tz<nz;tz++)for(uint32_t tx=0;tx<nx;tx++){
        if(!tiles[(size_t)tz*nx+tx])continue;
        uint32_t c0=tx*128,c1=c0+128,r0=tz*32,r1=r0+32;
        if(c1>p->c.source_width)c1=p->c.source_width;if(r1>p->c.source_height)r1=p->c.source_height;
        const size_t bytes=(size_t)(c1-c0)*sizeof(float);
        for(uint32_t r=r0;r<r1;r++){
            const float *a=source+(size_t)r*p->c.source_stride+c0;float *b=p->shadow+(size_t)r*p->c.source_stride+c0;
            if(memcmp(a,b,bytes))memcpy(b,a,bytes);
        }
    }
    if(stats)*stats=s;return 1;
}
int mxdirt_height_field_changed_rows(const MxdirtHeightField *p,uint64_t generation,
    const uint32_t **begin,const uint32_t **end,size_t *rows){
    if(!p||!p->has_cache||generation!=p->c.generation||!begin||!end||!rows)return 0;
    *begin=p->committed_begin;*end=p->committed_end;*rows=p->c.destination_height;return 1;
}
