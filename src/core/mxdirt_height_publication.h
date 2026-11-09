#ifndef MXB_HEIGHT_PUBLICATION_H
#define MXB_HEIGHT_PUBLICATION_H
#include <stdint.h>
#include <stddef.h>
/* Existing reusable header has non-inline static helpers; users need not call
   every helper. Keep this compatibility diagnostic scope local to that header. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "mxdirt_delta_stage.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* Original adapter code. Owns private ledger/queue memory only; has no hooks,
   game addresses, locking or lifecycle/installer logic. ALL calls are serialized
   by one owner. A worker transfers an immutable completed absolute target to that
   owner; enqueue copies it. Never call concurrently, mutate an input during a
   call, or expose mutable ledger arrays to another thread. */
typedef struct MxbHeightPublication MxbHeightPublication;
typedef enum {
 MXB_PUB_OK=0, MXB_PUB_PROGRESS=1, MXB_PUB_TARGET_ISSUED=2,
 MXB_PUB_BLOCKED=3, MXB_PUB_DUPLICATE=4, MXB_PUB_STALE=5,
 MXB_PUB_OUT_OF_ORDER=6, MXB_PUB_QUEUE_FULL=7, MXB_PUB_BUSY=8,
 MXB_PUB_NO_CHANGE=9, MXB_PUB_NEEDS_DRAIN=10,
 MXB_PUB_INVALID=-1, MXB_PUB_FAULT=-2, MXB_PUB_INACTIVE=-3
} MxbPubResult;
typedef struct {
 uint64_t generation,target_sequence,issued_sequence;
 MxbDeltaReceipt pending_write; size_t remaining_cells;
 uint64_t remaining_magnitude;
 /* TARGET_ISSUED means all target deltas reached the LOCAL pending buffer.
    It is NEVER an acknowledgment from native host/client publication. */
} MxbPubIssueReceipt;
typedef struct {
 uint64_t native_event_serial; uint32_t tile; size_t coalesced_events;
} MxbPubEchoReceipt;
typedef struct { uint32_t index;int32_t absolute; } MxbPubAbsoluteCell;
typedef struct { size_t tiles,matched_tiles,coalesced_events;uint64_t last_event_serial; } MxbPubBatchEchoReceipt;
typedef struct {
 uint64_t generation,last_enqueued_sequence,locally_issued_sequence;
 size_t queued_targets,outstanding_native_events; int active,faulted,reserved;
 const int32_t *issued,*sent,*native_echo,*expected_pending; size_t cells;
 size_t allocated_bytes; /* Exact requested private allocation bytes, no allocator overhead. */
 int fault_line; /* diagnostics: source line of the first ledger fault, 0 none */
 /* Read-only until next mutating API call. Sequence is deliberately NOT given
    to native_echo: a native tile may coalesce multiple local absolute targets. */
} MxbPubState;

MxbHeightPublication *mxb_pub_create(uint32_t width,uint32_t height,uint64_t generation,
 const int16_t *physics_base,const int32_t *initial_native,size_t cells,
 const MxbPendingView *pending,size_t target_capacity,size_t native_event_capacity);
/* Sparse mode stores at most max_changed_cells per immutable target. Its omitted
   cells inherit the previous accepted target. Caller MUST provide the complete
   changed-node set from that target, sorted uniquely by index. This is a worker
   input contract, never permission to omit checks of native pending/echo memory.
   Dense enqueue is intentionally INVALID on a sparse-mode ledger and vice versa. */
MxbHeightPublication *mxb_pub_create_sparse(uint32_t width,uint32_t height,uint64_t generation,
 const int16_t *physics_base,const int32_t *initial_native,size_t cells,
 const MxbPendingView *pending,size_t target_capacity,size_t native_event_capacity,size_t max_changed_cells);
void mxb_pub_destroy(MxbHeightPublication *p);
/* Before first activation only: exact immutable seed comparison. A converged
   native stream may contain saved deformation, but it must still be the SAME
   base and full DFM from which this ledger was prepared. No state is changed. */
int mxb_pub_matches_initial(const MxbHeightPublication *p,const int16_t *physics_base,
 const int32_t *initial_native,size_t cells);
/* Caller must actually disable every native wheel writer before activating.
   Keep writers excluded until quiescent drain/deactivation, including faults.
   No API here changes native excavation itself. */
MxbPubResult mxb_pub_activate(MxbHeightPublication *p,const MxbPendingView *pending,
 const int32_t *native_full_grid,size_t cells,int native_excavation_exclusively_disabled);
MxbPubResult mxb_pub_enqueue(MxbHeightPublication *p,uint64_t generation,uint64_t sequence,
 const int32_t *absolute_target,size_t cells);
