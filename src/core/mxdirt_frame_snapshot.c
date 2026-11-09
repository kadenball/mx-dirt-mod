#include "mxdirt_frame_snapshot.h"
#include <string.h>
#include <math.h>
#include <limits.h>

enum { CALL_RVA=0x135d70, TARGET_RVA=0x2a9bd0, STEP_RVA=0x396b48,
    SKIP_RVA=0x3d7fac, STATE_RVA=0x598904, REM30_RVA=0x5987f8,
    ADVANCE_RVA=0x5987fc, WHEEL_BASE=0x60f8, WHEEL_STRIDE=0xe310 };
static const char image_sha[]="9e792d7aed1dc40df11eea51d1a6d0519daec7be2b65609905df8c9de804a291";
static const unsigned char call_window[]={0x8b,0xcb,0xe8,0x5b,0x3e,0x17,0x00,
    0x83,0x3d,0xe8,0x00,0x43,0x00,0x00,0x74,0x17};
static const unsigned char target_prefix[]={0x40,0x53,0x48,0x83,0xec,0x20};
static int add_address(uint64_t a,uint64_t n,uint64_t *out) {
    if(n>UINT64_MAX-a)return 0;*out=a+n;return 1;
}
static int copy_at(MxbFrameRead read,void *user,uint64_t base,uint64_t offset,
    void *out,size_t n) {
    uint64_t p,end;
    return read&&n&&add_address(base,offset,&p)&&add_address(p,n-1,&end)&&
        read(user,p,out,n)==1;
}
static int valid_plan(const MxbFrameCallsitePlan *p) {
    uint64_t end;
    return p&&p->image_base&&p->image_bytes>=STATE_RVA+4&&
        add_address(p->image_base,p->image_bytes-1,&end)&&
        p->callsite==p->image_base+CALL_RVA&&
        p->original_target==p->image_base+TARGET_RVA&&
        p->return_address==p->callsite+5&&
        !memcmp(p->original_call,call_window+2,5);
}
int mxb_frame_callsite_plan(MxbFrameRead read,void *user,uint64_t base,
    uint64_t bytes,const char *sha,MxbFrameCallsitePlan *out) {
    if(!out||!read||!base||!sha||strcmp(sha,image_sha))return MXB_FRAME_BUILD;
    MxbFrameCallsitePlan p;memset(&p,0,sizeof(p));
    p.image_base=base;p.image_bytes=bytes;
    if(!add_address(base,CALL_RVA,&p.callsite)||
       !add_address(base,TARGET_RVA,&p.original_target)||
       !add_address(p.callsite,5,&p.return_address))return MXB_FRAME_BUILD;
    memcpy(p.original_call,call_window+2,5);
    if(!valid_plan(&p))return MXB_FRAME_BUILD;
    unsigned char window[sizeof(call_window)],entry[sizeof(target_prefix)];
    if(!copy_at(read,user,base,CALL_RVA-2,window,sizeof(window))||
       !copy_at(read,user,base,TARGET_RVA,entry,sizeof(entry)))return MXB_FRAME_READ;
    if(memcmp(window,call_window,sizeof(window))||
       memcmp(entry,target_prefix,sizeof(entry)))return MXB_FRAME_BUILD;
    *out=p;return MXB_FRAME_OK;
}
static int boolean(int v){return v==0||v==1;}
static int finite3(const float *v){return isfinite(v[0])&&isfinite(v[1])&&isfinite(v[2]);}
static int body_valid(const MxbWheelKinematics *b) {
    if(!finite3(b->center)||!finite3(b->linear_world)||!finite3(b->angular_world))return 0;
    for(int i=0;i<12;i++)if(!isfinite(b->rotation[i]))return 0;
    for(int a=0;a<3;a++)for(int c=a;c<3;c++) {
        float dot=0;for(int k=0;k<3;k++)dot+=b->rotation[a*4+k]*b->rotation[c*4+k];
        if(!isfinite(dot)||fabsf(dot-(a==c?1.f:0.f))>.01f)return 0;
    }
    return 1;
}
static int identity(const MxbFrameSnapshotState *s,const MxbFrameGate *g) {
    return g&&g->generation==s->config.generation&&g->bike==s->config.bike&&
        g->thread==s->config.owner_thread;
}
static int active(const MxbFrameGate *g) {
    return g&&boolean(g->testing)&&boolean(g->track_allowed)&&boolean(g->running)&&
        g->testing&&g->track_allowed&&g->running;
}
static void clear_history(MxbFrameSnapshotState *s) {
    s->has_pair=s->has_published=s->next_wheel=0;
    s->new_steps=0;memset(s->latest,0,sizeof(s->latest));
}
static int fail(MxbFrameSnapshotState *s,int reason) {
    s->in_call=s->busy=0;s->fault=reason;s->needs_rebind=1;clear_history(s);return reason;
}
static int read_pointer(MxbFrameSnapshotState *s,int wheel,uint64_t *p) {
    return copy_at(s->config.read,s->config.user,s->config.bike,
        WHEEL_BASE+(uint64_t)wheel*WHEEL_STRIDE+MXB_WHEEL_BODY_POINTER,p,8)&&*p;
}
typedef struct {int32_t step,skip,state,remainder30,advanced;} Scheduler;
static int read_scheduler(MxbFrameSnapshotState *s,Scheduler *v) {
    MxbFrameSnapshotConfig *c=&s->config;
    return copy_at(c->read,c->user,c->plan.image_base,STEP_RVA,&v->step,4)&&
        copy_at(c->read,c->user,c->plan.image_base,SKIP_RVA,&v->skip,4)&&
        copy_at(c->read,c->user,c->plan.image_base,STATE_RVA,&v->state,4)&&
        copy_at(c->read,c->user,c->plan.image_base,REM30_RVA,&v->remainder30,4)&&
        copy_at(c->read,c->user,c->plan.image_base,ADVANCE_RVA,&v->advanced,4);
}
static int scheduler_valid(const Scheduler *v) {
    int64_t pending=(int64_t)v->remainder30-v->advanced;
    return v->step==2&&v->skip==0&&v->state==7&&
        v->remainder30>=0&&v->remainder30<30&&pending>=0&&pending<2;
}
int mxb_frame_snapshot_init(MxbFrameSnapshotState *s,const MxbFrameSnapshotConfig *c) {
    uint64_t end;
    if(!s||!c||!c->read||!valid_plan(&c->plan)||!c->generation||!c->bike||
       !c->owner_thread||!c->max_elapsed_ms||c->max_elapsed_ms>100||
       c->first_outer_sequence==UINT64_MAX||
       !add_address(c->bike,WHEEL_BASE+WHEEL_STRIDE+MXB_WHEEL_BODY_POINTER+7,&end))
        return MXB_FRAME_INVALID;
    MxbFrameSnapshotState next;memset(&next,0,sizeof(next));next.config=*c;
    next.initialized=1;next.next_sequence=c->first_outer_sequence?c->first_outer_sequence:1;
    next.restarted=1;*s=next;return MXB_FRAME_OK;
}
uint64_t mxb_frame_snapshot_upcoming_sequence(const MxbFrameSnapshotState *s) {
    return s&&s->initialized&&s->in_call&&!s->busy&&!s->fault&&!s->needs_rebind?
        s->next_sequence:0;
}
int mxb_frame_snapshot_begin(MxbFrameSnapshotState *s,const MxbFrameGate *g,int ms) {
    if(!s||!s->initialized)return MXB_FRAME_INVALID;
    if(s->busy||s->in_call){s->fault=MXB_FRAME_REENTRY;return MXB_FRAME_REENTRY;}
    if(s->needs_rebind)return MXB_FRAME_REBIND_REQUIRED;
    if(s->next_sequence==UINT64_MAX)return fail(s,MXB_FRAME_SEQUENCE);
    if(!identity(s,g))return fail(s,MXB_FRAME_IDENTITY);
    if(!active(g)){clear_history(s);s->paused=1;s->restarted=1;return MXB_FRAME_PAUSED;}
    if(ms<0||(unsigned)ms>s->config.max_elapsed_ms)return fail(s,MXB_FRAME_INVALID);
    s->busy=1;s->fault=0;
    Scheduler v;
    if(!read_scheduler(s,&v))return fail(s,MXB_FRAME_READ);
    if(s->fault)return fail(s,s->fault);
    if(!scheduler_valid(&v))return fail(s,MXB_FRAME_SCHEDULER);
    for(int w=0;w<2;w++) {
        uint64_t p;if(!read_pointer(s,w,&p))return fail(s,MXB_FRAME_READ);
        if(s->body_address[w]&&s->body_address[w]!=p)return fail(s,MXB_FRAME_BODY_RESET);
        s->body_address[w]=p;
    }
    if(s->fault)return fail(s,s->fault);
    s->elapsed_ms=ms;s->new_steps=0;s->next_wheel=0;s->in_call=1;
    s->paused=0;s->busy=0;return MXB_FRAME_OK;
}
int mxb_frame_snapshot_note(MxbFrameSnapshotState *s,const MxbFrameGate *g,int w,
    uint64_t body,const MxdirtPhysicsSample *p) {
    if(!s||!s->initialized)return MXB_FRAME_INVALID;
    if(s->busy){s->fault=MXB_FRAME_REENTRY;return MXB_FRAME_REENTRY;}
    if(!s->in_call)return MXB_FRAME_WAITING;
    if(s->fault)return s->fault;
    int why=0;
    if(!identity(s,g)||!active(g))why=MXB_FRAME_IDENTITY;
    else if(w<0||w>1||w!=s->next_wheel||!p||p->generation!=s->config.generation||
        !p->sequence||p->sequence<=s->latest[w].sequence)why=MXB_FRAME_SEQUENCE;
    else if(body!=s->body_address[w])why=MXB_FRAME_BODY_RESET;
    else if(!body_valid(&p->body)||!boolean(p->active)||
        p->physics_dt!=2.f*.001f)why=MXB_FRAME_DATA;
    if(why){s->fault=why;return why;}
    s->latest[w]=*p;s->next_wheel=1-w;
    if(w==1){
        if(s->new_steps==UINT_MAX){s->fault=MXB_FRAME_SEQUENCE;return s->fault;}
        s->new_steps++;s->has_pair=1;
    }
    return MXB_FRAME_OK;
}
static int read_post(MxbFrameSnapshotState *s,int w,MxbWheelKinematics *body) {
    unsigned char raw[MXB_BODY_CAPTURE_BYTES],again[MXB_BODY_CAPTURE_BYTES];
    float matrix[16];uint64_t p,q;
    if(!read_pointer(s,w,&p))return MXB_FRAME_READ;
    if(p!=s->body_address[w])return MXB_FRAME_BODY_RESET;
    if(!copy_at(s->config.read,s->config.user,p,MXB_BODY_CAPTURE_OFFSET,raw,sizeof(raw))||
       !copy_at(s->config.read,s->config.user,s->config.bike,
          WHEEL_BASE+(uint64_t)w*WHEEL_STRIDE+0xc060,matrix,sizeof(matrix))||
       !copy_at(s->config.read,s->config.user,p,MXB_BODY_CAPTURE_OFFSET,again,sizeof(again))||
       !read_pointer(s,w,&q))return MXB_FRAME_READ;
    if(p!=q)return MXB_FRAME_BODY_RESET;
    if(memcmp(raw,again,sizeof(raw)))return MXB_FRAME_DATA;
    mxb_wheel_decode_body(body,raw,sizeof(raw));
    if(!body_valid(body))return MXB_FRAME_DATA;
    for(int row=0;row<3;row++) {
        if(memcmp(matrix+row*4,body->rotation+row*4,12)||
           memcmp(matrix+row*4+3,body->center+row,4))return MXB_FRAME_DATA;
    }
    return MXB_FRAME_OK;
}
int mxb_frame_snapshot_finish(MxbFrameSnapshotState *s,const MxbFrameGate *g,MxbFrameSnapshot *out) {
    if(!s||!s->initialized||!out)return MXB_FRAME_INVALID;
    if(s->busy){s->fault=MXB_FRAME_REENTRY;return MXB_FRAME_REENTRY;}
    if(!s->in_call)return s->needs_rebind?MXB_FRAME_REBIND_REQUIRED:MXB_FRAME_WAITING;
    if(s->fault)return fail(s,s->fault);
    if(!identity(s,g))return fail(s,MXB_FRAME_IDENTITY);
    if(!active(g)){
        s->in_call=0;s->paused=1;s->restarted=1;clear_history(s);return MXB_FRAME_PAUSED;
    }
    if(s->next_wheel)return fail(s,MXB_FRAME_SEQUENCE);
    s->busy=1;
    Scheduler before,after;
    if(!read_scheduler(s,&before))return fail(s,MXB_FRAME_READ);
    if(!scheduler_valid(&before))return fail(s,MXB_FRAME_SCHEDULER);
    MxbFrameSnapshot f;memset(&f,0,sizeof(f));
    for(int w=0;w<2;w++) {
        int result=read_post(s,w,&f.postintegration[w]);
        if(result!=MXB_FRAME_OK)return fail(s,result);
    }
    /* Re-read both bodies after the whole pair: reject movement while reading
       the other wheel as well as within one body's copy. */
    for(int w=0;w<2;w++) {
        MxbWheelKinematics check;
        int result=read_post(s,w,&check);
        if(result!=MXB_FRAME_OK)return fail(s,result);
        if(memcmp(&check,&f.postintegration[w],sizeof(check)))return fail(s,MXB_FRAME_DATA);
    }
    if(!read_scheduler(s,&after))return fail(s,MXB_FRAME_READ);
    if(memcmp(&before,&after,sizeof(before)))return fail(s,MXB_FRAME_SCHEDULER);
    if(s->fault)return fail(s,s->fault);
    if(!s->has_pair){s->busy=s->in_call=0;return MXB_FRAME_WAITING;}
    if(!s->new_steps&&s->has_published&&
       memcmp(f.postintegration,s->published.postintegration,sizeof(f.postintegration)))
        return fail(s,MXB_FRAME_BODY_RESET);
    if(s->next_sequence==UINT64_MAX)return fail(s,MXB_FRAME_SEQUENCE);
    f.generation=s->config.generation;f.bike=s->config.bike;f.thread=s->config.owner_thread;
    f.sequence=s->next_sequence++;f.new_steps=s->new_steps;f.elapsed_ms=s->elapsed_ms;
    f.pending_physics_ms=before.remainder30-before.advanced;
    f.dt=(float)s->elapsed_ms*.001f;f.history_restarted=s->restarted;
    for(int w=0;w<2;w++) {
        f.body_address[w]=s->body_address[w];f.last_preintegration[w]=s->latest[w];
        mxb_wheel_angular_local(f.angular_local[w],&f.postintegration[w]);
        if(!finite3(f.angular_local[w]))return fail(s,MXB_FRAME_DATA);
    }
    s->published=f;s->has_published=1;s->restarted=0;s->busy=s->in_call=0;*out=f;
    return f.new_steps?MXB_FRAME_OK:MXB_FRAME_NO_NEW_STEP;
}
int mxb_frame_snapshot_controller_values(const MxbFrameSnapshot *s,int w,
    MxdirtControllerFrame *out) {
    if(!s||!out||w<0||w>1||!s->generation||!s->sequence||
       !s->last_preintegration[w].sequence||
       s->last_preintegration[w].generation!=s->generation||
       !body_valid(&s->postintegration[w])||
       !isfinite(s->angular_local[w][0])||!isfinite(s->dt)||s->dt<0.f||s->dt>.1f)
        return MXB_FRAME_INVALID;
    MxdirtControllerFrame f=*out;
    memcpy(f.controller_center_metres,s->postintegration[w].center,12);
    memcpy(f.center_velocity_metres,s->postintegration[w].linear_world,12);
    f.angular_local_x=s->angular_local[w][0];f.dt=s->dt;f.generation=s->generation;
    f.sequence=s->sequence;f.physics_sequence=s->last_preintegration[w].sequence;
    *out=f;return MXB_FRAME_OK;
}
