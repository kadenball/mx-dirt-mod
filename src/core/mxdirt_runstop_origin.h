#ifndef MXB_RUNSTOP_ORIGIN_H
#define MXB_RUNSTOP_ORIGIN_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Read-only provenance for ONE ordinary-pause callback in the pinned Win64
   executable. R must be _AddressOfReturnAddress() evaluated in the actual
   exported RunStop, never in a helper. The host provides current-thread stack
   bounds, serialized ownership and its independently verified EXE digest.
   This does not prove stream convergence, drained transport, or retirement.
   All existing complete terrain/seed checks remain required after a match.
   See mxb-pause-provenance-evidence.json for native prolog/unwind derivation. */
typedef int (*MxbRunStopRead)(void *,uint64_t,void *,size_t);
typedef struct {
    uint64_t image_base,image_bytes,stack_low,stack_high,return_slot;
    const char *verified_executable_sha256;
} MxbRunStopOriginRequest;
enum {
    MXB_RUNSTOP_ORDINARY_PAUSE=1,
    MXB_RUNSTOP_INVALID=-1,MXB_RUNSTOP_BUILD=-2,MXB_RUNSTOP_STACK=-3,
    MXB_RUNSTOP_READ=-4,MXB_RUNSTOP_CALLER=-5,MXB_RUNSTOP_RUNNING=-6
};
typedef struct {uint32_t rva,size;uint64_t fnv;} MxbRunStopCodeSpan;
#include "mxdirt_runstop_origin_pins.inc"

static inline int mxb_runstop_pause_origin(MxbRunStopRead read,void *context,
                                          const MxbRunStopOriginRequest *r) {
    if(!read||!r||!r->image_base||r->image_bytes<0x9e4924||
       r->image_bytes>UINT64_MAX-r->image_base||!r->verified_executable_sha256)return MXB_RUNSTOP_INVALID;
    if(strcmp(r->verified_executable_sha256,
       "9e792d7aed1dc40df11eea51d1a6d0519daec7be2b65609905df8c9de804a291"))return MXB_RUNSTOP_BUILD;
    uint64_t slot=r->return_slot;
    if(!slot||(slot&7)||!r->stack_low||r->stack_low>=r->stack_high||
       slot<r->stack_low||slot>=r->stack_high||r->stack_high-slot<0x30c)return MXB_RUNSTOP_STACK;
    uint64_t sdk,api,parent;uint32_t command;
    if(read(context,slot,&sdk,sizeof(sdk))!=1)return MXB_RUNSTOP_READ;
    if(sdk!=r->image_base+0x12ac53)return MXB_RUNSTOP_CALLER;
    if(read(context,slot+0x30,&api,sizeof(api))!=1)return MXB_RUNSTOP_READ;
    if(api!=r->image_base+0x127e40)return MXB_RUNSTOP_CALLER;
    if(read(context,slot+0x300,&parent,sizeof(parent))!=1||
       read(context,slot+0x308,&command,sizeof(command))!=1)return MXB_RUNSTOP_READ;
    if(parent!=r->image_base+0xf23ca||command!=0x3b4)return MXB_RUNSTOP_CALLER;
    unsigned char bytes[512];
    for(size_t j=0;j<sizeof(mxb_runstop_code_spans)/sizeof(*mxb_runstop_code_spans);j++){
        const MxbRunStopCodeSpan *p=&mxb_runstop_code_spans[j];
        if(!p->size||p->size>sizeof(bytes)||p->rva>=r->image_bytes||p->size>r->image_bytes-p->rva)return MXB_RUNSTOP_BUILD;
        if(read(context,r->image_base+p->rva,bytes,p->size)!=1)return MXB_RUNSTOP_READ;
        uint64_t hash=UINT64_C(14695981039346656037);
        for(uint32_t k=0;k<p->size;k++)hash=(hash^bytes[k])*UINT64_C(1099511628211);
        if(hash!=p->fnv)return MXB_RUNSTOP_BUILD;
    }
    int32_t paused,host,mode;uint64_t api_pointer;
    if(read(context,r->image_base+0x566c48,&api_pointer,sizeof(api_pointer))!=1||
       read(context,r->image_base+0x3d7fac,&paused,sizeof(paused))!=1||
       read(context,r->image_base+0x9d7aa4,&host,sizeof(host))!=1||
       read(context,r->image_base+0x9e4920,&mode,sizeof(mode))!=1)return MXB_RUNSTOP_READ;
    if(api_pointer!=r->image_base+0x120cc0)return MXB_RUNSTOP_BUILD;
    return paused==1&&host&&mode==1?MXB_RUNSTOP_ORDINARY_PAUSE:MXB_RUNSTOP_RUNNING;
}
#endif
