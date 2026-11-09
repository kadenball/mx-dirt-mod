#ifndef MXDIRT_HEIGHT_LOOKUP_H
#define MXDIRT_HEIGHT_LOOKUP_H
#include <math.h>
#include <stdint.h>
#include <stddef.h>

/* Bilinear lookup of the dirt simulation's height texture (encoded 0..1, one
   float per texel) at a terrain position. World X/Z and spans are in the same
   native units, centred on the terrain; texture rows run with -Z. The mapping
   uses (texels-1) per span, so terrain edges land on edge texels. A position
   whose lower/upper texel falls on the border is reported as "outside" (the
   caller keeps the saved deformation there) and its corners are clamped.
   Compile without fast-math/FMA contraction so results are reproducible. */
typedef struct {
    float corners[4];   /* x0,z0,x1,z1 texel indices */
    float fractions[4]; /* x,z interpolation fractions (2,3 unused) */
} MxdirtHeightFootprint;

static inline int mxdirt_height_prepare(float world_x,float world_z,
        float span_x,float span_z,uint32_t width,uint32_t height,
        MxdirtHeightFootprint *out) {
    const float position[4]={world_x,-world_z,world_x,-world_z};
    const float spans[4]={span_x,span_z,span_x,span_z};
    const float limits[4]={(float)width-1.f,(float)height-1.f,
                          (float)width-1.f,(float)height-1.f};
    int inside=1;
    for (int k=0;k<4;++k) {
        float q=((spans[k]*.5f+position[k])*limits[k])/spans[k];
        float corner=floorf(q+(k<2 ? 0.f : 1.f));
        out->fractions[k]=q-corner;
        out->corners[k]=corner;
        if (!(corner>0.f && corner<limits[k])) inside=0;
    }
    if (!inside) for (int k=0;k<4;++k)
        out->corners[k]=fminf(limits[k],fmaxf(0.f,out->corners[k]));
    return inside;
}

/* stride is in floats. Interpolates along X first, then Z. */
static inline float mxdirt_height_sample(const float *pixels,size_t stride,
                                         const MxdirtHeightFootprint *f) {
    float row0=f->corners[1]*(float)stride,row1=f->corners[3]*(float)stride;
    size_t i00=(size_t)floorf(row0+f->corners[0]),i10=(size_t)floorf(row0+f->corners[2]);
    size_t i01=(size_t)floorf(row1+f->corners[0]),i11=(size_t)floorf(row1+f->corners[2]);
    float a=pixels[i00],b=pixels[i10],c=pixels[i01],d=pixels[i11];
    float top=(b-a)*f->fractions[0]+a;
    float bottom=(d-c)*f->fractions[0]+c;
    return (bottom-top)*f->fractions[1]+top;
}

/* Encoded 0..1 -> signed native height units; .5 is the zero plane. */
static inline float mxdirt_height_decode(float encoded,float scale) {
    return ((encoded-.5f)*2.f)*scale;
}
#endif
