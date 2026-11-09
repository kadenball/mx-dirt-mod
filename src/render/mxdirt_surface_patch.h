#ifndef MXDIRT_SURFACE_PATCH_H
#define MXDIRT_SURFACE_PATCH_H
/* Raised-dirt surface fix for MX Bikes layer exposure (function at 2600c0).
   Native deformation is dig-only and read as an UNSIGNED uint16 depth; raised
   samples (berms, roost) wrap to a huge depth, so the exposure masks show the
   bottom layer. Site 260355 "mov eax,ecx (8b c1); divss xmm4,xmm0 (f3 0f 5e e0)"
   becomes "jmp relay"; the relay runs
       movsx eax,cx ; test eax,eax ; jns +2 ; xor eax,eax ; divss xmm4,xmm0
   then jumps back to 26035b. Raised ground -> depth 0 -> surface layer kept.
   Heights (26034e) are untouched. [renderer] raised_dirt_surface=0 disables. */
#include <stdint.h>
#include <string.h>
#define MXDIRT_SURFACE_SITE 0x260355u
#define MXDIRT_SURFACE_RETURN 0x26035bu
#define MXDIRT_SURFACE_RELAY_BYTES 18u
static const unsigned char mxdirt_surface_original[5]={0x8b,0xc1,0xf3,0x0f,0x5e};
static const unsigned char mxdirt_surface_body[13]={0x0f,0xbf,0xc1,0x85,0xc0,0x79,0x02,0x31,0xc0,0xf3,0x0f,0x5e,0xe0};
/* Fills relay[18] (at address relay) and site_patch[5] for an image at base.
   Returns 0 when either rel32 does not reach. */
static inline int mxdirt_surface_build(uintptr_t base,uintptr_t relay,unsigned char out_relay[18],unsigned char site_patch[5]){
    int64_t back=(int64_t)(base+MXDIRT_SURFACE_RETURN)-(int64_t)(relay+MXDIRT_SURFACE_RELAY_BYTES);
    int64_t there=(int64_t)relay-(int64_t)(base+MXDIRT_SURFACE_SITE+5);
    if(back<=INT32_MIN||back>=INT32_MAX||there<=INT32_MIN||there>=INT32_MAX)return 0;
    int32_t b=(int32_t)back,t=(int32_t)there;
    memcpy(out_relay,mxdirt_surface_body,13);out_relay[13]=0xe9;memcpy(out_relay+14,&b,4);
    site_patch[0]=0xe9;memcpy(site_patch+1,&t,4);return 1;
}
#endif
