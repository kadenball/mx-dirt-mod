#include "mxdirt_dirt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
enum { S=MXDIRT_SIDE };
struct MxDirt {
 int valid;int32_t first_x,first_z;uint32_t shift_x,shift_z;
 uintptr_t current_grid,base_grid;uint32_t width,height;
 uint16_t *band;size_t band_cap; /* copy of the window's span when it cannot be looked at in place */
 uint32_t slice;uint64_t epoch;  /* next row slice to compare; counts full reloads */
 MxDirtDirty dirty;
 int state_valid,state_fresh;int32_t state_x,state_z;uint64_t state_steps,state_epoch;const uint8_t *state_src;uint32_t state_slice;
 MxDirtDirty state_dirty;
 float camera_cell[2];           /* camera in window texels (near box centre) */
 uint16_t texels[S*S*2];
 unsigned char state[S*S*4];
};
MxDirt *mxdirt_create(void){return calloc(1,sizeof(MxDirt));}
void mxdirt_destroy(MxDirt *d){if(d)free(d->band);free(d);}
static int finite_all(const float *v,unsigned n){for(unsigned k=0;k<n;k++)if(!isfinite(v[k]))return 0;return 1;}
/* First grid index of the window: inside the grid when the grid is big enough, else centred over it. */
static int32_t clamp_first(double centre,uint32_t size){
 if(size<S)return ((int32_t)size-S)/2;
 double first=floor(centre+.5)-MXDIRT_CELLS/2-MXDIRT_MARGIN,last=(double)size-S;
 return (int32_t)fmax(0,fmin(last,first));
}
static void dirty_rect(MxDirtDirty *q,uint32_t x,uint32_t y,uint32_t w,uint32_t h){
 if(!w||!h||q->n>MXDIRT_RECTS)return;
 if(q->n==MXDIRT_RECTS){q->n=MXDIRT_RECTS+1;return;}
 MxDirtRect r={(uint16_t)x,(uint16_t)y,(uint16_t)w,(uint16_t)h};q->r[q->n++]=r;
}
/* The window box [c0,c1) x [r0,r1) as its 1..4 storage rectangles. */
static void dirty_box(MxDirtDirty *q,uint32_t sx,uint32_t sz,int32_t c0,int32_t c1,int32_t r0,int32_t r1){
 if(c1<=c0||r1<=r0)return;
 uint32_t x0=((uint32_t)c0+sx)%S,xn=(uint32_t)(c1-c0),z0=((uint32_t)r0+sz)%S,zn=(uint32_t)(r1-r0);
 /* the whole width or height is one rectangle; a box that crosses the seam in x goes as whole rows, so a
    box is never more than two uploads (each upload call is what costs in the game, not its bytes) */
 if(xn>=S||x0+xn>S){x0=0;xn=S;}if(zn>=S){z0=0;zn=S;}
 uint32_t xa=xn<S-x0?xn:S-x0,za=zn<S-z0?zn:S-z0;
 dirty_rect(q,x0,z0,xa,za);dirty_rect(q,0,z0,xn-xa,za);dirty_rect(q,x0,0,xa,zn-za);dirty_rect(q,0,0,xn-xa,zn-za);
}
static int32_t clampi(int32_t v,int32_t lo,int32_t hi){return v<lo?lo:v>hi?hi:v;}
/* The part of the grid under the window, as ONE contiguous span (row pitch = grid width): looked at in
   place when the host offers a view, else copied out. Window texel (c,r) is span[row[r]+col[c]]. */
