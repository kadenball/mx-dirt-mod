#include "mxdirt_terrain_scope.h"
#include <string.h>
#include <limits.h>
#include <math.h>

_Static_assert(sizeof(uintptr_t)==8,"MX Bikes adapter requires x64");
static uint32_t u32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static uintptr_t ptr(const unsigned char *p){uintptr_t v;memcpy(&v,p,8);return v;}
static float f32(const unsigned char *p){float v;memcpy(&v,p,4);return v;}
static int copy(MxDirtTsRead read,void *user,uintptr_t p,void *out,size_t n){
    return read&&p&&n&&p<=UINTPTR_MAX-(n-1)&&read(user,p,out,n);
}
static int at(MxDirtTerrainScope *c,uintptr_t p,size_t off,void *out,size_t n){
    return p&&p<=UINTPTR_MAX-off&&copy(c->config.read,c->config.read_user,p+off,out,n);
}
static int config_ok(const MxDirtTsConfig *v){return v&&v->read&&v->generation&&v->owner_thread;}
int mxdirt_ts_init(MxDirtTerrainScope *c,const MxDirtTsConfig *v){
    if(!c||!config_ok(v))return 0;
    MxDirtTsConfig saved=*v;memset(c,0,sizeof(*c));c->config=saved;return 1;
}
int mxdirt_ts_next_capture(MxDirtTerrainScope *c,uint32_t thread){
    if(!c||!config_ok(&c->config)||thread!=c->config.owner_thread||c->depth||c->serial==UINT64_MAX)return 0;
    c->poisoned=0;c->scope_count=0;memset(&c->stats,0,sizeof(c->stats));memset(c->stack,0,sizeof(c->stack));return 1;
}
static MxDirtTsToken push(MxDirtTerrainScope *c,uint64_t gen,uint32_t thread){
    MxDirtTsToken none={0};
    if(!c||thread!=c->config.owner_thread)return none;
    if(gen!=c->config.generation){c->poisoned=1;c->stats.rejected_entries++;return none;}
    if(c->poisoned)return none;
    if(c->depth==MXDIRT_TS_DEPTH||c->serial==UINT64_MAX){c->poisoned=1;c->stats.overflow++;return none;}
    MxDirtTsFrame *f=&c->stack[c->depth++];memset(f,0,sizeof(*f));
    f->token.generation=gen;f->token.serial=++c->serial;f->token.depth=c->depth;
    f->scope=MXDIRT_TS_NONE;return f->token;
}
/* Native layout: model+0x10c == 1 with bit 0x100 of +0x110 clear marks a drawn
   heightfield model, model+0x18f8 points back at h. h+0: width, height,
   base grid, render (current) grid, extent x/z, height scale, origin xyz. */
static int terrain_read(MxDirtTerrainScope *c,MxDirtTsTerrain *t,uintptr_t h,uintptr_t m,
    uintptr_t v,uintptr_t matrix,uint32_t flags){
    unsigned char model[8],hp[8],b[0x30];
    if(!h||!m||!v||!at(c,m,0x10c,model,sizeof(model))||u32(model)!=1||(u32(model+4)&0x100)||
       !at(c,m,0x18f8,hp,8)||ptr(hp)!=h||!at(c,h,0,b,sizeof(b))||
       !at(c,matrix,0,t->view,sizeof(t->view)))return 0;
    t->h=h;t->model=m;t->view_context=v;t->flags=flags;
    t->width=u32(b);t->height=u32(b+4);t->base_grid=ptr(b+8);t->render_grid=ptr(b+0x10);
    t->extent_x=f32(b+0x18);t->extent_z=f32(b+0x1c);t->height_scale=f32(b+0x20);
    for(unsigned k=0;k<3;k++)t->origin[k]=f32(b+0x24+4*k);
    if(t->width<2||t->width>8193||t->height<2||t->height>8193||!t->base_grid||!t->render_grid||
       !(t->extent_x>0)||!(t->extent_z>0)||!isfinite(t->extent_x)||!isfinite(t->extent_z)||
       !isfinite(t->height_scale)||t->height_scale<0)return 0;
    for(unsigned k=0;k<3;k++)if(!isfinite(t->origin[k]))return 0;
    for(unsigned k=0;k<16;k++)if(!isfinite(t->view[k]))return 0;
    return 1;
}
MxDirtTsToken mxdirt_ts_enter_terrain(MxDirtTerrainScope *c,uint64_t gen,uint32_t thread,
    uint32_t call,uintptr_t h,uintptr_t m,uintptr_t v,uintptr_t matrix,uint32_t flags){
    MxDirtTsToken token=push(c,gen,thread);if(!token.serial)return token;
    MxDirtTsTerrain t;memset(&t,0,sizeof(t));
    if(call!=MXDIRT_TS_TERRAIN_CALL||!terrain_read(c,&t,h,m,v,matrix,flags))c->stats.rejected_entries++;
    else if(c->scope_count==MXDIRT_TS_SCOPES)c->stats.dropped_scopes++;
    else {uint32_t i=c->scope_count++;c->scopes[i]=t;c->stack[c->depth-1].scope=i;}
    return token;
}
int mxdirt_ts_leave(MxDirtTerrainScope *c,uint32_t thread,MxDirtTsToken token){
    if(!token.serial)return 1;
    if(!c||thread!=c->config.owner_thread)return 0;
    if(!c->depth||token.generation!=c->config.generation||token.depth!=c->depth||
       token.serial!=c->stack[c->depth-1].token.serial){c->stats.unbalanced++;c->poisoned=1;return 0;}
    memset(&c->stack[--c->depth],0,sizeof(c->stack[0]));return 1;
}

