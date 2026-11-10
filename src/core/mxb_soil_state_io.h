#ifndef MXB_SOIL_STATE_IO_H
#define MXB_SOIL_STATE_IO_H
/* The dirt sim's soil state saved with the track (MX Bikes' DFM holds only heights):
   packing, loose soil, moisture offsets, surface class, roost clods and tyre prints,
   so a ridden track reloads ridden-in instead of fresh soil in the old shape.
   "MXSS", version 1, size, saved wear (DFM vertices deeper/higher than 1.5 cm when it
   was written), tile count, then per non-empty 128x32 tile: uint32 index, loose
   float[4096], comp, mat, moist_off, clods, print, print_dir, print_off [4096 each].
   Read only onto the same size and while the DFM's wear still matches (3 %, at least
   300 vertices), so a reset or swapped track never gets someone else's soil. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mxb_soil_sim.h"
#define MXSS_TW 128
#define MXSS_TH 32
static inline int mxb_soil_state_tile_used(const MxbSoilSim *m,uint32_t tx,uint32_t ty){
    for(uint32_t r=ty*MXSS_TH;r<(ty+1)*MXSS_TH;r++){size_t k=(size_t)r*m->size+tx*MXSS_TW;
        for(uint32_t q=0;q<MXSS_TW;q++,k++)if(m->comp[k]|m->mat[k]|m->clods[k]|m->print[k]|(uint8_t)m->moist_off[k]||m->loose[k]!=0.f)return 1;}
    return 0;
}
static inline int mxb_soil_state_write(const MxbSoilSim *m,FILE *f,uint64_t wear){
    if(!m||!f)return 0;uint32_t nx=m->size/MXSS_TW,ny=m->size/MXSS_TH,count=0,ver=1,size=m->size;
    for(uint32_t ty=0;ty<ny;ty++)for(uint32_t tx=0;tx<nx;tx++)count+=(uint32_t)mxb_soil_state_tile_used(m,tx,ty);
    int ok=fwrite("MXSS",1,4,f)==4&&fwrite(&ver,4,1,f)==1&&fwrite(&size,4,1,f)==1&&fwrite(&wear,8,1,f)==1&&fwrite(&count,4,1,f)==1;
    uint32_t written=0;
    for(uint32_t ty=0;ty<ny&&ok;ty++)for(uint32_t tx=0;tx<nx&&ok;tx++){
        if(!mxb_soil_state_tile_used(m,tx,ty))continue;uint32_t idx=ty*nx+tx;ok=fwrite(&idx,4,1,f)==1;
        for(uint32_t r=ty*MXSS_TH;r<(ty+1)*MXSS_TH&&ok;r++)ok=fwrite(m->loose+(size_t)r*m->size+tx*MXSS_TW,4,MXSS_TW,f)==MXSS_TW;
        const uint8_t *arr[7]={m->comp,m->mat,(const uint8_t*)m->moist_off,m->clods,m->print,m->print_dir,(const uint8_t*)m->print_off};
        for(int a=0;a<7&&ok;a++)for(uint32_t r=ty*MXSS_TH;r<(ty+1)*MXSS_TH&&ok;r++)ok=fwrite(arr[a]+(size_t)r*m->size+tx*MXSS_TW,1,MXSS_TW,f)==MXSS_TW;
        written++;}
    return ok&&written==count;
}
/* Into a sim whose soil state is still empty. 1 = loaded (*tiles = tiles read), 0 = not this
   track / bad file (state left empty). wear: the loaded DFM's wear now. */
static inline int mxb_soil_state_read(MxbSoilSim *m,FILE *f,uint64_t wear,uint32_t *tiles){
    if(tiles)*tiles=0;if(!m||!f)return 0;
    char mg[4];uint32_t ver=0,size=0,count=0;uint64_t sw=0;
    int ok=fread(mg,1,4,f)==4&&!memcmp(mg,"MXSS",4)&&fread(&ver,4,1,f)==1&&ver==1&&fread(&size,4,1,f)==1&&size==m->size&&fread(&sw,8,1,f)==1&&fread(&count,4,1,f)==1;
    uint32_t nx=m->size/MXSS_TW,ny=m->size/MXSS_TH;
    if(ok){uint64_t d=sw>wear?sw-wear:wear-sw,tol=sw/33;if(tol<300)tol=300;if(!wear||d>tol)ok=0;} /* the DFM must still be the track this soil belongs to */
    if(!ok||count>nx*ny)return 0;
    const size_t tb=(size_t)MXSS_TW*MXSS_TH;float *lb=malloc(tb*4);uint8_t *bb=malloc(tb*7);if(!lb||!bb){free(lb);free(bb);return 0;}
    /* check the whole file first, apply only if it is all good */
    long start=ftell(f);
    for(uint32_t i=0;i<count&&ok;i++){uint32_t id;ok=fread(&id,4,1,f)==1&&id<nx*ny&&fread(lb,4,tb,f)==tb&&fread(bb,1,tb*7,f)==tb*7;
        for(size_t j=0;j<tb&&ok;j++)if(!isfinite(lb[j])||lb[j]<0.f||lb[j]>1.f)ok=0;}
    if(ok&&start>=0&&fseek(f,start,SEEK_SET)==0){
        uint8_t *arr[7]={m->comp,m->mat,(uint8_t*)m->moist_off,m->clods,m->print,m->print_dir,(uint8_t*)m->print_off};
        for(uint32_t i=0;i<count&&ok;i++){uint32_t id;ok=fread(&id,4,1,f)==1&&fread(lb,4,tb,f)==tb&&fread(bb,1,tb*7,f)==tb*7;if(!ok)break;
            uint32_t tx=id%nx,ty=id/nx;
            for(uint32_t r=0;r<MXSS_TH;r++){size_t k=(size_t)(ty*MXSS_TH+r)*m->size+tx*MXSS_TW;memcpy(m->loose+k,lb+(size_t)r*MXSS_TW,MXSS_TW*4);
                for(int a=0;a<7;a++)memcpy(arr[a]+k,bb+(size_t)a*tb+(size_t)r*MXSS_TW,MXSS_TW);}
            m->tiles[ty*nx+tx]=1;}}
    else ok=0;
    free(lb);free(bb);
    if(!ok){size_t n=(size_t)m->size*m->size;memset(m->loose,0,n*4);memset(m->comp,0,n);memset(m->mat,0,n);memset(m->moist_off,0,n);memset(m->clods,0,n);
        memset(m->print,0,n);memset(m->print_dir,0,n);memset(m->print_off,0,n);memset(m->tiles,0,(size_t)m->tiles_x*m->tiles_y);return 0;}
    if(tiles)*tiles=count;return 1;
}
#endif