typedef struct {const uint16_t *span;uint32_t row[S],col[S];} Band;
static int read_band(MxDirt *d,const MxDirtInput *in,uintptr_t grid,MxDirtStats *st,Band *b){
 const int32_t x0=clampi(d->first_x,0,(int32_t)in->width-1),x1=clampi(d->first_x+S-1,0,(int32_t)in->width-1);
 const int32_t z0=clampi(d->first_z,0,(int32_t)in->height-1),z1=clampi(d->first_z+S-1,0,(int32_t)in->height-1);
 size_t samples=(size_t)(z1-z0)*in->width+(size_t)(x1-x0)+1,offset=((size_t)z0*in->width+(size_t)x0)*2;
 for(int32_t k=0;k<S;k++){b->col[k]=(uint32_t)(clampi(d->first_x+k,x0,x1)-x0);b->row[k]=(uint32_t)(clampi(d->first_z+k,z0,z1)-z0)*in->width;}
 b->span=NULL;if(grid>UINTPTR_MAX-offset)return 0;st->read_calls++;
 if(in->view&&!((grid+offset)&1)){const void *v=in->view(in->read_user,grid+offset,samples*2);if(v){b->span=v;return 1;}}
 if(samples>d->band_cap){uint16_t *n=realloc(d->band,samples*2);if(!n)return 0;d->band=n;d->band_cap=samples;}
 st->read_bytes+=(uint32_t)(samples*2);
 if(in->read(in->read_user,grid+offset,d->band,samples*2)!=1)return 0;
 b->span=d->band;return 1;
}
/* Window texels [c0,c1) x [r0,r1) taken from the band; which: 0 current, 1 base. */
static void fill(MxDirt *d,const Band *b,int which,int32_t c0,int32_t c1,int32_t r0,int32_t r1,MxDirtStats *st){
 if(c1<=c0||r1<=r0)return;
 for(int32_t r=r0;r<r1;r++){uint16_t *row=d->texels+(size_t)(((uint32_t)r+d->shift_z)%S)*S*2;const uint16_t *src=b->span+b->row[r];
  for(int32_t c=c0;c<c1;c++)row[2*(((uint32_t)c+d->shift_x)%S)+which]=src[b->col[c]];}
 st->read_bytes+=(uint32_t)((size_t)(c1-c0)*(size_t)(r1-r0)*2);
}
/* CURRENT of the window texels [c0,c1) x [r0,r1) against the band; what differs is taken over and its
   bounding box (window texels) grown. */
