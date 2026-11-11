#include "mxdirt_dirt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
struct MxDirt {
 int valid;int32_t first_x,first_z;
 uintptr_t current_grid,base_grid;uint32_t width,height;
 uint16_t *band;size_t band_cap; /* one contiguous read of SIDE rows */
 uint16_t texels[MXDIRT_SIDE*MXDIRT_SIDE*2];
 int state_valid,state_fresh;int32_t state_x,state_z;uint64_t state_steps;const uint8_t *state_src;
 unsigned char state[MXDIRT_SIDE*MXDIRT_SIDE*4];
};
MxDirt *mxdirt_create(void){return calloc(1,sizeof(MxDirt));}
void mxdirt_destroy(MxDirt *d){if(d)free(d->band);free(d);}
static int finite_all(const float *v,unsigned n){for(unsigned k=0;k<n;k++)if(!isfinite(v[k]))return 0;return 1;}
static int32_t clamp_first(double centre,uint32_t size){
 double first=floor(centre+.5)-MXDIRT_CELLS/2-MXDIRT_MARGIN,last=(double)size-MXDIRT_SIDE;
 return (int32_t)fmax(0,fmin(last,first));
}
/* Reads the SIDE-row window as ONE contiguous span (row pitch = grid width). */
static int read_band(MxDirt *d,const MxDirtInput *in,uintptr_t grid,MxDirtStats *st){
 size_t samples=(size_t)(MXDIRT_SIDE-1)*in->width+MXDIRT_SIDE,offset=((size_t)(uint32_t)d->first_z*in->width+(uint32_t)d->first_x)*2;
 if(samples>d->band_cap){uint16_t *b=realloc(d->band,samples*2);if(!b)return 0;d->band=b;d->band_cap=samples;}
 if(grid>UINTPTR_MAX-offset)return 0;st->read_calls++;st->read_bytes+=(uint32_t)(samples*2);
 return in->read(in->read_user,grid+offset,d->band,samples*2)==1;
}
int mxdirt_update(MxDirt *d,const MxDirtInput *in,MxDirtWindow *out,MxDirtStats *st){
 MxDirtStats local;if(!st)st=&local;memset(st,0,sizeof(*st));st->stage=MXDIRT_INPUT;
 if(!d||!in||!out||!in->read||!in->current_grid||!in->base_grid||in->width<MXDIRT_SIDE||in->height<MXDIRT_SIDE||
    in->width>16384||in->height>16384||!finite_all(in->modelview,16)||!finite_all(&in->extent_x,3)||!finite_all(in->origin,3)||
    !(in->extent_x>0)||!(in->extent_z>0)||!(in->height_scale>0))return 0;
 size_t grid_bytes=(size_t)in->width*in->height*2;
 if(in->current_grid>UINTPTR_MAX-grid_bytes||in->base_grid>UINTPTR_MAX-grid_bytes)return 0;
 const double sx=(double)in->extent_x/(in->width-1),sz=(double)in->extent_z/(in->height-1);
 /* Camera = -R^T t for column-major world->eye; forward is eye -Z in world. */
 const float *m=in->modelview;double camera[3],forward[3];
 for(unsigned c=0;c<3;c++){camera[c]=0;for(unsigned r=0;r<3;r++)camera[c]-=(double)m[c*4+r]*m[12+r];forward[c]=-(double)m[c*4+2];}
 st->stage=MXDIRT_CAMERA;if(!isfinite(camera[0])||!isfinite(camera[1])||!isfinite(camera[2]))return 0;
 double flat=hypot(forward[0],forward[2]);double ahead=flat>1e-3?MXDIRT_AHEAD/flat:0;
 double centre_x=(camera[0]-in->origin[0])/sx+forward[0]*ahead,centre_z=(camera[2]-in->origin[2])/sz+forward[2]*ahead;
 if(!isfinite(centre_x)||!isfinite(centre_z))return 0;
 int32_t want_x=clamp_first(centre_x,in->width),want_z=clamp_first(centre_z,in->height);
 int same_grid=d->valid&&d->current_grid==in->current_grid&&d->base_grid==in->base_grid&&d->width==in->width&&d->height==in->height;
 int recentre=!same_grid||abs(want_x-d->first_x)>MXDIRT_RECENTER||abs(want_z-d->first_z)>MXDIRT_RECENTER;
 st->stage=MXDIRT_READ;
 if(recentre){d->valid=0;d->first_x=want_x;d->first_z=want_z;}
 int changed=recentre;
 if(recentre){
  if(!read_band(d,in,in->base_grid,st))return 0;
  for(int32_t r=0;r<MXDIRT_SIDE;r++){uint16_t *dst=d->texels+(size_t)r*MXDIRT_SIDE*2;const uint16_t *src=d->band+(size_t)r*in->width;for(unsigned c=0;c<MXDIRT_SIDE;c++)dst[2*c+1]=src[c];}
 }
 if(!read_band(d,in,in->current_grid,st)){d->valid=0;return 0;}
 for(int32_t r=0;r<MXDIRT_SIDE;r++){
  uint16_t *dst=d->texels+(size_t)r*MXDIRT_SIDE*2;const uint16_t *src=d->band+(size_t)r*in->width;
  for(unsigned c=0;c<MXDIRT_SIDE;c++)if(dst[2*c]!=src[c]){dst[2*c]=src[c];changed=1;}
 }
 d->valid=1;d->current_grid=in->current_grid;d->base_grid=in->base_grid;d->width=in->width;d->height=in->height;
 memset(out,0,sizeof(*out));out->first_x=d->first_x;out->first_z=d->first_z;out->spacing_x=(float)sx;out->spacing_z=(float)sz;out->height_scale=in->height_scale;
 out->texel0[0]=(float)(in->origin[0]+d->first_x*sx);out->texel0[1]=in->origin[1];out->texel0[2]=(float)(in->origin[2]+d->first_z*sz);
 out->bounds[0]=(float)(in->origin[0]+(d->first_x+MXDIRT_MARGIN)*sx);out->bounds[1]=(float)(in->origin[2]+(d->first_z+MXDIRT_MARGIN)*sz);
 out->bounds[2]=(float)(in->origin[0]+(d->first_x+MXDIRT_MARGIN+MXDIRT_CELLS)*sx);out->bounds[3]=(float)(in->origin[2]+(d->first_z+MXDIRT_MARGIN+MXDIRT_CELLS)*sz);
 for(unsigned c=0;c<3;c++)out->camera[c]=(float)camera[c];
 out->texels=d->texels;out->changed=(uint32_t)changed;out->recentered=(uint32_t)recentre;
 st->recentered=out->recentered;st->changed=out->changed;st->stage=MXDIRT_OK;return 1;
}
/* ---- generated clod relief (rounded grains on a tileable lattice) ---- */
static uint32_t mix32(uint32_t x){x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;}
static float cell_random(int x,int y,int n,uint32_t salt){
 x=((x%n)+n)%n;y=((y%n)+n)%n;return (float)(mix32((uint32_t)x*73856093u^(uint32_t)y*19349663u^salt*83492791u)&0xffffff)/16777215.f;
}
/* Union of rounded crumbs: every cell holds two grains of random size and
   squash; the surface is the highest grain at each point. Gaps between grains
   are small soft pockets, never a continuous crack network. */
