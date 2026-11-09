#include "mxdirt_dirt_gl.h"
#include "mxdirt_dirt_shader.h"
#include <GL/gl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
/* GL enums beyond the 1.1 header. */
enum {
 X_TEXTURE0=0x84c0,X_ACTIVE_TEXTURE=0x84e0,X_CURRENT_PROGRAM=0x8b8d,X_SAMPLER_BINDING=0x8919,
 X_ARRAY_BUFFER=0x8892,X_ELEMENT_ARRAY_BUFFER=0x8893,X_ARRAY_BUFFER_BINDING=0x8894,X_VERTEX_ARRAY_BINDING=0x85b5,
 X_PIXEL_UNPACK_BUFFER=0x88ec,X_PIXEL_UNPACK_BUFFER_BINDING=0x88ef,X_STATIC_DRAW=0x88e4,X_BUFFER_SIZE=0x8764,
 X_VERTEX_SHADER=0x8b31,X_FRAGMENT_SHADER=0x8b30,X_COMPILE_STATUS=0x8b81,X_LINK_STATUS=0x8b82,
 X_MAJOR_VERSION=0x821b,X_CONTEXT_PROFILE_MASK=0x9126,
 X_CLAMP_TO_EDGE=0x812f,X_TEXTURE_MIN_LOD=0x813a,X_TEXTURE_MAX_LOD=0x813b,X_TEXTURE_MAX_LEVEL=0x813d,
 X_MAX_ANISOTROPY=0x84fe,X_MAX_MAX_ANISOTROPY=0x84ff,
 X_RG16=0x822c,X_RG8=0x822b,X_RG=0x8227,
 X_DRAW_FRAMEBUFFER_BINDING=0x8ca6,X_READ_FRAMEBUFFER_BINDING=0x8caa,X_READ_FRAMEBUFFER=0x8ca8,X_DRAW_BUFFER0=0x8825,X_SAMPLES=0x80a9,
 X_TRANSFORM_FEEDBACK_ACTIVE=0x8e24,X_RASTERIZER_DISCARD=0x8c89,X_QUERY_ACTIVE=0x8865,
 X_BLEND_DST_RGB=0x80c8,X_BLEND_SRC_RGB=0x80c9,X_BLEND_DST_ALPHA=0x80ca,X_BLEND_SRC_ALPHA=0x80cb,X_BLEND_EQUATION_RGB=0x8009,X_BLEND_EQUATION_ALPHA=0x883d,
 X_SAMPLE_ALPHA_TO_COVERAGE=0x809e,X_SAMPLE_ALPHA_TO_ONE=0x809f,X_FRAMEBUFFER_SRGB=0x8db9,
 X_POLYGON_OFFSET_FACTOR=0x8038,X_POLYGON_OFFSET_UNITS=0x2a00
};
#define FN(ret,name,args) ret(APIENTRY *name)args
enum { GRID_VERTICES=(MXDIRT_CELLS+1)*(MXDIRT_CELLS+1), GRID_INDICES=MXDIRT_CELLS*MXDIRT_CELLS*6 };
/* Texture units: 0 heights, 1 native colour snapshot, 2 clods, 3 sim state, 4 tyre prints, 5 soft layer. */
enum { UNITS=6 };
/* Soft layer window: SOFT_SIDE^2 texels in SOFT_SLOTS^2 tile slots, slot = tile index mod SOFT_SLOTS. */
enum { SOFT_TILE=32,SOFT_SLOTS=64,SOFT_SIDE=SOFT_TILE*SOFT_SLOTS };
enum { U_VIEW,U_PROJECTION,U_HEIGHTS,U_BACKGROUND,U_CLODS,U_VIEWPORT,U_TEXEL0,U_CAMERA,U_SPACING,U_HEIGHT_SCALE,U_FADE,U_BOUNDS,U_STRENGTH,U_LOOSE,U_STATE,U_STATE_ON,U_PRINTS,
 U_SOFT,U_SOFT_ON,U_SOFT_ORIGIN,U_SOFT_WINDOW,U_SOFT_CELL,U_SOFT_DEPTH,U_SOFT_UNIT,U_COUNT };
static const char *const uniform_names[U_COUNT]={"view","projection","heights","background","clods","viewport_origin","texel0","camera","spacing","height_scale","fade","bounds","strength","base_grain","state","state_on","prints",
 "soft","soft_on","soft_origin","soft_window","soft_cell","soft_depth","soft_unit"};
