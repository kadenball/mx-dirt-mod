#ifndef MXB_TERRAIN_PACING_H
#define MXB_TERRAIN_PACING_H
/* Source-only x64 Windows adapter. This file never allocates executable memory,
   patches an image, swaps callback pointers, or writes terrain. The integrator
   owns quiescent install/removal, pinned lifetime, thread and event generation.
   Both halves are timing-sensitive; change them only with in-game measurements. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

enum { MXB_PACING_CLIENT=0, MXB_PACING_HOST=1,
       MXB_PACE_ENABLED=1, MXB_PACE_OFFLINE_TESTING=2, MXB_PACE_SOIL_LAB=4,
       MXB_PACE_STREAMS_VALID=8, MXB_PACE_LIFECYCLE_READY=16,
       MXB_PACE_REQUIRED=31 };
typedef int (*MxbPacingSpan)(const void *,size_t,int,void *);
typedef int (*MxbPacingDirty)(int,unsigned,unsigned char **,const int32_t *);
typedef struct { volatile LONG bits; DWORD owner_thread; uint64_t generation; } MxbPacingGate;
typedef struct {
 unsigned char *exe; size_t image_bytes; int verified_executable;
 MxbPacingGate *gate; uint64_t generation;
 MxbPacingSpan span; void *span_user; MxbPacingDirty wrapper[2];
} MxbPacingContext;
typedef struct {
 uintptr_t patch_address,stub_address,resume_address,counter_address;
 unsigned char original[6],patch[6],stub[96]; size_t stub_size;
} MxbPacingPlan;

static const uint32_t mxb_pace_site[2]={0x2a2f20,0x29d8a0};
static const uint32_t mxb_pace_counter[2]={0x5987f0,0x9e3ac0};
static const uint32_t mxb_pace_period[2]={0x598804,0xa1e4d8};
static const uint32_t mxb_pace_record[2]={0x5a21c8,0xa1e470};
static const uint32_t mxb_pace_return[2]={0x2a2f7b,0x29d8fb};
static const unsigned char mxb_pace_sig[2][21]={
 {0x8b,0x05,0xca,0x58,0x2f,0,0x99,0xf7,0x3d,0xd7,0x58,0x2f,0,0x3b,0xd3,0x0f,0x85,0xa5,3,0,0},
 {0x8b,0x05,0x1a,0x62,0x74,0,0x99,0xf7,0x3d,0x2b,0x0c,0x78,0,0x3b,0xd7,0x0f,0x85,0xba,5,0,0}};
static const unsigned char mxb_pace_dirty_sig[38]={
 0xff,0xc9,0x83,0xf9,2,0x77,0x19,0x48,0x63,0xc9,0x48,0x8d,5,0x37,0xc3,0x3a,0,
 0x48,0x69,0xc9,0x60,0x61,0,0,0x48,3,0xc8,0xe9,0x10,0x4a,3,0,0xb8,1,0,0,0,0xc3};
static inline uint32_t mxb_pace_u32(const void *p){uint32_t v;memcpy(&v,p,4);return v;}
static inline uintptr_t mxb_pace_ptr(const void *p){uintptr_t v;memcpy(&v,p,8);return v;}
static inline int mxb_pace_span(const MxbPacingContext *c,const void *p,size_t n,int w){
 uintptr_t a=(uintptr_t)p;return c&&c->span&&p&&n&&a<=UINTPTR_MAX-(n-1)&&c->span(p,n,w,c->span_user);
}
static inline int mxb_pace_context(const MxbPacingContext *c){
 return c&&c->exe&&c->verified_executable&&c->image_bytes>=0xa1e4dc&&
  (uintptr_t)c->exe<=UINTPTR_MAX-c->image_bytes&&
  mxb_pace_span(c,c->gate,sizeof(*c->gate),1)&&!((uintptr_t)c->gate&3)&&
  c->generation==c->gate->generation&&c->generation!=0;
}
/* Validates live stream metadata and full spans. Call only on the owning thread
   or under lifecycle quiescence. Pointer validity checks do not create ownership. */
