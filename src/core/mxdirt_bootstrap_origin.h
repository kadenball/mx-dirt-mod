#ifndef MXB_BOOTSTRAP_ORIGIN_H
#define MXB_BOOTSTRAP_ORIGIN_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Read-only exact native receive-return ancestry. The integrator supplies the
   actual wrapper's _AddressOfReturnAddress and current TIB stack bounds.
   This is a potential serialized pre-physics gap, NEVER a convergence receipt.
   Native source arrays still require every complete transport creation check. */
typedef int (*MxbBootstrapRead)(void *,uint64_t,void *,size_t);
typedef struct {uint32_t rva,size;uint64_t fnv;} MxbBootstrapCodeSpan;
#include "mxdirt_bootstrap_origin_pins.inc"
typedef struct {
 uint64_t image,image_bytes,stack_low,stack_high,return_slot;
 const char *verified_sha256;const unsigned char *owned_receive_call;
} MxbBootstrapOrigin;
static inline int mxb_bootstrap_code(MxbBootstrapRead read,void *u,const MxbBootstrapOrigin *q){
 if(!read||!q||!q->image||q->image_bytes<0xa1e4dc||q->image_bytes>UINT64_MAX-q->image||
  !q->verified_sha256||strcmp(q->verified_sha256,"9e792d7aed1dc40df11eea51d1a6d0519daec7be2b65609905df8c9de804a291"))return 0;
 unsigned char b[1536];
 for(size_t j=0;j<sizeof(mxb_bootstrap_code_spans)/sizeof(*mxb_bootstrap_code_spans);j++){
  const MxbBootstrapCodeSpan *p=&mxb_bootstrap_code_spans[j];
  if(p->size>sizeof(b)||p->rva>=q->image_bytes||p->size>q->image_bytes-p->rva||read(u,q->image+p->rva,b,p->size)!=1)return 0;
  uint64_t h=UINT64_C(14695981039346656037);for(uint32_t k=0;k<p->size;k++)h=(h^b[k])*UINT64_C(1099511628211);
  if(h!=p->fnv)return 0;
 }
 static const unsigned char native[5]={0xe8,0xba,0xf9,0xff,0xff};
 return read(u,q->image+0x2a6241,b,5)==1&&!memcmp(b,q->owned_receive_call?q->owned_receive_call:native,5);
}
static inline int mxb_bootstrap_immediate(MxbBootstrapRead read,void *u,uint64_t image){
 int32_t client,host,peers,pause,queued,completed,cursor,total;uint64_t api;
 if(read(u,image+0x598904,&client,4)!=1||read(u,image+0x9d7aa4,&host,4)!=1||
  read(u,image+0x9e4920,&peers,4)!=1||read(u,image+0x3d7fac,&pause,4)!=1||
  read(u,image+0x3e7164,&queued,4)!=1||read(u,image+0x3e7168,&completed,4)!=1||
  read(u,image+0x3e70f4,&cursor,4)!=1||read(u,image+0x3e7174,&total,4)!=1||
  read(u,image+0x566c48,&api,8)!=1)return 0;
 /* count is historical after completion; never demand it be reset to zero.
    No-queue and completed-positive-queue are the only accepted forms. */
 return client==7&&host>=3&&peers==1&&pause==0&&queued==0&&api==image+0x120cc0&&
  ((cursor==0&&total==0)||(total>0&&cursor>=total&&completed==1));
}
static inline int mxb_bootstrap_ancestry(MxbBootstrapRead read,void *u,const MxbBootstrapOrigin *q){
 if(!q||!q->return_slot||(q->return_slot&7)||!q->stack_low||q->stack_low>=q->stack_high||
  q->return_slot<q->stack_low||q->return_slot>=q->stack_high||q->stack_high-q->return_slot<0xa8)return 0;
 static const unsigned offsets[3]={0,0x70,0xa0},returns[3]={0x2a6246,0x2a9c02,0x135d75};
 for(unsigned j=0;j<3;j++){uint64_t v;if(read(u,q->return_slot+offsets[j],&v,8)!=1||v!=q->image+returns[j])return 0;}
 return mxb_bootstrap_code(read,u,q);
}
static inline int mxb_bootstrap_origin(MxbBootstrapRead read,void *u,const MxbBootstrapOrigin *q){
 return mxb_bootstrap_ancestry(read,u,q)&&mxb_bootstrap_immediate(read,u,q->image);
}
#endif