static float clod_layer(float u,float v,int n,uint32_t salt){
 float pu=u*n,pv=v*n;int cu=(int)floorf(pu),cv=(int)floorf(pv);float top=0;
 for(int j=-1;j<=1;j++)for(int i=-1;i<=1;i++)for(uint32_t g=0;g<2;g++){
  int x=cu+i,y=cv+j;uint32_t s=salt+g*7;
  float fx=(float)x+cell_random(x,y,n,s),fy=(float)y+cell_random(x,y,n,s+1);
  float r=.32f+.42f*cell_random(x,y,n,s+2),squash=.55f+.45f*cell_random(x,y,n,s+3);
  float t=hypotf(pu-fx,pv-fy)/r;if(t<1.f)top=fmaxf(top,sqrtf(1.f-t*t)*r*squash);
 }
 return top/.74f;
}
void mxdirt_clods(unsigned char *rgba){
 enum {N=MXDIRT_CLOD_SIDE};float *h=malloc(sizeof(float)*N*N),*grain=malloc(sizeof(float)*N*N);float top=1e-6f;
 if(!h||!grain){free(h);free(grain);if(rgba)memset(rgba,128,(size_t)N*N*4);return;}
 for(int y=0;y<N;y++)for(int x=0;x<N;x++){float u=(x+.5f)/N,v=(y+.5f)/N;
  float a=clod_layer(u,v,7,11),b=clod_layer(u,v,17,29),c=clod_layer(u,v,41,47);
  float value=.50f*a+.32f*b+.18f*c;h[y*N+x]=value;grain[y*N+x]=a>.02f?1.f:0.f;if(value>top)top=value;
 }
 for(int y=0;y<N;y++)for(int x=0;x<N;x++){
  float gx=(h[y*N+(x+1)%N]-h[y*N+(x+N-1)%N])*.5f/top,gy=(h[((y+1)%N)*N+x]-h[((y+N-1)%N)*N+x])*.5f/top;
  float sx=fmaxf(-1.f,fminf(1.f,gx*6.f)),sy=fmaxf(-1.f,fminf(1.f,gy*6.f));unsigned char *p=rgba+(size_t)(y*N+x)*4;
  p[0]=(unsigned char)lrintf(127.5f+127.5f*sx);p[1]=(unsigned char)lrintf(127.5f+127.5f*sy);
  p[2]=(unsigned char)lrintf(255.f*fminf(1.f,h[y*N+x]/top));p[3]=(unsigned char)lrintf(255.f*grain[y*N+x]);
 }
 free(h);free(grain);
}
static unsigned char unit_byte(float v){return (unsigned char)lrintf(255.f*fmaxf(0.f,fminf(1.f,v)));}
int mxdirt_state(MxDirt *d,const MxDirtWindow *w,uint32_t grid_width,uint32_t grid_height,const MxDirtSim *m,MxDirtState *out){
 if(!out)return 0;memset(out,0,sizeof(*out));
 if(!d||!w||!m||!m->comp||!m->loose||!m->moist||!m->clods||m->size<128||m->size>8192||grid_width<2||grid_height<2||
    !isfinite(m->moisture)||!isfinite(m->compaction0)){if(d)d->state_valid=0;return 0;}
 int moved=!d->state_valid||d->state_x!=w->first_x||d->state_z!=w->first_z||d->state_src!=m->comp;
 /* While riding the sim steps every frame: rebuild at most every other frame
    (a one-frame-late wetness/clod look is invisible; ~0.1 ms per rebuild). */
 int changed=moved||(d->state_steps!=m->steps&&!d->state_fresh);d->state_fresh=0;
 if(changed){
  const int32_t n=(int32_t)m->size;int mo8=(int)lrintf(fminf(1.f,fmaxf(0.f,m->moisture))*255.f);
  /* packed = (compaction-.30)/.50, untouched (0) reads 0; integer per texel */
  uint8_t pk_lut[256];pk_lut[0]=0;for(int v=1;v<256;v++)pk_lut[v]=unit_byte(((float)v/255.f-.30f)/.50f);
  for(int32_t r=0;r<MXDIRT_SIDE;r++){
   /* native row j (z up) -> sim row (rows run with -z) */
   int32_t j=w->first_z+r,row=m->vertex_exact?n-1-j:(int32_t)lrint((double)(n-1)-(double)j*(n-1)/(grid_height-1));
   if(row<0)row=0;if(row>n-1)row=n-1;
   unsigned char *dst=d->state+(size_t)r*MXDIRT_SIDE*4;size_t base=(size_t)row*n;
   const uint8_t *comp=m->comp+base,*clods=m->clods+base;const float *loose=m->loose+base;const int8_t *moist=m->moist+base;
   for(int32_t c=0;c<MXDIRT_SIDE;c++){
    int32_t i=w->first_x+c,col=m->vertex_exact?i-1:(int32_t)lrint((double)i*(n-1)/(grid_width-1));
    if(col<0)col=0;if(col>n-1)col=n-1;
    float L=loose[col];int lo=L>0.f?(L<.04f?(int)(L*6375.f+.5f):255):0;     /* loose depth / 4 cm */
    int wet=mo8+moist[col];wet=wet<0?0:wet>255?255:wet;
    dst[4*c]=(unsigned char)((pk_lut[comp[col]]*(255-lo)+127)/255);dst[4*c+1]=(unsigned char)lo;dst[4*c+2]=(unsigned char)wet;dst[4*c+3]=clods[col];
   }
  }
  d->state_valid=1;d->state_x=w->first_x;d->state_z=w->first_z;d->state_steps=m->steps;d->state_src=m->comp;d->state_fresh=1;
 }
 out->valid=1;out->changed=changed;out->rgba=d->state;return 1;
}
