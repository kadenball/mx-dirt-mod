#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "mxdirt_patches.h"

#define ADD_SITE 0x1f61e6u
#define MATERIAL_SITE 0x1f1f65u
#define REPLACE_ENTRY 0x1f5f20u
#define ADD_ENTRY 0x1f60c0u

/* Original deformation add: clamps to INT32_MAX only (positive overflow). */
static const unsigned char old_add[]={0xb8,0xff,0xff,0xff,0x7f,0x2b,0xc2,0x44,0x3b,0xc8,0x7e,0x08,0xc7,0x01,0xff,0xff,0xff,0x7f,0xeb,0x06,0x41,0x8d,0x04,0x11,0x89,0x01};
/* mov eax,edx; add eax,r9d; jno store; mov eax,INT32_MAX; test edx,edx; jns store; mov eax,INT32_MIN; store: mov [rcx],eax; nop x3 */
static const unsigned char new_add[]={0x8b,0xc2,0x44,0x01,0xc8,0x71,0x0e,0xb8,0xff,0xff,0xff,0x7f,0x85,0xd2,0x79,0x05,0xb8,0x00,0x00,0x00,0x80,0x89,0x01,0x90,0x90,0x90};
_Static_assert(sizeof(old_add)==sizeof(new_add),"same patch length");
static const unsigned char old_material[]={0x41,0x8b,0xb3,0x70,0x07,0x00,0x00}; /* mov esi,[r11+770h] */
/* Trampoline body: maxss xmm3,xmm4 (xmm4 is zero here); the displaced mov; jmp back. */
static const unsigned char relay_prefix[]={0xf3,0x0f,0x5f,0xdc,0x41,0x8b,0xb3,0x70,0x07,0x00,0x00,0xe9};
static const unsigned char replace_entry[]={0x44,0x89,0x44,0x24,0x18,0x53,0x56,0x48,0x83,0xec,0x38,0x41,0x8b,0xc0,0x48,0x8b};
static const unsigned char add_entry[]={0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x4c,0x24,0x08,0x57,0x41,0x54,0x48,0x83,0xec};

static unsigned char *patched_exe,*relay;
static unsigned char material_jump[7];
static unsigned owned_sites;