MxbPubResult mxb_pub_enqueue_sparse(MxbHeightPublication *p,uint64_t generation,uint64_t previous_sequence,
 uint64_t sequence,const MxbPubAbsoluteCell *changes,size_t count);
MxbPubResult mxb_pub_issue(MxbHeightPublication *p,MxbPendingView *pending,
 int native_excavation_exclusively_disabled,MxbPubIssueReceipt *receipt);
/* Reserve history BEFORE native send/clear, then finish AFTER exactly that
   native tile is zeroed. A prepared reservation blocks issue/echo processing.
   If native callbacks reenter, retain immutable echo snapshots and defer them.
   Queue-full means DO NOT send/clear: retain native work until capacity exists.
   If the publisher cannot honor backpressure, this interface cannot be used. */
MxbPubResult mxb_pub_prepare_native_clear(MxbHeightPublication *p,uint32_t tile,
 const MxbPendingView *pending,uint64_t *reservation);
/* Sorted unique nonempty tile set. Reserve ALL history capacity before ANY native
   send. finish/cancel below act on the complete reserved set, atomically. Native
   may touch no unreserved tile; caller must retain/re-dirty skipped work. */
MxbPubResult mxb_pub_prepare_native_clear_batch(MxbHeightPublication *p,const uint32_t *tiles,size_t count,
 const MxbPendingView *pending,uint64_t *reservation);
/* One exact pending scan shared by issue and prepare under the same serialized
   owner; no external code/callback runs in between. Capacity/tile-set validation
   happens BEFORE issue. PROGRESS/TARGET_ISSUED/BLOCKED/NO_CHANGE return a nonzero
   reservation token. Other results, including positive QUEUE_FULL/BUSY/NEEDS_DRAIN,
   leave token and pending unchanged. Other results are errors. */
MxbPubResult mxb_pub_issue_prepare_batch(MxbHeightPublication *p,MxbPendingView *pending,
 int native_excavation_exclusively_disabled,const uint32_t *tiles,size_t count,
 MxbPubIssueReceipt *receipt,uint64_t *reservation);
MxbPubResult mxb_pub_finish_native_clear(MxbHeightPublication *p,uint64_t reservation,
 const MxbPendingView *pending);
MxbPubResult mxb_pub_cancel_native_clear(MxbHeightPublication *p,uint64_t reservation,
 const MxbPendingView *pending);
/* Call only for an authentic native published-tile event and its coherent FULL
   grid snapshot. Native host may skip earlier checkpoints for the SAME tile.
   Unknown values or changes outside the indicated tile fail closed. */
MxbPubResult mxb_pub_observe_native_echo(MxbHeightPublication *p,uint32_t tile,
 const int32_t *native_full_grid,size_t cells,MxbPubEchoReceipt *receipt);
/* Tile set and coherent full grid from one native batch barrier. All outside-set
   cells are compared once. Every indicated tile must match a retained ordered
   checkpoint or its unchanged echo. Whole batch validates before any mutation. */
MxbPubResult mxb_pub_observe_native_echo_batch(MxbHeightPublication *p,const uint32_t *tiles,size_t count,
 const int32_t *native_full_grid,size_t cells,MxbPubBatchEchoReceipt *receipt);
/* Self-heal: adopt native published + pending as truth and clear the fault.
   Owner thread only, outside any native publisher call. */
MxbPubResult mxb_pub_resync(MxbHeightPublication *,const MxbPendingView *,const int32_t *native,size_t cells);
/* Tolerant echo: adopt a tile changed by another native terrain writer instead of faulting (default off). */
void mxb_pub_set_adopt_foreign(MxbHeightPublication *,int enabled);
size_t mxb_pub_adopted_rects(const MxbHeightPublication *,uint64_t *cursor,uint32_t *rects,size_t max);
uint64_t mxb_pub_foreign_tiles(const MxbHeightPublication *);
MxbPubResult mxb_pub_state(const MxbHeightPublication *p,MxbPubState *state);
/* Transport quiescence is an explicit external fact, not elapsed time. Requires
   no target work, zero pending, coherent equal issued/sent/echo/native grids.
   It can retire redundant unobserved checkpoints after net-zero coalescence. */
MxbPubResult mxb_pub_deactivate_drained(MxbHeightPublication *p,const MxbPendingView *pending,
 const int32_t *native_full_grid,size_t cells,int transport_quiescent);
/* Same dimensions only; generation must increase. Normal reset requires an
   inactive drained ledger and transport quiescence. retire_old_track permits
   explicit abandonment ONLY after old track/transport is stopped and retired;
   new pending must be zero. No external buffers are cleared by reset. */
MxbPubResult mxb_pub_reset(MxbHeightPublication *p,uint64_t new_generation,
 const int16_t *physics_base,const int32_t *initial_native,size_t cells,
 const MxbPendingView *pending,int transport_quiescent,int retire_old_track);
#endif