static inline int mxb_pace_binding(const MxbPacingContext *c,int side,uint32_t *tiles){
 if(side<0||side>1||!mxb_pace_context(c))return 0;
 unsigned char *r=c->exe+mxb_pace_record[side],*g=c->exe+0x568488+side*0x6160;
 if(!mxb_pace_span(c,r,0x68,0)||!mxb_pace_span(c,g,0x800,0)||
    !mxb_pace_span(c,c->exe+0x1bc140,38,0)||
    memcmp(c->exe+0x1bc140,mxb_pace_dirty_sig,38)||
    !mxb_pace_span(c,c->exe+mxb_pace_period[side],4,0)||
    !mxb_pace_u32(c->exe+mxb_pace_period[side])||
    mxb_pace_u32(c->exe+mxb_pace_period[side])>INT32_MAX)return 0;
 uint32_t w=mxb_pace_u32(g+0x740),h=mxb_pace_u32(g+0x744);
 if(w<2||h<2||w>8193||h>8193)return 0;
 uint32_t tx=(w+63)/64,ty=(h+63)/64,n=tx*ty;
 const uint32_t want[8]={w,h,4,64,64,tx,ty,n};
 for(unsigned i=0;i<8;i++)if(mxb_pace_u32(r+8+i*4)!=want[i])return 0;
 if(mxb_pace_u32(g+0x790)!=64||mxb_pace_u32(g+0x794)!=64||
    mxb_pace_u32(g+0x798)!=tx||mxb_pace_u32(g+0x79c)!=ty||mxb_pace_u32(g+0x7a0)!=n)return 0;
 unsigned cb=side?0x58:0x48,fo=side?0x7b0:0x7d0,so=side?0x7a8:0x7c8;
 uintptr_t src=mxb_pace_ptr(g+so),flags=mxb_pace_ptr(g+fo),snap=mxb_pace_ptr(g+fo+8),fn=mxb_pace_ptr(r+cb);
 if(mxb_pace_ptr(r)!=src||mxb_pace_u32(r+cb+8)!=(uint32_t)(side+1)||
    mxb_pace_u32(r+cb+12)!=(side?4u:0x104u)||
    !fn||(fn!=(uintptr_t)(c->exe+0x1bc140)&&fn!=(uintptr_t)c->wrapper[side])||
    !mxb_pace_span(c,(void*)src,(size_t)w*h*4,!side)||
    !mxb_pace_span(c,(void*)flags,n,1)||!mxb_pace_span(c,(void*)snap,n,1)||flags==snap)return 0;
 if(tiles)*tiles=n;return 1;
}
/* Builds bytes only. The patch is E9 rel32 + NOP, displacing exactly6 bytes.
   The stub preserves RFLAGS and every register except EAX, the original output.
   Active iff bits exactly31 AND stream offset is1a0; otherwise exact native load.
   Gate/counter/resume addresses are absolute64, so the stub has no hidden RIP fixups. */