static int readable(uintptr_t p,size_t n,int executable){
    if(!n||n>UINTPTR_MAX-p)return 0;uintptr_t end=p+n;
    while(p<end){MEMORY_BASIC_INFORMATION m;
        if(!VirtualQuery((void*)p,&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return 0;
        DWORD a=m.Protect&0xff;
        if(executable){if(a!=PAGE_EXECUTE_READ&&a!=PAGE_EXECUTE_READWRITE&&a!=PAGE_EXECUTE_WRITECOPY)return 0;}
        else if(a!=PAGE_READONLY&&a!=PAGE_READWRITE&&a!=PAGE_WRITECOPY&&a!=PAGE_EXECUTE_READ&&a!=PAGE_EXECUTE_READWRITE&&a!=PAGE_EXECUTE_WRITECOPY)return 0;
        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=p)return 0;p=next;
    }return 1;
}
static int same(uintptr_t p,const void *bytes,size_t n){return readable(p,n,1)&&!memcmp((const void*)p,bytes,n);}
static int write_code(void *p,const void *bytes,size_t n){
    DWORD old;if(!VirtualProtect(p,n,PAGE_EXECUTE_READWRITE,&old))return 0;
    memcpy(p,bytes,n);int flushed=FlushInstructionCache(GetCurrentProcess(),p,n)!=0;
    DWORD ignored;return VirtualProtect(p,n,old,&ignored)!=0&&flushed;
}
/* Material site holds `jmp rel32; nop; nop` into a relay that runs the clamp
   and jumps back to site+7 (ours or another plugin's identical patch). */
static int material_patched(unsigned char *exe){
    uintptr_t site=(uintptr_t)exe+MATERIAL_SITE;unsigned char jump[7],body[16];int32_t rel,back;
    if(!readable(site,7,1))return 0;memcpy(jump,(void*)site,7);
    if(jump[0]!=0xe9||jump[5]!=0x90||jump[6]!=0x90)return 0;
    memcpy(&rel,jump+1,4);uintptr_t target=(uintptr_t)((int64_t)site+5+rel);
    if(!readable(target,sizeof(body),1))return 0;memcpy(body,(void*)target,sizeof(body));
    if(memcmp(body,relay_prefix,sizeof(relay_prefix)))return 0;
    memcpy(&back,body+12,4);return (uintptr_t)((int64_t)target+16+back)==site+7;
}
int mxdirt_patches_present(unsigned char *exe){
    return exe&&same((uintptr_t)exe+ADD_SITE,new_add,sizeof(new_add))&&material_patched(exe);
}
unsigned mxdirt_patches_owned(void){return owned_sites;}
int mxdirt_patches_install(unsigned char *exe,const char **why){
    const char *dummy;if(!why)why=&dummy;*why="ok";
    if(!exe){*why="no_image";return MXDIRT_PATCHES_FAILED;}
    if(owned_sites){
        if(patched_exe==exe&&mxdirt_patches_present(exe))return MXDIRT_PATCHES_INSTALLED;
        *why="owned_patch_changed";return MXDIRT_PATCHES_FAILED;
    }
    if(!same((uintptr_t)exe+REPLACE_ENTRY,replace_entry,sizeof(replace_entry))||
       !same((uintptr_t)exe+ADD_ENTRY,add_entry,sizeof(add_entry))){*why="unsupported_terrain_functions";return MXDIRT_PATCHES_FAILED;}
    int add_old=same((uintptr_t)exe+ADD_SITE,old_add,sizeof(old_add));
    int add_new=!add_old&&same((uintptr_t)exe+ADD_SITE,new_add,sizeof(new_add));
    int mat_old=same((uintptr_t)exe+MATERIAL_SITE,old_material,sizeof(old_material));
    int mat_new=!mat_old&&material_patched(exe);
    if((!add_old&&!add_new)||(!mat_old&&!mat_new)){*why="unsupported_instruction_signatures";return MXDIRT_PATCHES_FAILED;}
    if(add_new&&mat_new){*why="already patched by another plugin";return MXDIRT_PATCHES_FOREIGN;}
    if(mat_old){
        /* Trampoline within rel32 reach; allocated once, kept for the process lifetime. */
        for(uintptr_t off=0x2000000;off<0x70000000&&!relay;off+=0x10000)
            relay=VirtualAlloc(exe+off,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if(!relay){*why="no_nearby_trampoline";return MXDIRT_PATCHES_FAILED;}
        unsigned char body[16];memcpy(body,relay_prefix,sizeof(relay_prefix));
        int64_t back=(int64_t)(uintptr_t)(exe+MATERIAL_SITE+7)-(int64_t)(uintptr_t)(relay+16);
        int64_t dest=(int64_t)(uintptr_t)relay-(int64_t)(uintptr_t)(exe+MATERIAL_SITE+5);
        if(back<INT32_MIN||back>INT32_MAX||dest<INT32_MIN||dest>INT32_MAX){*why="trampoline_out_of_reach";return MXDIRT_PATCHES_FAILED;}
        int32_t b32=(int32_t)back,d32=(int32_t)dest;memcpy(body+12,&b32,4);
        DWORD old;
        if(!VirtualProtect(relay,4096,PAGE_READWRITE,&old)){*why="trampoline_protect";return MXDIRT_PATCHES_FAILED;}
        memcpy(relay,body,16);
        if(!VirtualProtect(relay,4096,PAGE_EXECUTE_READ,&old)){*why="trampoline_protect";return MXDIRT_PATCHES_FAILED;}
        FlushInstructionCache(GetCurrentProcess(),relay,16);
        material_jump[0]=0xe9;memcpy(material_jump+1,&d32,4);material_jump[5]=material_jump[6]=0x90;
    }
    patched_exe=exe;
    if(add_old){
        if(!write_code(exe+ADD_SITE,new_add,sizeof(new_add))){
            if(!memcmp(exe+ADD_SITE,old_add,sizeof(old_add))){*why="write_failed";return MXDIRT_PATCHES_FAILED;}
            owned_sites|=1u;mxdirt_patches_remove();*why="write_failed";return MXDIRT_PATCHES_FAILED;
        }
        owned_sites|=1u;
    }
    if(mat_old){
        if(!write_code(exe+MATERIAL_SITE,material_jump,sizeof(material_jump))){
            if(memcmp(exe+MATERIAL_SITE,old_material,sizeof(old_material)))owned_sites|=2u;
            mxdirt_patches_remove();*why="write_failed";return MXDIRT_PATCHES_FAILED;
        }
        owned_sites|=2u;
    }
    if(!mxdirt_patches_present(exe)){mxdirt_patches_remove();*why="verify_failed";return MXDIRT_PATCHES_FAILED;}
    if(add_new||mat_new)*why="partly patched by another plugin";
    return MXDIRT_PATCHES_INSTALLED;
}
void mxdirt_patches_remove(void){
    unsigned char *exe=patched_exe;if(!exe||!owned_sites)return;
    if((owned_sites&1u)&&same((uintptr_t)exe+ADD_SITE,new_add,sizeof(new_add))&&
       write_code(exe+ADD_SITE,old_add,sizeof(old_add)))owned_sites&=~1u;
    if((owned_sites&2u)&&same((uintptr_t)exe+MATERIAL_SITE,material_jump,sizeof(material_jump))&&
       write_code(exe+MATERIAL_SITE,old_material,sizeof(old_material)))owned_sites&=~2u;
    /* A site that no longer holds our bytes was changed by someone else: drop it. */
    if((owned_sites&1u)&&!same((uintptr_t)exe+ADD_SITE,new_add,sizeof(new_add)))owned_sites&=~1u;
    if((owned_sites&2u)&&!same((uintptr_t)exe+MATERIAL_SITE,material_jump,sizeof(material_jump)))owned_sites&=~2u;
}
