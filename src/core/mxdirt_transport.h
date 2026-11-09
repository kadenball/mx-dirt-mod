#ifndef MXDIRT_TRANSPORT_H
#define MXDIRT_TRANSPORT_H
#include "mxdirt_publication_hooks.h"
#include "mxdirt_excavation_gate.h"

/* Source-only coordinator. No discovery, allocation of executable memory,
   installation into a process, disk I/O or automatic activation. The parent
   DLL calls this at a VERIFIED serialized engine ownership gap on the captured
   owner: a stopped native lifecycle, or the exact startup receive-return gap
   authenticated by mxb_bootstrap_origin.h before local physics. The latter is
   not a stopped state and must separately exclude reentry/lifecycle changes.
   Pause is not retirement. All context/code/grid lifetimes stay pinned until
   successful owned restoration; zero inflight alone is not quiescence. */
typedef struct MxdirtTransport MxdirtTransport;
enum { MXB_TRANSPORT_CODE_BYTES=640, MXB_TRANSPORT_PATCHES=8 };
typedef enum { MXB_TRANSPORT_PREPARED,MXB_TRANSPORT_INSTALLED,MXB_TRANSPORT_ACTIVE,
 MXB_TRANSPORT_PAUSED,MXB_TRANSPORT_DRAINED,MXB_TRANSPORT_RETIRED,
 MXB_TRANSPORT_REMOVED,MXB_TRANSPORT_FAULTED } MxbTransportPhase;
typedef struct {
 uint64_t generation;DWORD owner_thread;
 int offline_testing,exact_track_profile,pinned_lifetime;
 int engine_quiescent,transport_quiescent,old_track_retired;
 /* 0 normally. 3 ONLY after both signature-guarded client2a3da0 and host29e5a0
    teardown calls returned and parent proved their native stream resets.
    Module also verifies both complete0x68-byte stream records are now zero.
    Retirement then leaves dirty slots zero, never resurrecting old pointers. */
 unsigned native_reset_slots;
} MxbTransportFacts;
/* Kind0 code; kind1 aligned native pointer slot. Parent may provide its own
   approved patch primitive. It MUST compare expected bytes and either replace
   ALL bytes or leave them unchanged; failure may follow a complete write.
   Coordinator independently inspects the
   outcome and rolls back ONLY owned bytes at quiescence. Unknown/partial bytes
   retain the context and pinned code; never claim successful atomic install.
   No callbacks or engine execution may interleave the whole transaction. */
typedef int (*MxbTransportPatch)(void *,void *address,const void *expected,const void *next,size_t,int kind);
typedef struct {
 unsigned char *exe;size_t image_bytes;
 const char *verified_executable_sha256; /* Hash ACTUALLY checked by parent. */
 unsigned char *near_code;size_t near_code_bytes; /* >=640 zero, pinned, executable, rel32 reachable. */
 MxbPacingSpan span;void *span_user;
 MxbTransportPatch patch;void *patch_user;
 size_t target_capacity,native_event_capacity,max_changed_cells,max_batch_tiles,trace_capacity;
 /* Production: NULL selects exact native targets. Private fixtures may supply
    equivalent functions; never use these overrides in an installed build. */
 MxbNativePublisher fixture_publisher;MxbNativeReplace fixture_replace;
 MxbPacingDirty fixture_dirty;MxbNativeExcavation fixture_excavation;
} MxbTransportConfig;
typedef struct {
 uint64_t sequence,generation;LONGLONG begin,end;DWORD thread;
 int kind,result,phase;uint32_t tile;size_t queued,outstanding,selected;
} MxbTransportTrace;
typedef struct {
 MxbTransportPhase phase;uint64_t generation;DWORD owner_thread;
 unsigned owned_patch_mask;int installed,exclusive,pace_bits,faulted,inflight;
 size_t queued_targets,outstanding_events,trace_count,trace_dropped,allocated_ledger_bytes;
 uint64_t accepted_sequence,locally_issued_sequence,sent_batches,echo_batches,suppressed_excavations;
 MxbPubResult last_result;
 /* Diagnostics: source line and result of the FIRST fault, and health detail
    bits (1 hooks,2 ledger,4 excavation,8 pace!=31 | pace<<8). 0 = none. */
 int fault_line,fault_why;unsigned fault_detail;
 int hook_fail_line,hook_result,ledger_fault_line;
} MxbTransportState;

