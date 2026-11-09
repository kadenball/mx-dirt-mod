#include "mxdirt_native_height.h"
#include <math.h>
#include <limits.h>
static float mxb_nh_sample(const MxbNativeHeightGrid *g,uint32_t x,uint32_t z,float factor){return (float)g->samples[(size_t)z*g->width+x]*factor;}
static int mxb_nh_normalize(float v[3]){
 float xx=v[0]*v[0],yy=v[1]*v[1],zz=v[2]*v[2];float sum=xx+yy;sum=sum+zz;
 if(!isfinite(sum))return 0;float length=(float)sqrt((double)sum);
 /* Native267c00 leaves its in-place vector alone when267b80 length is tiny. */
 if((double)length<1e-6)return 1;float reciprocal=1.f/length;
 v[0]=v[0]*reciprocal;v[1]=v[1]*reciprocal;v[2]=v[2]*reciprocal;return 1;
}
int mxb_native_height_query(const MxbNativeHeightGrid *g,float x,float z,MxbNativeHeightPoint *out){
 if(!g||!out||!g->samples||g->width<2||g->height<2||g->width>8193||g->height>8193||
  g->sample_count<(size_t)g->width*g->height||!isfinite(x)||!isfinite(z)||
  !isfinite(g->extent_x)||!isfinite(g->extent_z)||g->extent_x<=0||g->extent_z<=0||
  !isfinite(g->height_scale)||!isfinite(g->origin[0])||!isfinite(g->origin[1])||!isfinite(g->origin[2]))return 0;
 float max_x=g->origin[0]+g->extent_x,max_z=g->origin[2]+g->extent_z;
 if(!isfinite(max_x)||!isfinite(max_z)||x<g->origin[0]||x>max_x||z<g->origin[2]||z>max_z)return 0;
 float u=x-g->origin[0],v=z-g->origin[2];v=v/g->extent_z;u=u/g->extent_x;
 u=u*(float)(g->width-1);v=v*(float)(g->height-1);
 /* Native uses truncation then clamps only the next neighbor. At the inclusive
    world edge, rounding can leave a tiny positive fraction at the last node. */
 if(!isfinite(u)||!isfinite(v)||u<0||v<0||u>=(float)g->width||v>=(float)g->height)return 0;
 uint32_t x0=(uint32_t)u,z0=(uint32_t)v,x1=x0<g->width-1?x0+1:x0,z1=z0<g->height-1?z0+1:z0;
 u=u-(float)x0;v=v-(float)z0;
 float factor=(float)((double)g->height_scale/65535.0),iu=1.f-u,iv=1.f-v;
 float w00=iu*iv,w10=iv*u,w01=iu*v,w11=v*u;
 float h00=mxb_nh_sample(g,x0,z0,factor),h10=mxb_nh_sample(g,x1,z0,factor),h01=mxb_nh_sample(g,x0,z1,factor),h11=mxb_nh_sample(g,x1,z1,factor);
 float height=h10*w10,h=h00*w00;height=height+h;h=h01*w01;height=height+h;h=h11*w11;height=height+h;
 float left00=x0?mxb_nh_sample(g,x0-1,z0,factor):h00,left01=x0?mxb_nh_sample(g,x0-1,z1,factor):h01;
 float right10=x1<g->width-1?mxb_nh_sample(g,x1+1,z0,factor):h10,right11=x1<g->width-1?mxb_nh_sample(g,x1+1,z1,factor):h11;
 float up00=z0?mxb_nh_sample(g,x0,z0-1,factor):h00,up10=z0?mxb_nh_sample(g,x1,z0-1,factor):h10;
 float down01=z1<g->height-1?mxb_nh_sample(g,x0,z1+1,factor):h01,down11=z1<g->height-1?mxb_nh_sample(g,x1,z1+1,factor):h11;
 float a=h10-left00,b=right10-h00,c=h11-left01,d=right11-h01;
 a=a*w00;b=b*w10;c=c*w01;d=d*w11;float dx=a+b;dx=dx+c;dx=dx+d;
 float tangent_x[3]={g->extent_x/(float)(g->width-1),dx,0};tangent_x[0]=tangent_x[0]*2.f;
 if(!mxb_nh_normalize(tangent_x))return 0;
 a=h11-up10;b=h01-up00;c=down01-h00;d=down11-h10;
 a=a*w10;b=b*w00;c=c*w01;d=d*w11;float dz=a+b;dz=dz+c;dz=dz+d;
 float tangent_z[3]={0,dz,g->extent_z/(float)(g->height-1)};tangent_z[2]=tangent_z[2]*2.f;
 if(!mxb_nh_normalize(tangent_z))return 0;
 MxbNativeHeightPoint result;result.normal[0]=tangent_x[2]*tangent_z[1]-tangent_x[1]*tangent_z[2];
 result.normal[1]=tangent_x[0]*tangent_z[2]-tangent_z[0]*tangent_x[2];
 result.normal[2]=tangent_z[0]*tangent_x[1]-tangent_x[0]*tangent_z[1];
 float bias=factor*32768.f;bias=bias+g->origin[1];result.height=bias+height;
 if(!isfinite(result.height)||!isfinite(result.normal[0])||!isfinite(result.normal[1])||!isfinite(result.normal[2]))return 0;
 *out=result;return 1;
}
