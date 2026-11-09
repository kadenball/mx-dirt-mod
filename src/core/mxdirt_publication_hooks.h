#ifndef MXB_PUBLICATION_HOOKS_H
#define MXB_PUBLICATION_HOOKS_H
/* Source-only native boundary adapter. No code patch, pointer-slot install,
   process discovery or game launch. Installation/removal requires externally
   established quiescence and pinned lifetime. ALL active calls use one owner. */
#include "mxdirt_height_publication.h"
#include "mxdirt_terrain_pacing.h"

typedef struct MxbPublicationHooks MxbPublicationHooks;
/* Proven native 2a2e60 ABI: no inputs consumed, EAX=0 at its normal return. */
typedef int (*MxbNativePublisher)(void);
/* command160 call1230ed ->1bbe10 ->1f5f20; zero means successful replacement. */
typedef int (*MxbNativeReplace)(int terrain_id,const int32_t *full_grid,int tile);
typedef struct {
 uintptr_t site,original_target,replacement;
 unsigned char original[5],patch[5];
} MxbPublicationCallPlan;
typedef struct {
 MxbPacingContext pacing;
 MxbHeightPublication *ledger;
 MxbPendingView pending;
 const int32_t *native_published;
 MxbNativePublisher original_publisher;
 MxbPacingDirty original_dirty;
 MxbPublicationCallPlan call_plan;
 MxbNativeReplace original_replace;
 MxbPublicationCallPlan receive_plan;
 size_t max_batch_tiles; /* positive, <= ledger's configured native event capacity */
} MxbPublicationHookConfig;
typedef struct {
 int enabled,in_publisher,faulted;
 uint64_t publisher_calls,dirty_calls,sent_batches,backpressure_batches,echo_batches;
 MxbPubResult last_ledger_result;
 size_t last_selected_tiles,deferred_echo_tiles;
 int fail_line; /* diagnostics: source line of the first hook failure, 0 none */
} MxbPublicationHookState;

/* At quiescence, verify executable hash externally, then validate exact MX Bikes
   code and build E8 rel32 bytes for call2a37f5 only. replacement may be a pinned
   near relay to mxb_publication_publisher_entry. NO bytes are installed here. */
int mxb_publication_call_plan(const MxbPacingContext *,uintptr_t replacement,MxbPublicationCallPlan *);
int mxb_publication_receive_plan(const MxbPacingContext *,uintptr_t replacement,MxbPublicationCallPlan *);
MxbPublicationHooks *mxb_publication_hooks_create(const MxbPublicationHookConfig *);
/* Bind exactly one private context to the real ABI entry functions. Quiescent
   install/uninstall ONLY. The context, native originals and code must stay live
   until all native users have left. Unbind also refuses while either planned
   call patch or the owned dirty-slot pointer remains installed. No native
   callback slot or instruction is changed by these APIs. */
int mxb_publication_hooks_bind(MxbPublicationHooks *);
int mxb_publication_hooks_unbind(MxbPublicationHooks *);
int mxb_publication_hooks_enable(MxbPublicationHooks *,DWORD owner_thread);
/* Requires ledger already inactive/drained. A fault requires retired old track
   before unbind/destroy; never silently restore native writers while outstanding. */
int mxb_publication_hooks_disable_drained(MxbPublicationHooks *,DWORD owner_thread);
int mxb_publication_hooks_retire(MxbPublicationHooks *,int old_track_and_transport_quiescent);
int mxb_publication_hooks_destroy(MxbPublicationHooks *);
int mxb_publication_hooks_state(const MxbPublicationHooks *,MxbPublicationHookState *);
int mxb_publication_hooks_recover(MxbPublicationHooks *,DWORD owner_thread);

/* Exact ABI entrypoints for the callsite and client stream4 DirtyFn slot. */
int mxb_publication_publisher_entry(void);
int mxb_publication_dirty_entry(int id,unsigned mode,unsigned char **out,const int32_t *range);
int mxb_publication_receive_entry(int id,const int32_t *full_grid,int tile);

/* Explicit-context equivalents used by private fake-native fixtures. return_slot
   MUST come from the real entry wrapper, never a helper's own return address.
   Native originals are captured unhooked 2a2e60/1bc140 in production; fixtures
   supply equivalent private native functions. No arbitrary callback is accepted
   as evidence of native code identity by the call-plan validator. */
int mxb_publication_run(MxbPublicationHooks *,const void *return_slot,uintptr_t stack_low,uintptr_t stack_high,DWORD thread);
int mxb_publication_dirty(MxbPublicationHooks *,int id,unsigned mode,unsigned char **out,const int32_t *range,
 const void *return_slot,uintptr_t stack_low,uintptr_t stack_high,DWORD thread);
/* Active client calls require the authentic command160 return address, canonical
   stream4 receive grid, valid tile, current generation and owning thread. Original
   is called exactly once after validation; only result0 notifies the ledger.
   Other terrain IDs and inactive calls forward unchanged. Invalid active client
   calls fail closed with native failure1 and do not invoke the mutator. */
int mxb_publication_receive(MxbPublicationHooks *,int id,const int32_t *full_grid,int tile,
 const void *return_slot,uintptr_t stack_low,uintptr_t stack_high,DWORD thread);

/* Invoke AFTER authentic client1f5f20 replacement on the same serialized owner.
   The full native grid must be coherent. During publisher reentry this records
   tile IDs only; publisher normal return is the coherent same-thread barrier
   that finishes clears THEN validates the current full echo grid once. No
   borrowed pointer is deferred to another thread/time. Outside the publisher,
   this validates one tile immediately. The provided receive call plan/entry
   supplies the authentic command160 replacement boundary.
   Invalid/reentrant-thread evidence faults closed; it never acknowledges local
   pending as native publication. The receive wrapper invokes this after native
   success. Every other authentic native replacement, if any, must also be
   reported or excluded by the active-session ownership contract. */
MxbPubResult mxb_publication_after_native_replace(MxbPublicationHooks *,uint64_t generation,uint32_t tile,DWORD thread);
#endif