/* Bindings this pass changes and must hand back unchanged. */
typedef struct {GLint program,active,unpack_buffer,unpack[6],texture[UNITS],sampler[UNITS];} SavedBindings;
static const GLenum unpack_names[6]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,GL_UNPACK_SWAP_BYTES,GL_UNPACK_LSB_FIRST};
struct MxDirtGl {
 HGLRC context;DWORD thread;uint64_t generation;
 GLuint program,vao,vbo,ibo,heights,background,clods,state,prints,soft,linear,nearest,mipped,wrapped;
 GLint width,height,uniform[U_COUNT];int heights_ready,clods_ready,state_ready,prints_ready,soft_ready;
 uint32_t soft_slot[SOFT_SLOTS*SOFT_SLOTS],soft_slot_serial[SOFT_SLOTS*SOFT_SLOTS]; /* tile id+1 (UINT32_MAX = outside the terrain) and serial in each slot */
 FN(void,active_texture,(GLenum));FN(void,use_program,(GLuint));FN(void,bind_sampler,(GLuint,GLuint));FN(void,bind_buffer,(GLenum,GLuint));
 FN(void,get_indexed,(GLenum,GLuint,GLint*));FN(void,program_iv,(GLuint,GLenum,GLint*));
 FN(void,gen_samplers,(GLsizei,GLuint*));FN(void,delete_samplers,(GLsizei,const GLuint*));FN(void,sampler_i,(GLuint,GLenum,GLint));FN(void,sampler_f,(GLuint,GLenum,GLfloat));
 FN(GLuint,shader,(GLenum));FN(void,source,(GLuint,GLsizei,const char*const*,const GLint*));FN(void,compile,(GLuint));FN(void,shader_iv,(GLuint,GLenum,GLint*));FN(void,delete_shader,(GLuint));
 FN(GLuint,program_create,(void));FN(void,attach,(GLuint,GLuint));FN(void,link,(GLuint));FN(void,delete_program,(GLuint));FN(GLint,location,(GLuint,const char*));
 FN(void,ui,(GLint,GLint));FN(void,u2i,(GLint,GLint,GLint));FN(void,u1f,(GLint,GLfloat));FN(void,u2f,(GLint,GLfloat,GLfloat));FN(void,u3f,(GLint,GLfloat,GLfloat,GLfloat));FN(void,u4f,(GLint,GLfloat,GLfloat,GLfloat,GLfloat));FN(void,matrix,(GLint,GLsizei,GLboolean,const GLfloat*));
 FN(void,gen_buffers,(GLsizei,GLuint*));FN(void,delete_buffers,(GLsizei,const GLuint*));FN(void,buffer_data,(GLenum,intptr_t,const void*,GLenum));FN(void,buffer_iv,(GLenum,GLenum,GLint*));
 FN(void,gen_vao,(GLsizei,GLuint*));FN(void,delete_vao,(GLsizei,const GLuint*));FN(void,bind_vao,(GLuint));FN(void,attribute,(GLuint,GLint,GLenum,GLboolean,GLsizei,const void*));FN(void,enable_attribute,(GLuint));
 FN(GLboolean,is_indexed,(GLenum,GLuint));FN(void,boolean_indexed,(GLenum,GLuint,GLboolean*));FN(void,enable_indexed,(GLenum,GLuint));FN(void,disable_indexed,(GLenum,GLuint));FN(void,color,(GLuint,GLboolean,GLboolean,GLboolean,GLboolean));
 FN(void,blend,(GLuint,GLenum,GLenum,GLenum,GLenum));FN(void,equation,(GLuint,GLenum,GLenum));FN(void,query,(GLenum,GLenum,GLint*));
 FN(void,bind_framebuffer,(GLenum,GLuint));FN(void,generate_mipmap,(GLenum));
};
static int owner(const MxDirtGl *p){return p&&p->context&&p->context==wglGetCurrentContext()&&p->thread==GetCurrentThreadId();}
static void toggle(GLenum key,GLboolean v){if(v)glEnable(key);else glDisable(key);}
static void save_bindings(const MxDirtGl *p,SavedBindings *s){
 memset(s,0,sizeof(*s));glGetIntegerv(X_CURRENT_PROGRAM,&s->program);glGetIntegerv(X_ACTIVE_TEXTURE,&s->active);
 glGetIntegerv(X_PIXEL_UNPACK_BUFFER_BINDING,&s->unpack_buffer);for(unsigned k=0;k<6;k++)glGetIntegerv(unpack_names[k],&s->unpack[k]);
 for(unsigned k=0;k<UNITS;k++){p->active_texture(X_TEXTURE0+k);glGetIntegerv(GL_TEXTURE_BINDING_2D,&s->texture[k]);p->get_indexed(X_SAMPLER_BINDING,k,&s->sampler[k]);}
 p->active_texture((GLenum)s->active);
}
/* Our program, textures on units 0..UNITS-1, no unpack PBO, tightly packed CPU uploads. */
static void apply_bindings(const MxDirtGl *p,const GLuint *textures,const GLuint *samplers){
 p->use_program(p->program);
 for(unsigned k=0;k<UNITS;k++){p->active_texture(X_TEXTURE0+k);glBindTexture(GL_TEXTURE_2D,textures[k]);p->bind_sampler(k,samplers[k]);}
 p->bind_buffer(X_PIXEL_UNPACK_BUFFER,0);for(unsigned k=0;k<6;k++)glPixelStorei(unpack_names[k],k?0:1);
}
static void restore_bindings(const MxDirtGl *p,const SavedBindings *s){
 p->use_program((GLuint)s->program);
 for(unsigned k=0;k<UNITS;k++){p->active_texture(X_TEXTURE0+k);glBindTexture(GL_TEXTURE_2D,(GLuint)s->texture[k]);p->bind_sampler(k,(GLuint)s->sampler[k]);}
 p->active_texture((GLenum)s->active);p->bind_buffer(X_PIXEL_UNPACK_BUFFER,(GLuint)s->unpack_buffer);
 for(unsigned k=0;k<6;k++)glPixelStorei(unpack_names[k],s->unpack[k]);
}
static GLuint make_sampler(MxDirtGl *p,GLint min,GLint mag,GLint wrap,GLfloat anisotropy){
 GLuint s=0;p->gen_samplers(1,&s);if(!s)return 0;
 p->sampler_i(s,GL_TEXTURE_MIN_FILTER,min);p->sampler_i(s,GL_TEXTURE_MAG_FILTER,mag);
 p->sampler_i(s,GL_TEXTURE_WRAP_S,wrap);p->sampler_i(s,GL_TEXTURE_WRAP_T,wrap);
 p->sampler_f(s,X_TEXTURE_MIN_LOD,-1000.f);p->sampler_f(s,X_TEXTURE_MAX_LOD,1000.f);
 if(anisotropy>1.f)p->sampler_f(s,X_MAX_ANISOTROPY,anisotropy);
 return s;
}
int mxdirt_gl_destroy(MxDirtGl *p){
 if(!p)return 1;if(!owner(p))return 0;GLint program=0,vao=0,buffer=0;glGetIntegerv(X_CURRENT_PROGRAM,&program);glGetIntegerv(X_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(X_ARRAY_BUFFER_BINDING,&buffer);
 if((p->program&&program==(GLint)p->program)||(p->vao&&vao==(GLint)p->vao)||(p->vbo&&buffer==(GLint)p->vbo))return 0;
 GLuint textures[6]={p->heights,p->background,p->clods,p->state,p->prints,p->soft};for(unsigned k=0;k<6;k++)if(textures[k])glDeleteTextures(1,textures+k);
 GLuint samplers[4]={p->linear,p->nearest,p->mipped,p->wrapped};for(unsigned k=0;k<4;k++)if(samplers[k]&&p->delete_samplers)p->delete_samplers(1,samplers+k);
 if(p->program&&p->delete_program)p->delete_program(p->program);if(p->vao&&p->delete_vao)p->delete_vao(1,&p->vao);
 GLuint buffers[2]={p->vbo,p->ibo};for(unsigned k=0;k<2;k++)if(buffers[k]&&p->delete_buffers)p->delete_buffers(1,buffers+k);
 free(p);return 1;
}
/* Where the last create failed (logged by the content module): 1 context, 2 GL
   version/profile, 3 entry points, 4 shader compile/link, 5 uniforms,
   6 samplers, 7 textures, 8 mesh. */
static int gl_stage,gl_major,gl_profile;
void mxdirt_gl_last_failure(int *stage,int *major,int *profile){if(stage)*stage=gl_stage;if(major)*major=gl_major;if(profile)*profile=gl_profile;}
int mxdirt_gl_create(MxDirtGl **out,uint64_t generation){
 if(!out||!generation)return 0;gl_stage=1;
 if(!wglGetCurrentContext())return 0;
 MxDirtGl *p=calloc(1,sizeof(*p));if(!p)return 0;p->context=wglGetCurrentContext();p->thread=GetCurrentThreadId();
 float *cells=NULL;uint32_t *indices=NULL;
 /* A legacy (wglCreateContext) context reports no profile bits on some drivers and
    behind ReShade's opengl32 - it IS compatibility; only a pure core profile is refused. */
 GLint major=0,profile=0;glGetIntegerv(X_MAJOR_VERSION,&major);glGetIntegerv(X_CONTEXT_PROFILE_MASK,&profile);gl_major=major;gl_profile=profile;gl_stage=2;
 if(major<4||((profile&1)&&!(profile&2)))goto fail;gl_stage=3;
#define LOAD(field,name) do{PROC f=wglGetProcAddress(name);if((uintptr_t)f<=3||(uintptr_t)f==UINTPTR_MAX)goto fail;memcpy(&p->field,&f,sizeof(f));}while(0)
 LOAD(active_texture,"glActiveTexture");LOAD(use_program,"glUseProgram");LOAD(bind_sampler,"glBindSampler");LOAD(bind_buffer,"glBindBuffer");
 LOAD(get_indexed,"glGetIntegeri_v");LOAD(program_iv,"glGetProgramiv");
 LOAD(gen_samplers,"glGenSamplers");LOAD(delete_samplers,"glDeleteSamplers");LOAD(sampler_i,"glSamplerParameteri");LOAD(sampler_f,"glSamplerParameterf");
 LOAD(shader,"glCreateShader");LOAD(source,"glShaderSource");LOAD(compile,"glCompileShader");LOAD(shader_iv,"glGetShaderiv");LOAD(delete_shader,"glDeleteShader");LOAD(program_create,"glCreateProgram");LOAD(attach,"glAttachShader");LOAD(link,"glLinkProgram");LOAD(delete_program,"glDeleteProgram");LOAD(location,"glGetUniformLocation");
 LOAD(ui,"glUniform1i");LOAD(u2i,"glUniform2i");LOAD(u1f,"glUniform1f");LOAD(u2f,"glUniform2f");LOAD(u3f,"glUniform3f");LOAD(u4f,"glUniform4f");LOAD(matrix,"glUniformMatrix4fv");
 LOAD(gen_buffers,"glGenBuffers");LOAD(delete_buffers,"glDeleteBuffers");LOAD(buffer_data,"glBufferData");LOAD(buffer_iv,"glGetBufferParameteriv");
 LOAD(gen_vao,"glGenVertexArrays");LOAD(delete_vao,"glDeleteVertexArrays");LOAD(bind_vao,"glBindVertexArray");LOAD(attribute,"glVertexAttribPointer");LOAD(enable_attribute,"glEnableVertexAttribArray");
 LOAD(is_indexed,"glIsEnabledi");LOAD(boolean_indexed,"glGetBooleani_v");LOAD(enable_indexed,"glEnablei");LOAD(disable_indexed,"glDisablei");LOAD(color,"glColorMaski");LOAD(blend,"glBlendFuncSeparatei");LOAD(equation,"glBlendEquationSeparatei");LOAD(query,"glGetQueryiv");
 LOAD(bind_framebuffer,"glBindFramebuffer");LOAD(generate_mipmap,"glGenerateMipmap");
#undef LOAD
 gl_stage=4;const char *source[]={mxdirt_vertex_shader,mxdirt_fragment_shader};GLuint shaders[2]={0};int good=1;
 for(unsigned k=0;k<2;k++){shaders[k]=p->shader(k?X_FRAGMENT_SHADER:X_VERTEX_SHADER);if(!shaders[k]){good=0;break;}p->source(shaders[k],1,source+k,NULL);p->compile(shaders[k]);GLint status=0;p->shader_iv(shaders[k],X_COMPILE_STATUS,&status);if(!status){good=0;break;}}
 if(good){p->program=p->program_create();if(!p->program)good=0;else{p->attach(p->program,shaders[0]);p->attach(p->program,shaders[1]);p->link(p->program);GLint status=0;p->program_iv(p->program,X_LINK_STATUS,&status);good=status;}}
 for(unsigned k=0;k<2;k++)if(shaders[k])p->delete_shader(shaders[k]);if(!good)goto fail;
 gl_stage=5;for(unsigned k=0;k<U_COUNT;k++)if((p->uniform[k]=p->location(p->program,uniform_names[k]))<0)goto fail;
 gl_stage=6;GLfloat max_aniso=0;glGetFloatv(X_MAX_MAX_ANISOTROPY,&max_aniso);
 p->linear=make_sampler(p,GL_LINEAR,GL_LINEAR,X_CLAMP_TO_EDGE,1.f);p->nearest=make_sampler(p,GL_NEAREST,GL_NEAREST,X_CLAMP_TO_EDGE,1.f);
 p->mipped=make_sampler(p,GL_LINEAR_MIPMAP_LINEAR,GL_LINEAR,GL_REPEAT,max_aniso>=4.f?4.f:1.f);
 p->wrapped=make_sampler(p,GL_LINEAR,GL_LINEAR,GL_REPEAT,1.f); /* toroidal soft-layer window */
 if(!p->linear||!p->nearest||!p->mipped||!p->wrapped)goto fail;
 /* Names only; storage is allocated inside the draw scope after unpack state
    has been normalized. Binding once makes each name a GL_TEXTURE_2D. */
 gl_stage=7;GLint previous_texture=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous_texture);GLuint names[6]={0};glGenTextures(6,names);
 p->heights=names[0];p->background=names[1];p->clods=names[2];p->state=names[3];p->prints=names[4];p->soft=names[5];
 for(unsigned k=0;k<6;k++)if(!names[k]){glBindTexture(GL_TEXTURE_2D,(GLuint)previous_texture);goto fail;}
 for(unsigned k=0;k<6;k++)glBindTexture(GL_TEXTURE_2D,names[k]);glBindTexture(GL_TEXTURE_2D,(GLuint)previous_texture);
 gl_stage=8;cells=malloc(sizeof(float)*2*GRID_VERTICES);indices=malloc(sizeof(uint32_t)*GRID_INDICES);if(!cells||!indices)goto fail;
 for(unsigned j=0,n=0;j<=MXDIRT_CELLS;j++)for(unsigned i=0;i<=MXDIRT_CELLS;i++,n++){cells[2*n]=(float)i;cells[2*n+1]=(float)j;}
 for(unsigned j=0,n=0;j<MXDIRT_CELLS;j++)for(unsigned i=0;i<MXDIRT_CELLS;i++){uint32_t a=j*(MXDIRT_CELLS+1)+i,b=a+1,c=a+MXDIRT_CELLS+1,d=c+1;
  indices[n++]=a;indices[n++]=c;indices[n++]=b;indices[n++]=b;indices[n++]=c;indices[n++]=d;}
 GLint vao=0,buffer=0;glGetIntegerv(X_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(X_ARRAY_BUFFER_BINDING,&buffer);p->gen_vao(1,&p->vao);GLuint objects[2]={0};p->gen_buffers(2,objects);p->vbo=objects[0];p->ibo=objects[1];
 if(!p->vao||!p->vbo||!p->ibo)goto fail;
 /* Our VAO is bound before the element buffer so the host VAO never changes. */
 p->bind_vao(p->vao);p->bind_buffer(X_ARRAY_BUFFER,p->vbo);p->buffer_data(X_ARRAY_BUFFER,(intptr_t)(sizeof(float)*2*GRID_VERTICES),cells,X_STATIC_DRAW);
 p->bind_buffer(X_ELEMENT_ARRAY_BUFFER,p->ibo);p->buffer_data(X_ELEMENT_ARRAY_BUFFER,(intptr_t)(sizeof(uint32_t)*GRID_INDICES),indices,X_STATIC_DRAW);
 GLint vbytes=0,ibytes=0;p->buffer_iv(X_ARRAY_BUFFER,X_BUFFER_SIZE,&vbytes);p->buffer_iv(X_ELEMENT_ARRAY_BUFFER,X_BUFFER_SIZE,&ibytes);
 good=vbytes==(GLint)(sizeof(float)*2*GRID_VERTICES)&&ibytes==(GLint)(sizeof(uint32_t)*GRID_INDICES);
 p->attribute(0,2,GL_FLOAT,GL_FALSE,8,(void*)0);p->enable_attribute(0);
 p->bind_vao((GLuint)vao);p->bind_buffer(X_ARRAY_BUFFER,(GLuint)buffer);if(!good)goto fail;
 free(cells);free(indices);p->generation=generation;*out=p;gl_stage=0;return 1;
fail:free(cells);free(indices);mxdirt_gl_destroy(p);return 0;
}
/* Allocates a SIDE x SIDE (or side x side) texture on the bound unit once, then
   refreshes it only when the content changed. Returns 0 if storage failed. */
static int upload(int *ready,GLint internal,GLenum format,GLenum type,int side,const void *data,size_t texel_bytes,int changed){
 if(!*ready){void *neutral=NULL;if(!data){neutral=calloc((size_t)side*side,texel_bytes);if(!neutral)return 0;}
  glTexImage2D(GL_TEXTURE_2D,0,internal,side,side,0,format,type,data?data:neutral);free(neutral);glTexParameteri(GL_TEXTURE_2D,X_TEXTURE_MAX_LEVEL,0);
  GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);if(width!=side)return 0;*ready=1;}
 else if(data&&changed)glTexSubImage2D(GL_TEXTURE_2D,0,0,0,side,side,format,type,data);
 return 1;
}
/* Soft-layer window: every tile slot holds the tile it should hold at its
   latest serial; returns the window in local metres (x0,z0,x1,z1). */