MxdirtTransport *mxb_transport_create(const MxbTransportConfig *,const MxbTransportFacts *);
/* First rejected startup predicate, captured synchronously on the owner.
   Strings are static; this report never borrows native memory. */
typedef struct { const char *stage; int side; } MxbTransportCreateReport;
MxdirtTransport *mxb_transport_create_report(const MxbTransportConfig *,const MxbTransportFacts *,MxbTransportCreateReport *);
/* Parent's copied solver seed must match this prepared ledger exactly. Private
   copied arrays only; verified owner/gap facts required, no native write occurs. */
int mxb_transport_matches_initial(const MxdirtTransport *,const MxbTransportFacts *,
 const int16_t *physics_base,const int32_t *initial_native,size_t cells);
int mxb_transport_install(MxdirtTransport *,const MxbTransportFacts *);
int mxb_transport_activate(MxdirtTransport *,const MxbTransportFacts *,int explicit_activation);
/* Compatible with MxdirtAcceptTarget/session_publish. Synchronous immutable
   complete sparse target copy; serialized captured owner only. Return1 accepted
   (including exact duplicate),0 backpressure/pause,-1 invalid/fault. */
int mxb_transport_accept(void *,uint64_t generation,uint64_t previous_sequence,
 uint64_t sequence,const MxbPubAbsoluteCell *,size_t count);
int mxb_transport_pause(MxdirtTransport *,DWORD owner_thread);
int mxb_transport_resume(MxdirtTransport *,DWORD owner_thread);
MxbPubResult mxb_transport_drain(MxdirtTransport *,const MxbTransportFacts *);
int mxb_transport_retire(MxdirtTransport *,const MxbTransportFacts *);
int mxb_transport_remove(MxdirtTransport *,const MxbTransportFacts *);
int mxb_transport_destroy(MxdirtTransport *);
int mxb_transport_state(const MxdirtTransport *,MxbTransportState *);
/* Self-heal: adopt native terrain/pending as truth after a fault and resume.
   Owner thread, between native publisher calls. Returns 1 when ACTIVE again. */
int mxb_transport_recover(MxdirtTransport *,DWORD owner_thread);
/* Adopt tiles changed by another native terrain writer instead of faulting (see mxb_pub_set_adopt_foreign). */
void mxb_transport_set_adopt_foreign(MxdirtTransport *,int enabled);
uint64_t mxb_transport_foreign_tiles(const MxdirtTransport *);
/* v8.8: rects of tiles adopted since *cursor (SIZE_MAX: too many - resend everything) */
size_t mxb_transport_adopted_rects(const MxdirtTransport *,uint64_t *cursor,uint32_t *rects,size_t max);
/* Borrowed immutable trace valid only under supplied stopped owner facts. */
const MxbTransportTrace *mxb_transport_trace(const MxdirtTransport *,const MxbTransportFacts *,size_t *count);
/* Default in-process patch primitive; never call independently on live code.
   Uses guarded CAS for slots, VirtualProtect/FlushInstructionCache for code. */
int mxb_transport_patch_windows(void *,void *,const void *,const void *,size_t,int);

/* Authentic ABI entrypoints; their _AddressOfReturnAddress is passed directly
   to the previously validated boundary adapter, never via another ABI wrapper. */
int mxb_transport_publisher_entry(void);
int mxb_transport_receive_entry(int,const int32_t *,int);
int mxb_transport_client_dirty_entry(int,unsigned,unsigned char **,const int32_t *);
int mxb_transport_host_dirty_entry(int,unsigned,unsigned char **,const int32_t *);
int mxb_transport_excavation_entry(void *,float,float,double);
/* Explicit contexts used by private native-call fixtures. Return slots and
   ranges must satisfy the same native stack layout as the actual entries. */
int mxb_transport_publisher(MxdirtTransport *,const void *,uintptr_t,uintptr_t,DWORD);
int mxb_transport_receive(MxdirtTransport *,int,const int32_t *,int,const void *,uintptr_t,uintptr_t,DWORD);
int mxb_transport_dirty(MxdirtTransport *,int side,int,unsigned,unsigned char **,const int32_t *,const void *,uintptr_t,uintptr_t,DWORD);
int mxb_transport_excavation(MxdirtTransport *,void *,float,float,double,DWORD);
#endif
