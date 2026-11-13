#ifndef MXDIRT_FAST_READ_H
#define MXDIRT_FAST_READ_H
/* Checked reads of game memory without two system calls per read.
   The render plugin used VirtualQuery + ReadProcessMemory for every read of
   native memory: ~15-30 us each, ~19 of them per frame, plus a 1 MB copy of the
   height band - about half a millisecond of every frame.
   Here a range is checked once with VirtualQuery (committed, readable, no
   guard), remembered, and then read with a plain copy. The owner drops the
   memory (mxd_fr_expire) every MXD_FR_FRAMES frames and at every lifecycle
   change, so a range is never trusted for longer than that.
   Every range read this way is one the game itself has just used on this
   thread: the terrain descriptor and model handed to the native terrain draw,
   and the two height grids that descriptor points at. Not thread-safe: one
   instance per owning thread. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
enum { MXD_FR_RANGES=8, MXD_FR_FRAMES=30 };
typedef struct {uintptr_t first[MXD_FR_RANGES],end[MXD_FR_RANGES];unsigned next;} MxdFastRead;
static inline void mxd_fr_expire(MxdFastRead *c){memset(c,0,sizeof(*c));}
/* 1 when [p,p+n) is committed readable memory, checked now or remembered. */
static inline int mxd_fr_check(MxdFastRead *c,uintptr_t p,size_t n){
 if(!c||!p||!n||n>UINTPTR_MAX-p)return 0;
 const uintptr_t end=p+n;
 for(unsigned k=0;k<MXD_FR_RANGES;k++)if(p>=c->first[k]&&end<=c->end[k])return 1;
 uintptr_t first=0,at=p;
 while(at<end){MEMORY_BASIC_INFORMATION m;
  if(!VirtualQuery((void*)at,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||
     !(m.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return 0;
  if(!first)first=(uintptr_t)m.BaseAddress;
  uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=at)return 0;at=next;
 }
 const unsigned k=c->next++%MXD_FR_RANGES;c->first[k]=first;c->end[k]=at;return 1;
}
static inline int mxd_fr_read(MxdFastRead *c,uintptr_t p,void *out,size_t n){
 if(!mxd_fr_check(c,p,n))return 0;memcpy(out,(const void*)p,n);return 1;
}
#endif