static unsigned char soft_neutral[SOFT_TILE*SOFT_TILE*2];
static void soft_update(MxDirtGl *p,const MxDirtSoftInput *s,const float *modelview,const float camera[3],float out[4]){
 if(soft_neutral[0]!=128)for(int i=0;i<SOFT_TILE*SOFT_TILE;i++){soft_neutral[2*i]=128;soft_neutral[2*i+1]=0;}
 /* window centred ~10 m ahead of the camera: forward is eye -Z in world */
 float fx=-modelview[2],fz=-modelview[10],fl=hypotf(fx,fz),cx=camera[0]-s->origin_x,cz=camera[2]-s->origin_z;
 if(fl>1e-3f){cx+=fx/fl*10.f;cz+=fz/fl*10.f;}
 const float tile_m=s->cell*SOFT_TILE;
 const int32_t tx0=(int32_t)floorf(cx/tile_m)-SOFT_SLOTS/2,tz0=(int32_t)floorf(cz/tile_m)-SOFT_SLOTS/2;
 for(int j=0;j<SOFT_SLOTS;j++)for(int i=0;i<SOFT_SLOTS;i++){
  const int32_t tx=tx0+i,tz=tz0+j;const unsigned sx=(unsigned)tx&(SOFT_SLOTS-1),sz=(unsigned)tz&(SOFT_SLOTS-1),slot=sz*SOFT_SLOTS+sx;
  uint32_t id=UINT32_MAX,serial=0;const uint8_t *data=NULL;
  if(tx>=0&&tz>=0&&(uint32_t)tx<s->tiles_x&&(uint32_t)tz<s->tiles_z){size_t t=(size_t)tz*s->tiles_x+(uint32_t)tx;id=(uint32_t)t+1;serial=s->serial[t];data=s->tile[t];}
  if(p->soft_slot[slot]==id&&p->soft_slot_serial[slot]==serial)continue;
  glTexSubImage2D(GL_TEXTURE_2D,0,(GLint)(sx*SOFT_TILE),(GLint)(sz*SOFT_TILE),SOFT_TILE,SOFT_TILE,X_RG,GL_UNSIGNED_BYTE,data?data:soft_neutral);
  p->soft_slot[slot]=id;p->soft_slot_serial[slot]=serial;
 }
 out[0]=(float)tx0*tile_m;out[1]=(float)tz0*tile_m;out[2]=out[0]+SOFT_SLOTS*tile_m;out[3]=out[1]+SOFT_SLOTS*tile_m;
}
int mxdirt_gl_draw(MxDirtGl *p,const MxDirtCameraScope *scope,const MxDirtWindow *w,const MxDirtLook *look,const MxDirtState *state,const MxDirtSoftInput *soft){
 if(!owner(p)||!scope||!w||!w->texels||!look||scope->generation!=p->generation||scope->thread!=p->thread||scope->context!=p->context)return -1;
 if(!scope->serial||!scope->complete)return 0;
 const float *values[]={&look->strength,&look->base_loose,&look->fade_start,&look->fade_end,&w->spacing_x,&w->spacing_z,&w->height_scale,w->texel0,w->texel0+1,w->texel0+2,w->bounds,w->bounds+1,w->bounds+2,w->bounds+3,w->camera,w->camera+1,w->camera+2};
 for(unsigned k=0;k<sizeof(values)/sizeof(*values);k++)if(!isfinite(*values[k]))return -1;
 int state_on=state&&state->valid&&state->rgba;int prints_on=state_on&&state->prints;
 int soft_on=soft&&soft->tile&&soft->serial&&soft->side==SOFT_TILE&&soft->tiles_x&&soft->tiles_z&&soft->tiles_x<=65536&&soft->tiles_z<=65536&&
  isfinite(soft->cell)&&soft->cell>=.005f&&soft->cell<=.2f&&isfinite(soft->thickness)&&soft->thickness>0.f&&soft->thickness<=.1f&&isfinite(soft->unit)&&soft->unit>=.0001f&&soft->unit<=.005f&&isfinite(soft->origin_x)&&isfinite(soft->origin_z);
 if(look->strength<0||look->strength>2||look->base_loose<0||look->base_loose>1||!(look->fade_start>=0)||!(look->fade_end>look->fade_start)||!(w->spacing_x>0)||!(w->spacing_z>0)||!(w->height_scale>0))return -1;
 for(unsigned k=0;k<16;k++)if(!isfinite(scope->modelview[k])||!isfinite(scope->projection[k]))return 0;
 /* Still the projection the native terrain was drawn with. */
 float pr[16];glGetFloatv(GL_PROJECTION_MATRIX,pr);if(memcmp(pr,scope->projection,sizeof(pr)))return 0;
 if(look->strength<=0)return 1;
 GLboolean feedback=0;glGetBooleanv(X_TRANSFORM_FEEDBACK_ACTIVE,&feedback);if(feedback||glIsEnabled(X_RASTERIZER_DISCARD))return 0;
 /* No open occlusion/primitive/timer query would count our draw. */
 const GLenum targets[]={0x8914,0x8c2f,0x8c87,0x8c88,0x88bf};for(unsigned k=0;k<5;k++){GLint active=0;p->query(targets[k],X_QUERY_ACTIVE,&active);if(active)return 0;}
 GLint draw_fbo=0,read_fbo=0,draw_buffer=0,samples=0,viewport[4],limit=0;
 glGetIntegerv(X_DRAW_FRAMEBUFFER_BINDING,&draw_fbo);glGetIntegerv(X_READ_FRAMEBUFFER_BINDING,&read_fbo);glGetIntegerv(X_DRAW_BUFFER0,&draw_buffer);glGetIntegerv(X_SAMPLES,&samples);glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);
 if(draw_buffer==GL_NONE||(draw_fbo&&samples)||viewport[0]<0||viewport[1]<0||viewport[2]<1||viewport[3]<1||viewport[2]>limit||viewport[3]>limit||(uint64_t)viewport[2]*viewport[3]>16777216)return 0;
 SavedBindings saved;save_bindings(p,&saved);
 GLint vao=0,buffer=0,blend[6],depthfunc=0,polygon[2];GLboolean color[4],write,enabled[8],blend_enabled=p->is_indexed(GL_BLEND,0);
 glGetIntegerv(X_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(X_ARRAY_BUFFER_BINDING,&buffer);
 const GLenum factors[]={X_BLEND_SRC_RGB,X_BLEND_DST_RGB,X_BLEND_SRC_ALPHA,X_BLEND_DST_ALPHA,X_BLEND_EQUATION_RGB,X_BLEND_EQUATION_ALPHA};for(unsigned k=0;k<6;k++)p->get_indexed(factors[k],0,blend+k);
 p->boolean_indexed(GL_COLOR_WRITEMASK,0,color);glGetBooleanv(GL_DEPTH_WRITEMASK,&write);glGetIntegerv(GL_DEPTH_FUNC,&depthfunc);glGetIntegerv(GL_POLYGON_MODE,polygon);
 const GLenum caps[]={GL_DEPTH_TEST,GL_CULL_FACE,GL_ALPHA_TEST,GL_STENCIL_TEST,GL_COLOR_LOGIC_OP,GL_POLYGON_OFFSET_FILL,X_SAMPLE_ALPHA_TO_COVERAGE,X_SAMPLE_ALPHA_TO_ONE};for(unsigned k=0;k<8;k++)enabled[k]=glIsEnabled(caps[k]);
 GLboolean srgb=glIsEnabled(X_FRAMEBUFFER_SRGB);GLfloat offset_factor=0,offset_units=0;glGetFloatv(X_POLYGON_OFFSET_FACTOR,&offset_factor);glGetFloatv(X_POLYGON_OFFSET_UNITS,&offset_units);
 const GLuint textures[UNITS]={p->heights,p->background,p->clods,p->state,p->prints,p->soft},samplers[UNITS]={p->linear,p->nearest,p->mipped,p->linear,p->nearest,p->wrapped};
 int result=-1;apply_bindings(p,textures,samplers);
 p->active_texture(X_TEXTURE0+2);
 if(!p->clods_ready){unsigned char *rgba=malloc((size_t)MXDIRT_CLOD_SIDE*MXDIRT_CLOD_SIDE*4);if(!rgba)goto restore;mxdirt_clods(rgba);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,MXDIRT_CLOD_SIDE,MXDIRT_CLOD_SIDE,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);free(rgba);p->generate_mipmap(GL_TEXTURE_2D);
  GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);if(width!=MXDIRT_CLOD_SIDE)goto restore;p->clods_ready=1;}
 /* Unit3 per-texel sim state, unit4 tyre prints (texelFetch only); same layout as the heights. Neutral = untouched / no print. */
 p->active_texture(X_TEXTURE0+3);
 if(!upload(&p->state_ready,GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,MXDIRT_SIDE,state_on?state->rgba:NULL,4,state_on&&state->changed))goto restore;
 p->active_texture(X_TEXTURE0+4);
 if(!upload(&p->prints_ready,GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,MXDIRT_SIDE,prints_on?state->prints:NULL,4,prints_on&&state->changed))goto restore;
 /* Unit5 soft layer window: neutral once, then only tiles that entered the window or changed. */
 p->active_texture(X_TEXTURE0+5);float soft_window[4]={0,0,0,0};
 if(!p->soft_ready){unsigned char *n=malloc((size_t)SOFT_SIDE*SOFT_SIDE*2);if(!n)goto restore;
  for(size_t i=0;i<(size_t)SOFT_SIDE*SOFT_SIDE;i++){n[2*i]=128;n[2*i+1]=0;}
  glTexImage2D(GL_TEXTURE_2D,0,X_RG8,SOFT_SIDE,SOFT_SIDE,0,X_RG,GL_UNSIGNED_BYTE,n);free(n);glTexParameteri(GL_TEXTURE_2D,X_TEXTURE_MAX_LEVEL,0);
  GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);if(width!=SOFT_SIDE)goto restore;
  memset(p->soft_slot,0,sizeof(p->soft_slot));memset(p->soft_slot_serial,0,sizeof(p->soft_slot_serial));p->soft_ready=1;}
 if(soft_on)soft_update(p,soft,scope->modelview,w->camera,soft_window);
 p->active_texture(X_TEXTURE0);
 if(!upload(&p->heights_ready,X_RG16,X_RG,GL_UNSIGNED_SHORT,MXDIRT_SIDE,w->texels,4,(int)w->changed))goto restore;
 p->active_texture(X_TEXTURE0+1);
 if(p->width!=viewport[2]||p->height!=viewport[3]){
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,viewport[2],viewport[3],0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);glTexParameteri(GL_TEXTURE_2D,X_TEXTURE_MAX_LEVEL,0);
  GLint width=0,height=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
  if(width!=viewport[2]||height!=viewport[3])goto restore;p->width=width;p->height=height;
 }
 /* Snapshot the native colour of this viewport from the actual DRAW target,
    then restore both the source FBO's read selector and the read binding. */
 p->bind_framebuffer(X_READ_FRAMEBUFFER,(GLuint)draw_fbo);GLint source_read=0;glGetIntegerv(GL_READ_BUFFER,&source_read);
 glReadBuffer(draw_buffer==GL_FRONT_AND_BACK?GL_FRONT:(GLenum)draw_buffer);
 glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,viewport[0],viewport[1],viewport[2],viewport[3]);
 glReadBuffer((GLenum)source_read);p->bind_framebuffer(X_READ_FRAMEBUFFER,(GLuint)read_fbo);
 p->matrix(p->uniform[U_VIEW],1,GL_FALSE,scope->modelview);p->matrix(p->uniform[U_PROJECTION],1,GL_FALSE,scope->projection);
 p->ui(p->uniform[U_HEIGHTS],0);p->ui(p->uniform[U_BACKGROUND],1);p->ui(p->uniform[U_CLODS],2);p->u2i(p->uniform[U_VIEWPORT],viewport[0],viewport[1]);
 p->u3f(p->uniform[U_TEXEL0],w->texel0[0],w->texel0[1],w->texel0[2]);p->u3f(p->uniform[U_CAMERA],w->camera[0],w->camera[1],w->camera[2]);
 p->u2f(p->uniform[U_SPACING],w->spacing_x,w->spacing_z);p->u1f(p->uniform[U_HEIGHT_SCALE],w->height_scale);p->u2f(p->uniform[U_FADE],look->fade_start,look->fade_end);
 p->u4f(p->uniform[U_BOUNDS],w->bounds[0],w->bounds[1],w->bounds[2],w->bounds[3]);p->u1f(p->uniform[U_STRENGTH],look->strength);p->u1f(p->uniform[U_LOOSE],look->base_loose);
 p->ui(p->uniform[U_STATE],3);p->ui(p->uniform[U_STATE_ON],state_on);p->ui(p->uniform[U_PRINTS],4);
 p->ui(p->uniform[U_SOFT],5);p->ui(p->uniform[U_SOFT_ON],soft_on);
 p->u2f(p->uniform[U_SOFT_ORIGIN],soft_on?soft->origin_x:0.f,soft_on?soft->origin_z:0.f);
 p->u4f(p->uniform[U_SOFT_WINDOW],soft_window[0],soft_window[1],soft_window[2],soft_window[3]);
 p->u1f(p->uniform[U_SOFT_CELL],soft_on?soft->cell:.025f);p->u1f(p->uniform[U_SOFT_DEPTH],soft_on?soft->thickness:.03f);p->u1f(p->uniform[U_SOFT_UNIT],soft_on?soft->unit:.001f);
 p->disable_indexed(GL_BLEND,0);p->color(0,1,1,1,0);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);for(unsigned k=1;k<8;k++)glDisable(caps[k]);glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
 glDisable(X_FRAMEBUFFER_SRGB);glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(-1.f,-1.f);
 p->bind_vao(p->vao);glDrawElements(GL_TRIANGLES,GRID_INDICES,GL_UNSIGNED_INT,(void*)0);result=1;
restore:
 if(blend_enabled)p->enable_indexed(GL_BLEND,0);else p->disable_indexed(GL_BLEND,0);p->blend(0,(GLenum)blend[0],(GLenum)blend[1],(GLenum)blend[2],(GLenum)blend[3]);p->equation(0,(GLenum)blend[4],(GLenum)blend[5]);p->color(0,color[0],color[1],color[2],color[3]);
 for(unsigned k=0;k<8;k++)toggle(caps[k],enabled[k]);toggle(X_FRAMEBUFFER_SRGB,srgb);glPolygonOffset(offset_factor,offset_units);glDepthFunc((GLenum)depthfunc);glDepthMask(write);
 glPolygonMode(GL_FRONT,(GLenum)polygon[0]);glPolygonMode(GL_BACK,(GLenum)polygon[1]);p->bind_vao((GLuint)vao);p->bind_buffer(X_ARRAY_BUFFER,(GLuint)buffer);
 restore_bindings(p,&saved);return result;
}
