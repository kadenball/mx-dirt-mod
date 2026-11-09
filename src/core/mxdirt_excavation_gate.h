#ifndef MXB_EXCAVATION_GATE_H
#define MXB_EXCAVATION_GATE_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

/* The source-only gate replaces the ONE MX Bikes tyre excavation call, not
   wheel forces. It prevents adding native excavation to the dirt-sim field.
   Quiescent install/remove, signature reads, near relay and pinned code are
   integrator duties. Unknown writers remain detectable by publication ledger.
   ABI at1a4d8f: RCX owner, XMM1 worldX, XMM2 worldZ, XMM3 double amount. */
typedef int (*MxbNativeExcavation)(void *,float,float,double);
typedef struct {
    volatile LONG exclusive, faulted;
    DWORD owner_thread;
    uint64_t generation, suppressed;
    void *terrain_owner[2];
    MxbNativeExcavation original;
} MxbExcavationGate;
typedef struct {
    uintptr_t site,original_target,replacement;
    unsigned char original[5],patch[5];
} MxbExcavationCallPlan;

/* Caller has already established readable byte snapshots and image identity.
   This creates bytes only. Native success/no-op return0 is proven at1f5ed6;
   suppressing this call does not skip any wheel force application. */
static inline int mxb_excavation_call_plan(uintptr_t base,uintptr_t relay,
    int verified_executable,const unsigned char call[5],
    const unsigned char wrapper[12],MxbExcavationCallPlan *out) {
    static const unsigned char cs[5]={0xe8,0x0c,0x70,0x01,0};
    static const unsigned char ws[12]={0x48,0x81,0xc1,0xe8,0x0c,0,0,0xe9,0x14,0x9d,3,0};
    if(!base||!relay||!out||!call||!wrapper||!verified_executable||
       base>UINTPTR_MAX-0x1bbda0||memcmp(call,cs,5)||memcmp(wrapper,ws,12))return 0;
    uintptr_t site=base+0x1a4d8f,from=site+5;int64_t distance;
    if(relay>=from){if(relay-from>INT32_MAX)return 0;distance=(int64_t)(relay-from);}
    else {if(from-relay>(uint64_t)INT32_MAX+1)return 0;distance=-(int64_t)(from-relay);}
    MxbExcavationCallPlan p;memset(&p,0,sizeof(p));p.site=site;
    p.original_target=base+0x1bbda0;p.replacement=relay;memcpy(p.original,cs,5);
    p.patch[0]=0xe8;int32_t offset=(int32_t)distance;memcpy(p.patch+1,&offset,4);*out=p;return 1;
}

/* A fresh gate is zero-initialized. Binds canonical client/host owner addresses
   from this executable's G=owner+ce8 layout. No native code is modified here. */
static inline int mxb_excavation_gate_init(MxbExcavationGate *g,uintptr_t base,
    uint64_t generation,DWORD thread,MxbNativeExcavation original) {
    if(!g||!base||base>UINTPTR_MAX-0x56d900||!generation||!thread||!original)return 0;
    memset(g,0,sizeof(*g));g->generation=generation;g->owner_thread=thread;g->original=original;
    g->terrain_owner[0]=(void*)(base+0x568488-0xce8);
    g->terrain_owner[1]=(void*)(base+0x568488+0x6160-0xce8);return 1;
}
/* All external facts are explicit. Set exclusive BEFORE activating a ledger.
   Original pending work must already have drained at the same quiescent point.
   This gate cannot infer offline authorization from an address or material ID. */
static inline int mxb_excavation_gate_activate(MxbExcavationGate *g,uint64_t generation,
    DWORD thread,int offline_soil_lab,int installed_and_quiescent,int native_pending_drained) {
    if(!g||!g->original||generation!=g->generation||thread!=g->owner_thread||
       !offline_soil_lab||!installed_and_quiescent||!native_pending_drained||g->faulted)return 0;
    InterlockedExchange(&g->exclusive,1);return 1;
}
/* Pause and runtime faults are NOT reasons to restore the other writer.
   Restore only after coherent drain, or retirement of the old track/transport,
   and quiescence. A fault stays suppressed until this explicit reset boundary. */
static inline int mxb_excavation_gate_release(MxbExcavationGate *g,uint64_t generation,
    DWORD thread,int quiescent,int drained_or_old_track_retired) {
    if(!g||generation!=g->generation||thread!=g->owner_thread||
       !quiescent||!drained_or_old_track_retired)return 0;
    InterlockedExchange(&g->exclusive,0);return 1;
}
static inline int mxb_excavation_invoke(MxbExcavationGate *g,void *owner,
    float x,float z,double amount,DWORD thread) {
    /* g/original have a pinned lifetime for every installed native call. */
    if(InterlockedCompareExchange(&g->exclusive,0,0)&&
       (owner==g->terrain_owner[0]||owner==g->terrain_owner[1])) {
        if(thread!=g->owner_thread)InterlockedExchange(&g->faulted,1);
        else ++g->suppressed;
        return 0;
    }
    return g->original(owner,x,z,amount);
}
#endif