/* Code bytes around the outer terrain call (ending in the CALL itself) and
   the prologue of the called terrain draw function. */
static const unsigned char outer_guard[]={
 0x83,0xb8,0x0c,1,0,0,1,0x75,0x2e,0x48,0x8b,0x88,0xf8,0x18,0,0,0x48,0x85,0xc9,
 0x74,0x22,0x0f,0xba,0xa0,0x10,1,0,0,8,0x72,0x18,0x4c,0x8d,0x8c,0x24,0xd0,0,0,0,
 0x4c,0x8b,0xc3,0x48,0x8b,0xd0,0x44,0x89,0x64,0x24,0x20,0xe8,8,0x1c,1,0};
static const unsigned char terrain_guard[]={
 0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x56,0x57,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0xc0,1,0,0};
static const unsigned char terrain_call[5]={0xe8,8,0x1c,1,0};
enum { OUTER_GUARD=0x24f5e1u, TERRAIN_FUNCTION=0x261220u };
static int image_match(const MxDirtTsImage *v,uint32_t off,const void *expected,size_t n){
    unsigned char got[80];
    return n<=sizeof(got)&&off<=v->image_bytes&&n<=v->image_bytes-off&&
      copy(v->read,v->read_user,v->image+off,got,n)&&!memcmp(got,expected,n);
}
static int rel32(uintptr_t to,uintptr_t from,int32_t *out){
    if(to>=from){if(to-from>INT32_MAX)return 0;*out=(int32_t)(to-from);}
    else {if(from-to>(uint64_t)INT32_MAX+1)return 0;*out=(int32_t)(-(int64_t)(from-to));}
    return 1;
}
int mxdirt_ts_plan_terrain_call(const MxDirtTsImage *v,uintptr_t wrapper,uintptr_t relay,MxDirtTsCallPlan *out){
    _Static_assert(OUTER_GUARD+sizeof(outer_guard)==MXDIRT_TS_TERRAIN_CALL+5,"guard ends with the call");
    if(!v||!out||!wrapper||!v->image||!v->executable_identity_verified||
       v->image_bytes<TERRAIN_FUNCTION+sizeof(terrain_guard)||v->image>UINTPTR_MAX-v->image_bytes||
       !image_match(v,OUTER_GUARD,outer_guard,sizeof(outer_guard))||
       !image_match(v,TERRAIN_FUNCTION,terrain_guard,sizeof(terrain_guard))||
       !image_match(v,MXDIRT_TS_TERRAIN_CALL,terrain_call,5))return 0;
    MxDirtTsCallPlan p;memset(&p,0,sizeof(p));p.site=v->image+MXDIRT_TS_TERRAIN_CALL;p.resume=p.site+5;
    p.target=v->image+TERRAIN_FUNCTION;p.wrapper=wrapper;memcpy(p.original,terrain_call,5);
    int32_t d;
    /* The call's rel32 must land on TERRAIN_FUNCTION. */
    int32_t native;memcpy(&native,terrain_call+1,4);if((uintptr_t)((intptr_t)p.resume+native)!=p.target)return 0;
    if(!rel32(wrapper,p.resume,&d)){
        if(!relay||relay>UINTPTR_MAX-14||!rel32(relay,p.resume,&d))return 0;
        p.relay=relay;p.relay_size=14;p.relay_bytes[0]=0xff;p.relay_bytes[1]=0x25;
        memcpy(p.relay_bytes+6,&wrapper,8); /* jmp [rip+0]; absolute wrapper */
    }
    p.patch[0]=0xe8;memcpy(p.patch+1,&d,4);*out=p;return 1;
}