static void compare(MxDirt *d,const Band *b,int32_t c0,int32_t c1,int32_t r0,int32_t r1,int32_t box[4],MxDirtStats *st){
 if(c1<=c0||r1<=r0)return;
 const int32_t wrap=(int32_t)(S-d->shift_x),ca=c1<wrap?c1:wrap,cb=c0>wrap?c0:wrap; /* [c0,ca) is stored at c+shift, [cb,c1) at c+shift-S */
 for(int32_t r=r0;r<r1;r++){uint16_t *row=d->texels+(size_t)(((uint32_t)r+d->shift_z)%S)*S*2;const uint16_t *src=b->span+b->row[r];
  for(int32_t c=c0;c<ca;c++){const uint16_t v=src[b->col[c]];uint16_t *t=row+2*((size_t)c+d->shift_x);if(*t!=v){*t=v;if(c<box[0])box[0]=c;if(c>=box[1])box[1]=c+1;if(r<box[2])box[2]=r;if(r>=box[3])box[3]=r+1;}}
  for(int32_t c=cb;c<c1;c++){const uint16_t v=src[b->col[c]];uint16_t *t=row+2*(size_t)(c-wrap);if(*t!=v){*t=v;if(c<box[0])box[0]=c;if(c>=box[1])box[1]=c+1;if(r<box[2])box[2]=r;if(r>=box[3])box[3]=r+1;}}
 }
 st->read_bytes+=(uint32_t)((size_t)(c1-c0)*(size_t)(r1-r0)*2);
}
/* Storage rows of slice k of n, as window row runs [run[i][0],run[i][1]) (the second is empty unless they wrap). */
static void slice_rows(uint32_t k,uint32_t n,uint32_t shift_z,int32_t run[2][2]){
 const uint32_t s0=(uint32_t)((uint64_t)k*S/n),s1=(uint32_t)((uint64_t)(k+1)*S/n),count=s1-s0,r0=(s0+S-shift_z)%S;
 run[0][0]=(int32_t)r0;run[0][1]=(int32_t)(r0+count<S?r0+count:S);run[1][0]=0;run[1][1]=(int32_t)(r0+count>S?r0+count-S:0);
}
int mxdirt_update(MxDirt *d,const MxDirtInput *in,MxDirtWindow *out,MxDirtStats *st){
 MxDirtStats local;if(!st)st=&local;memset(st,0,sizeof(*st));st->stage=MXDIRT_INPUT;
 if(!d||!in||!out||!in->read||!in->current_grid||!in->base_grid||in->width<2||in->height<2||
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
 const double cam_x=(camera[0]-in->origin[0])/sx,cam_z=(camera[2]-in->origin[2])/sz;
 double centre_x=cam_x+forward[0]*ahead,centre_z=cam_z+forward[2]*ahead;
 if(!isfinite(centre_x)||!isfinite(centre_z))return 0;
 int32_t want_x=clamp_first(centre_x,in->width),want_z=clamp_first(centre_z,in->height);
 int same_grid=d->valid&&d->current_grid==in->current_grid&&d->base_grid==in->base_grid&&d->width==in->width&&d->height==in->height;
 int32_t dx=same_grid?want_x-d->first_x:0,dz=same_grid?want_z-d->first_z:0;
 if(abs(dx)<=MXDIRT_RECENTER&&abs(dz)<=MXDIRT_RECENTER)dx=dz=0;
 const int full=!same_grid||abs(dx)>=S||abs(dz)>=S,recentre=full||dx||dz;
 st->stage=MXDIRT_READ;d->valid=0; /* a failed read forgets the window */
 if(full){d->first_x=want_x;d->first_z=want_z;d->shift_x=d->shift_z=0;d->slice=0;d->epoch++;d->dirty.n=MXDIRT_RECTS+1;dx=dz=0;}
 else if(recentre){d->first_x+=dx;d->first_z+=dz;d->shift_x=(uint32_t)(((int32_t)d->shift_x+dx)%S+S)%S;d->shift_z=(uint32_t)(((int32_t)d->shift_z+dz)%S+S)%S;}
 /* rows and columns that entered: [nr0,nr1) x all columns, all rows x [nc0,nc1) */
 const int32_t nr0=full?0:dz>0?S-dz:0,nr1=full?S:dz>0?S:-dz,nc0=full?0:dx>0?S-dx:0,nc1=full?0:dx>0?S:-dx;
 static Band band; /* owner thread only (as the whole module) */
 if(recentre){
  if(!read_band(d,in,in->base_grid,st,&band))return 0;
  fill(d,&band,1,0,S,nr0,nr1,st);fill(d,&band,1,nc0,nc1,0,S,st);
 }
 if(!read_band(d,in,in->current_grid,st,&band))return 0;
 int changed=recentre;
 if(recentre){
  fill(d,&band,0,0,S,nr0,nr1,st);fill(d,&band,0,nc0,nc1,0,S,st);
  if(!full){dirty_box(&d->dirty,d->shift_x,d->shift_z,0,S,nr0,nr1);dirty_box(&d->dirty,d->shift_x,d->shift_z,nc0,nc1,0,S);}
 }
 d->camera_cell[0]=(float)(cam_x-d->first_x);d->camera_cell[1]=(float)(cam_z-d->first_z);
 if(!full){
  /* the box around the camera, then one slice of rows of the whole window */
  const int32_t cc=(int32_t)floor(cam_x)-d->first_x,cr=(int32_t)floor(cam_z)-d->first_z;
  int32_t box[4]={S,0,S,0};
  compare(d,&band,clampi(cc-MXDIRT_NEAR,0,S),clampi(cc+MXDIRT_NEAR+1,0,S),clampi(cr-MXDIRT_NEAR,0,S),clampi(cr+MXDIRT_NEAR+1,0,S),box,st);
  if(box[1]>box[0]){changed=1;dirty_box(&d->dirty,d->shift_x,d->shift_z,box[0],box[1],box[2],box[3]);}
  /* a slice of STORAGE rows: a grid row keeps its storage row while it is in the window, so every row is
     compared once in MXDIRT_SLICES updates however the window scrolls. As window rows that is 1 or 2 runs. */
  int32_t run[2][2];slice_rows(d->slice,MXDIRT_SLICES,d->shift_z,run);d->slice=(d->slice+1)%MXDIRT_SLICES;
  for(int k=0;k<2;k++){int32_t far_box[4]={S,0,S,0};compare(d,&band,0,S,run[k][0],run[k][1],far_box,st);
   if(far_box[1]>far_box[0]){changed=1;dirty_box(&d->dirty,d->shift_x,d->shift_z,far_box[0],far_box[1],far_box[2],far_box[3]);}}
 }
 d->valid=1;d->current_grid=in->current_grid;d->base_grid=in->base_grid;d->width=in->width;d->height=in->height;
 memset(out,0,sizeof(*out));out->first_x=d->first_x;out->first_z=d->first_z;out->shift_x=d->shift_x;out->shift_z=d->shift_z;
 out->spacing_x=(float)sx;out->spacing_z=(float)sz;out->height_scale=in->height_scale;
 out->texel0[0]=(float)(in->origin[0]+d->first_x*sx);out->texel0[1]=in->origin[1];out->texel0[2]=(float)(in->origin[2]+d->first_z*sz);
 const int32_t bx0=clampi(d->first_x+MXDIRT_MARGIN,0,(int32_t)in->width-1),bx1=clampi(d->first_x+MXDIRT_MARGIN+MXDIRT_CELLS,0,(int32_t)in->width-1);
 const int32_t bz0=clampi(d->first_z+MXDIRT_MARGIN,0,(int32_t)in->height-1),bz1=clampi(d->first_z+MXDIRT_MARGIN+MXDIRT_CELLS,0,(int32_t)in->height-1);
 out->bounds[0]=(float)(in->origin[0]+bx0*sx);out->bounds[1]=(float)(in->origin[2]+bz0*sz);
 out->bounds[2]=(float)(in->origin[0]+bx1*sx);out->bounds[3]=(float)(in->origin[2]+bz1*sz);
 for(unsigned c=0;c<3;c++)out->camera[c]=(float)camera[c];
 out->texels=d->texels;out->dirty=&d->dirty;out->changed=(uint32_t)changed;out->recentered=(uint32_t)recentre;
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
typedef struct {const MxDirtSim *m;int32_t n,mo8;uint32_t grid_width,grid_height;uint8_t pk_lut[256];int32_t cols[S];} StateMap;
/* The state of window texels [c0,c1) x [r0,r1), written to their storage place. */
static void state_fill(MxDirt *d,const MxDirtWindow *w,const StateMap *q,int32_t c0,int32_t c1,int32_t r0,int32_t r1){
 if(c1<=c0||r1<=r0)return;const MxDirtSim *m=q->m;const int32_t n=q->n;
 for(int32_t r=r0;r<r1;r++){
  /* native row j (z up) -> sim row (rows run with -z) */
  int32_t j=w->first_z+r,row=m->vertex_exact?n-1-j:(int32_t)lrint((double)(n-1)-(double)j*(n-1)/(q->grid_height-1));
  if(row<0)row=0;if(row>n-1)row=n-1;
  unsigned char *line=d->state+(size_t)(((uint32_t)r+w->shift_z)%S)*S*4;size_t base=(size_t)row*n;
  const uint8_t *comp=m->comp+base,*clods=m->clods+base;const float *loose=m->loose+base;const int8_t *moist=m->moist+base;
  for(int32_t c=c0;c<c1;c++){
   const int32_t col=q->cols[c];unsigned char *dst=line+4*(size_t)(((uint32_t)c+w->shift_x)%S);
   float L=loose[col];int lo=L>0.f?(L<.04f?(int)(L*6375.f+.5f):255):0;     /* loose depth / 4 cm */
   int wet=q->mo8+moist[col];wet=wet<0?0:wet>255?255:wet;
   dst[0]=(unsigned char)((q->pk_lut[comp[col]]*(255-lo)+127)/255);dst[1]=(unsigned char)lo;dst[2]=(unsigned char)wet;dst[3]=clods[col];
  }
 }
 dirty_box(&d->state_dirty,w->shift_x,w->shift_z,c0,c1,r0,r1);
}
int mxdirt_state(MxDirt *d,const MxDirtWindow *w,uint32_t grid_width,uint32_t grid_height,const MxDirtSim *m,MxDirtState *out){
 if(!out)return 0;memset(out,0,sizeof(*out));
 if(!d||!w||!m||!m->comp||!m->loose||!m->moist||!m->clods||m->size<128||m->size>8192||grid_width<2||grid_height<2||
    !isfinite(m->moisture)||!isfinite(m->compaction0)){if(d)d->state_valid=0;return 0;}
 const int32_t dx=w->first_x-d->state_x,dz=w->first_z-d->state_z;
 const int full=!d->state_valid||d->state_src!=m->comp||d->state_epoch!=d->epoch||abs(dx)>=S||abs(dz)>=S,moved=full||dx||dz;
 /* While riding the sim steps every frame: rebuild at most every other frame
    (a one-frame-late wetness/clod look is invisible). */
 const int stepped=!full&&d->state_steps!=m->steps&&!d->state_fresh;d->state_fresh=0;
 if(moved||stepped){
  static StateMap q;q.m=m;q.n=(int32_t)m->size;q.mo8=(int)lrintf(fminf(1.f,fmaxf(0.f,m->moisture))*255.f);q.grid_width=grid_width;q.grid_height=grid_height;
  /* packed = (compaction-.30)/.50, untouched (0) reads 0; integer per texel */
  q.pk_lut[0]=0;for(int v=1;v<256;v++)q.pk_lut[v]=unit_byte(((float)v/255.f-.30f)/.50f);
  /* window column -> sim column, the same on every row */
  for(int32_t c=0;c<S;c++){int32_t i=w->first_x+c,col=m->vertex_exact?i-1:(int32_t)lrint((double)i*(q.n-1)/(grid_width-1));
   q.cols[c]=col<0?0:col>q.n-1?q.n-1:col;}
  if(full){d->state_dirty.n=0;state_fill(d,w,&q,0,S,0,S);d->state_dirty.n=MXDIRT_RECTS+1;d->state_slice=0;}
  else{
   if(dz)state_fill(d,w,&q,0,S,dz>0?S-dz:0,dz>0?S:-dz);
   if(dx)state_fill(d,w,&q,dx>0?S-dx:0,dx>0?S:-dx,0,S);
   if(stepped){
    const int32_t cc=(int32_t)floorf(d->camera_cell[0]),cr=(int32_t)floorf(d->camera_cell[1]);
    state_fill(d,w,&q,clampi(cc-MXDIRT_NEAR,0,S),clampi(cc+MXDIRT_NEAR+1,0,S),clampi(cr-MXDIRT_NEAR,0,S),clampi(cr+MXDIRT_NEAR+1,0,S));
    int32_t run[2][2];slice_rows(d->state_slice,MXDIRT_STATE_SLICES,w->shift_z,run);d->state_slice=(d->state_slice+1)%MXDIRT_STATE_SLICES;
    state_fill(d,w,&q,0,S,run[0][0],run[0][1]);state_fill(d,w,&q,0,S,run[1][0],run[1][1]);
   }
  }
  d->state_valid=1;d->state_x=w->first_x;d->state_z=w->first_z;d->state_src=m->comp;d->state_epoch=d->epoch;
  if(full||stepped){d->state_steps=m->steps;d->state_fresh=1;}
 }
 out->valid=1;out->changed=moved||stepped;out->rgba=d->state;out->dirty=&d->state_dirty;return 1;
}