static inline int mxb_pace_build(const MxbPacingContext *c,int side,uintptr_t stub_address,MxbPacingPlan *out){
 uint32_t n;if(!out||side<0||side>1||!stub_address||!mxb_pace_binding(c,0,&n)||!mxb_pace_binding(c,1,&n))return 0;
 const unsigned char *site=c->exe+mxb_pace_site[side];
 if(!mxb_pace_span(c,site,21,0)||memcmp(site,mxb_pace_sig[side],21))return 0;
 uintptr_t from=(uintptr_t)site+5;int64_t rel;
 if(stub_address>=from){if(stub_address-from>INT32_MAX)return 0;rel=(int64_t)(stub_address-from);}
 else {if(from-stub_address>(uint64_t)INT32_MAX+1)return 0;rel=-(int64_t)(from-stub_address);}
 MxbPacingPlan p;memset(&p,0,sizeof(p));p.patch_address=(uintptr_t)site;p.stub_address=stub_address;
 p.resume_address=(uintptr_t)site+6;p.counter_address=(uintptr_t)c->exe+mxb_pace_counter[side];
 memcpy(p.original,site,6);p.patch[0]=0xe9;int32_t d=(int32_t)rel;memcpy(p.patch+1,&d,4);p.patch[5]=0x90;
 unsigned char *s=p.stub;size_t z=0;s[z++]=0x9c; /* pushfq */
 s[z++]=0x48;s[z++]=0xb8;uintptr_t a=(uintptr_t)&c->gate->bits;memcpy(s+z,&a,8);z+=8;
 s[z++]=0x83;s[z++]=0x38;s[z++]=MXB_PACE_REQUIRED; /* cmp dword [rax],31 */
 s[z++]=0x75;size_t j1=z++;s[z++]=0x48;s[z++]=0x81;s[z++]=(unsigned char)(side?0xfb:0xfd);
 uint32_t stream=0x1a0;memcpy(s+z,&stream,4);z+=4; /* cmp rbx/rbp,1a0 */
 s[z++]=0x75;size_t j2=z++;s[z++]=0x31;s[z++]=0xc0;s[z++]=0xeb;size_t jt=z++;
 size_t native=z;s[z++]=0x48;s[z++]=0xb8;a=p.counter_address;memcpy(s+z,&a,8);z+=8;
 s[z++]=0x8b;s[z++]=0x00; /* mov eax,[rax] */
 size_t tail=z;s[z++]=0x9d;s[z++]=0xff;s[z++]=0x25;memset(s+z,0,4);z+=4;
 a=p.resume_address;memcpy(s+z,&a,8);z+=8;
 s[j1]=(unsigned char)(native-j1-1);s[j2]=(unsigned char)(native-j2-1);s[jt]=(unsigned char)(tail-jt-1);
 p.stub_size=z;*out=p;return 1;
}
/* native_return_slot MUST be _AddressOfReturnAddress() in the actual DirtyFn
   wrapper (not in a nested helper). Bounds come from current thread's TEB stack.
   No pointer from a foreign caller or local substitute range is accepted. */
static inline int mxb_pace_widen(MxbPacingContext *c,int side,int id,unsigned mode,
 const int32_t *range,const void *native_return_slot,uintptr_t stack_low,uintptr_t stack_high,DWORD thread){
 if(!c||!mxb_pace_span(c,c->gate,sizeof(*c->gate),1)||((uintptr_t)c->gate&3)||
    InterlockedCompareExchange(&c->gate->bits,0,0)!=MXB_PACE_REQUIRED)return 0;
 if(!mxb_pace_context(c)){InterlockedExchange(&c->gate->bits,0);return 0;}
 uint32_t n=0;uintptr_t a=(uintptr_t)native_return_slot,delta=side?0x50:0x60;
 int ok=side>=0&&side<=1&&thread==c->gate->owner_thread&&id==side+1&&mode==(side?4u:0x104u)&&
  a>=stack_low&&a<=UINTPTR_MAX-delta-8&&a+delta+8<=stack_high&&
  (uintptr_t)range==a+delta&&mxb_pace_span(c,native_return_slot,8,0)&&
  mxb_pace_ptr(native_return_slot)==(uintptr_t)c->exe+mxb_pace_return[side]&&
  mxb_pace_span(c,range,8,1)&&mxb_pace_binding(c,side,&n)&&c->wrapper[side]&&
  mxb_pace_ptr(c->exe+mxb_pace_record[side]+(side?0x58:0x48))==(uintptr_t)c->wrapper[side];
 if(ok){int32_t before[2];memcpy(before,range,8);ok=before[0]>=0&&before[1]>=before[0]&&(uint32_t)before[1]<=n;}
 if(!ok){InterlockedExchange(&c->gate->bits,0);return 0;}
 int32_t full[2]={0,(int32_t)n};memcpy((void*)range,full,8);return 1;
}
static inline int mxb_pace_invoke_dirty(MxbPacingContext *c,int side,MxbPacingDirty original,
 int id,unsigned mode,unsigned char **out,const int32_t *range,const void *return_slot,
 uintptr_t stack_low,uintptr_t stack_high,DWORD thread){
 DWORD saved=GetLastError();mxb_pace_widen(c,side,id,mode,range,return_slot,stack_low,stack_high,thread);
 SetLastError(saved);return original(id,mode,out,range); /* exactly once; preserve original last-error */
}
#endif
