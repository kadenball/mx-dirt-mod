/* Startup bootstrap: when the saved track is still streaming in at RunInit,
   MX Bikes' receive call and tyre excavation call are wrapped until a quiet
   receive return lets the transport adopt the terrain (gives up after 30 s).
   Included by mx_dirt.c after its complete preparation transaction.
   Native ownership remains separate until that transaction succeeds. No DFM,
   collision, pending, dirty, pacing or saved-track data is written here. */
enum { RA_BOOT_IDLE,RA_BOOT_DRAINING,RA_BOOT_ADOPTED,RA_BOOT_ABORTED };
static const unsigned char ra_boot_receive_call[5]={0xe8,0xba,0xf9,0xff,0xff};
static const unsigned char ra_boot_dig_call[5]={0xe8,0x0c,0x70,0x01,0};
static int ra_bootstrap_receive(void);
static int ra_bootstrap_excavation(void *,float,float,double);
static int ra_bootstrap_owned(void){return ra_bootstrap_hooks[0].owned||ra_bootstrap_hooks[1].owned;}
static int ra_bootstrap_hook_owned(const RaHook *h,uintptr_t target){
 unsigned char code[14],expected[14]={0xff,0x25,0,0,0,0},site[5];memcpy(expected+6,&target,8);
 return h->owned&&ra_read(NULL,(uintptr_t)h->site,site,5)&&!memcmp(site,h->patch,5)&&
  ra_read(NULL,(uintptr_t)h->relay,code,14)&&!memcmp(code,expected,14);
}
static int ra_bootstrap_restore_one(unsigned index){
 RaHook *h=&ra_bootstrap_hooks[index];if(!h->owned)return 1;
 unsigned char b[5];if(!ra_read(NULL,(uintptr_t)h->site,b,5))return 0;
 if(!memcmp(b,h->original,5)){if(!FlushInstructionCache(GetCurrentProcess(),h->site,5))return 0;h->owned=0;return 1;}
 if(memcmp(b,h->patch,5))return 0;
 int wrote=ra_write_code(h->site,h->original,5);
 if(!memcmp(h->site,h->original,5)&&FlushInstructionCache(GetCurrentProcess(),h->site,5)){h->owned=0;return 1;}
 (void)wrote;return 0;
}
static int ra_bootstrap_patch_one(unsigned index){
 RaHook *h=&ra_bootstrap_hooks[index];if(h->owned||memcmp(h->site,h->original,5))return 0;
 int wrote=ra_write_code(h->site,h->patch,5);
 if(memcmp(h->site,h->original,5))h->owned=1;
 return wrote&&!memcmp(h->site,h->patch,5);
}
static void ra_bootstrap_abort(int reason){
 InterlockedExchange(&ra_bootstrap_cancel,1);
 if(GetCurrentThreadId()==ra_owner){ra_bootstrap_gap=0;if(ra_bootstrap_phase==RA_BOOT_DRAINING){ra_bootstrap_phase=RA_BOOT_ABORTED;ra_mark(RA_BOOTSTRAP,reason);}}
}
static int ra_bootstrap_remove(void){
 if(!ra_bootstrap_owned())return 1;
 if(GetCurrentThreadId()!=ra_owner||ra_inflight||ra_bootstrap_depth||ra_bootstrap_dig_depth||ra_running)return 0;
 ra_bootstrap_abort(-12);
 /* Receive dispatch removed first, then prospective excavation. All relay
    allocations remain pinned for process lifetime; no returning wrapper freed. */
 int a=ra_bootstrap_restore_one(0),b=ra_bootstrap_restore_one(1);return a&&b;
}
static int ra_bootstrap_binding(int require_clean_dirty){
 MxbPacingGate gate={0,ra_owner,ra_generation};
 MxbPacingContext c={ra_image,(size_t)ra_image_size,1,&gate,ra_generation,ra_span,NULL,{NULL,NULL}};
 RaGrid grid;if(!ra_grid_read(&grid))return 0;
 for(int side=0;side<2;side++){
  uint32_t tiles;if(!mxb_pace_binding(&c,side,&tiles)||tiles!=((grid.width+63)/64)*((grid.height+63)/64))return 0;
  unsigned char *g=ra_image+0x568488+side*0x6160;
  if(mxb_pace_u32(g+0x740)!=grid.width||mxb_pace_u32(g+0x744)!=grid.height)return 0;
  if(require_clean_dirty){const unsigned char *flags=(void*)mxb_pace_ptr(g+(side?0x7b0:0x7d0));
   for(unsigned k=0;k<tiles;k++)if(flags[k])return 0;
  }
 }
 return 1;
}
static int ra_bootstrap_arm(void){
 if(!ra_stopped_gap||!ra_engine_gap()||!ra_bootstrap_requested||ra_bootstrap_phase!=RA_BOOT_IDLE||
  ra_transport||ra_session||ra_setup_attempted||ra_fatal||ra_foreign||ra_allowed||!ra_new_run||
  !ra_event_valid||!ra_enabled||!ra_pinned||!ra_activation_requested||ra_bootstrap_owned()||
  ra_hooks[0].owned||ra_hooks[1].owned||ra_hooks[2].owned||ra_hooks[3].owned)return 0;
 MxbBootstrapOrigin q={(uintptr_t)ra_image,ra_image_size,0,0,0,ra_verified_hash,NULL};
 if(!mxb_bootstrap_code(ra_read,NULL,&q)||!mxdirt_patches_present(ra_image)||!ra_bootstrap_binding(0))return 0;
 MxbExcavationCallPlan plan;
 if(!ra_hook_plan(&ra_bootstrap_hooks[0],ra_image+0x2a6241,ra_boot_receive_call,(uintptr_t)ra_bootstrap_receive)||
  !ra_hook_plan(&ra_bootstrap_hooks[1],ra_image+0x1a4d8f,ra_boot_dig_call,(uintptr_t)ra_bootstrap_excavation)||
  !mxb_excavation_call_plan((uintptr_t)ra_image,(uintptr_t)ra_bootstrap_hooks[1].relay,1,
   ra_image+0x1a4d8f,ra_image+0x1bbda0,&plan))return 0;
 ra_bootstrap_original_receive=(RaCloseFn)(ra_image+0x2a5c00);
 ra_bootstrap_original_excavation=(MxbNativeExcavation)(ra_image+0x1bbda0);
 ra_bootstrap_generation=ra_generation;ra_bootstrap_calls=ra_bootstrap_attempts=ra_bootstrap_suppressed=0;
 ra_bootstrap_elapsed=ra_bootstrap_clock=ra_bootstrap_attempt_at=0;InterlockedExchange(&ra_bootstrap_cancel,0);
 /* Inactive until the complete pair is installed. A partial/failed write is
    independently inspected and rolled back; unknown bytes retain ownership. */
 if(!ra_bootstrap_patch_one(0)||!ra_bootstrap_patch_one(1)){
  ra_bootstrap_abort(-2);if(!ra_bootstrap_remove())ra_fault(-129);return 0;
 }
 ra_bootstrap_phase=RA_BOOT_DRAINING;ra_mark(RA_BOOTSTRAP,1);return 1;
}
static __declspec(noinline) int ra_bootstrap_excavation(void *terrain,float x,float z,double amount){
 uintptr_t caller=(uintptr_t)_ReturnAddress();DWORD incoming=GetLastError();
 LONG depth=InterlockedIncrement(&ra_bootstrap_dig_depth);InterlockedIncrement(&ra_inflight);
 int owner=GetCurrentThreadId()==ra_owner;
 if(!owner){InterlockedExchange(&ra_foreign,1);InterlockedExchange(&ra_bootstrap_cancel,1);}
 int scoped=terrain==ra_image+0x568488-0xce8||terrain==ra_image+0x568488+0x6160-0xce8;
 int suppress=owner&&depth==1&&!ra_bootstrap_depth&&ra_bootstrap_phase==RA_BOOT_DRAINING&&
  !ra_bootstrap_cancel&&ra_bootstrap_generation==ra_generation&&ra_event_valid&&ra_running&&
  !ra_transport&&!ra_session&&!ra_allowed&&!ra_foreign&&!ra_fatal&&scoped&&
  caller==(uintptr_t)ra_image+0x1a4d94&&ra_bootstrap_hook_owned(&ra_bootstrap_hooks[1],(uintptr_t)ra_bootstrap_excavation)&&
  mxb_bootstrap_immediate(ra_read,NULL,(uintptr_t)ra_image)&&ra_bootstrap_binding(0);
 if(owner&&depth!=1)ra_bootstrap_abort(-3);
 SetLastError(incoming);int result;
 if(suppress){ra_bootstrap_suppressed++;result=0;}else result=ra_bootstrap_original_excavation(terrain,x,z,amount);
 DWORD outgoing=GetLastError();InterlockedDecrement(&ra_inflight);InterlockedDecrement(&ra_bootstrap_dig_depth);SetLastError(outgoing);return result;
}
static __declspec(noinline) int ra_bootstrap_receive(void){
 void *slot=_AddressOfReturnAddress();DWORD incoming=GetLastError();
 uint64_t generation=ra_generation;LONG token=ra_reset_token;
 LONG depth=InterlockedIncrement(&ra_bootstrap_depth);InterlockedIncrement(&ra_inflight);
 int owner=GetCurrentThreadId()==ra_owner;
 if(!owner){InterlockedExchange(&ra_foreign,1);InterlockedExchange(&ra_bootstrap_cancel,1);}
 if(owner&&depth!=1)ra_bootstrap_abort(-3);
 SetLastError(incoming);int result=ra_bootstrap_original_receive();DWORD outgoing=GetLastError();
 if(owner&&depth==1&&ra_bootstrap_phase==RA_BOOT_DRAINING&&!ra_bootstrap_cancel&&
  generation==ra_generation&&generation==ra_bootstrap_generation&&token==ra_reset_token&&
  ra_event_valid&&ra_running&&!ra_fatal&&!ra_foreign&&!ra_transport&&!ra_session&&!ra_allowed&&
  !ra_outer_depth&&!ra_close_depth&&!ra_bootstrap_dig_depth&&ra_inflight==1){
  uint64_t now=GetTickCount64();if(ra_bootstrap_clock&&now>=ra_bootstrap_clock)ra_bootstrap_elapsed+=now-ra_bootstrap_clock;
  ra_bootstrap_clock=now;ra_bootstrap_calls++;
  NT_TIB *tib=(NT_TIB*)NtCurrentTeb();MxbBootstrapOrigin q={(uintptr_t)ra_image,ra_image_size,
   (uintptr_t)tib->StackLimit,(uintptr_t)tib->StackBase,(uintptr_t)slot,ra_verified_hash,ra_bootstrap_hooks[0].patch};
  int ancestry=mxb_bootstrap_ancestry(ra_read,NULL,&q)&&
   ra_bootstrap_hook_owned(&ra_bootstrap_hooks[0],(uintptr_t)ra_bootstrap_receive)&&
   ra_bootstrap_hook_owned(&ra_bootstrap_hooks[1],(uintptr_t)ra_bootstrap_excavation);
  int qualified=result==0&&ancestry&&mxb_bootstrap_immediate(ra_read,NULL,(uintptr_t)ra_image);
  int expired=ra_bootstrap_elapsed>=30000||ra_bootstrap_calls>=20000;
  if(qualified){
   ra_bootstrap_gap=1;ra_bootstrap_gap_generation=generation;ra_bootstrap_gap_token=token;
   if(expired){
    if(!ra_bootstrap_restore_one(1))ra_fatal=1;ra_bootstrap_abort(-4);
   }else if((!ra_bootstrap_attempts||ra_bootstrap_elapsed>=ra_bootstrap_attempt_at+250)&&ra_bootstrap_binding(1)){
    ra_bootstrap_attempt_at=ra_bootstrap_elapsed;ra_bootstrap_attempts++;
    if(!ra_bootstrap_restore_one(1)){ra_bootstrap_abort(-5);ra_fatal=1;}
    else {
     int adopted;
     adopted=ra_prepare_transport();
     if(adopted){ra_bootstrap_phase=RA_BOOT_ADOPTED;ra_mark(RA_BOOTSTRAP,2);}
     else if(ra_transport||ra_session||!ra_engine_gap()||!ra_bootstrap_requested){
      /* A partially owned active transaction keeps its own exclusion/retention.
         Never restore its CALL or re-enable a competing writer here. */
      ra_bootstrap_abort(-6);
     }else if(!ra_bootstrap_patch_one(1)){ra_bootstrap_abort(-7);ra_fatal=1;}
    }
   }
   ra_bootstrap_gap=0;
  }else if(expired||!ancestry||result!=0||!ra_bootstrap_binding(0)){
   /* No recognized gap: cancel prospective suppression immediately but defer
      CALL restoration to a stopped owner lifecycle. Timeout cannot depend on
      an unavailable queue-completion predicate. */
   ra_bootstrap_abort(expired?-4:-8);
  }
 }else if(owner&&ra_bootstrap_phase==RA_BOOT_DRAINING&&
          (generation!=ra_generation||token!=ra_reset_token||ra_fatal||ra_foreign))ra_bootstrap_abort(-9);
 InterlockedDecrement(&ra_inflight);InterlockedDecrement(&ra_bootstrap_depth);SetLastError(outgoing);return result;
}
