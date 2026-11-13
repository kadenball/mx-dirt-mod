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
 X_RG16=0x822c,X_RG8=0x822b,X_RG=0x8227,X_R8=0x8229,
 X_DRAW_FRAMEBUFFER_BINDING=0x8ca6,X_READ_FRAMEBUFFER_BINDING=0x8caa,X_READ_FRAMEBUFFER=0x8ca8,X_DRAW_FRAMEBUFFER=0x8ca9,X_DRAW_BUFFER0=0x8825,X_SAMPLES=0x80a9,
 X_COLOR_ATTACHMENT0=0x8ce0,X_FRAMEBUFFER_COMPLETE=0x8cd5,
 X_TRANSFORM_FEEDBACK_ACTIVE=0x8e24,X_RASTERIZER_DISCARD=0x8c89,X_QUERY_ACTIVE=0x8865,
 X_BLEND_DST_RGB=0x80c8,X_BLEND_SRC_RGB=0x80c9,X_BLEND_DST_ALPHA=0x80ca,X_BLEND_SRC_ALPHA=0x80cb,X_BLEND_EQUATION_RGB=0x8009,X_BLEND_EQUATION_ALPHA=0x883d,
 X_SAMPLE_ALPHA_TO_COVERAGE=0x809e,X_SAMPLE_ALPHA_TO_ONE=0x809f,X_FRAMEBUFFER_SRGB=0x8db9,
 X_POLYGON_OFFSET_FACTOR=0x8038,X_POLYGON_OFFSET_UNITS=0x2a00
};
#ifndef MXDIRT_STATE_RECHECK
#define MXDIRT_STATE_RECHECK 30u
#endif
#define FN(ret,name,args) ret(APIENTRY *name)args
enum { GRID_VERTICES=(MXDIRT_CELLS+1)*(MXDIRT_CELLS+1), GRID_INDICES=MXDIRT_CELLS*MXDIRT_CELLS*6 };
/* Texture units: 0 heights, 1 native colour snapshot, 2 clods, 3 sim state, 4 soft layer, 5 soft tile occupancy. */
enum { UNITS=6 };
/* Soft layer window: SOFT_SIDE^2 texels in SOFT_SLOTS^2 tile slots, slot = tile index mod SOFT_SLOTS. */
enum { SOFT_TILE=32,SOFT_SLOTS=64,SOFT_SIDE=SOFT_TILE*SOFT_SLOTS };
enum { U_VIEW,U_PROJECTION,U_HEIGHTS,U_BACKGROUND,U_CLODS,U_VIEWPORT,U_TEXEL0,U_CAMERA,U_SPACING,U_HEIGHT_SCALE,U_FADE,U_BOUNDS,U_STRENGTH,U_LOOSE,U_STATE,U_STATE_ON,
 U_SOFT,U_SOFT_ON,U_SOFT_ORIGIN,U_SOFT_WINDOW,U_SOFT_CELL,U_SOFT_DEPTH,U_SOFT_UNIT,U_SOFT_MODE,U_SOFT_OCC,U_SOFT_TILE,U_SHIFT,U_COUNT };
static const char *const uniform_names[U_COUNT]={"view","projection","heights","background","clods","viewport_origin","texel0","camera","spacing","height_scale","fade","bounds","strength","base_grain","state","state_on",
 "soft","soft_on","soft_origin","soft_window","soft_cell","soft_depth","soft_unit","soft_mode","soft_occ","soft_tile","shift"};
/* Bindings this pass changes and must hand back unchanged. */
typedef struct {GLint program,active,unpack_buffer,unpack[6],texture[UNITS],sampler[UNITS];} SavedBindings;
static const GLenum unpack_names[6]={GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS,GL_UNPACK_SWAP_BYTES,GL_UNPACK_LSB_FIRST};
struct MxDirtGl {
 HGLRC context;DWORD thread;uint64_t generation;
 /* program[0] colour, program[1] the depth-only tyre-sink draw (same source with SINK defined; a uniform it
    does not use has location -1, which glUniform ignores). */
 GLuint program[2],vao,vbo,ibo,heights,background,clods,state,soft,occ,linear,nearest,mipped,wrapped;
 GLint width,height,uniform[2][U_COUNT];int heights_ready,clods_ready,state_ready,soft_ready,occ_ready;
 /* Soft tile occupancy: slot_data = the slot's tile has soil data (was ever touched); the texture holds it
    widened by one slot each way (the window wraps, so does this). occ_fast: the layer's dimensions allow the
    untouched shortcut at all (else every slot reads as touched). */
 unsigned char slot_data[SOFT_SLOTS*SOFT_SLOTS];int occ_dirty,occ_fast;
 /* Multisampled draw FBO (the game's MSAA): its colour cannot be read with glCopyTexSubImage2D, so the
    snapshot is a resolve blit into this FBO, which has the snapshot texture as its colour attachment. */
 GLuint resolve_fbo;int resolving;
 /* Measurement only: GPU time of the pass, from two timestamp queries around it on the frames that already
    run the full state check (one in MXDIRT_STATE_RECHECK); read back at the next such frame, so nothing
    waits. A timestamp query is never 'active', so it cannot disturb a query of the game's. Optional. */
 GLuint timer[2];int timing,gpu_fresh;double gpu_ms;
 /* perf: the host GL state at the terrain call, queried in full only every MXDIRT_STATE_RECHECK frames (and
    after any skipped frame). Every glGet* makes NVIDIA's threaded driver drain its queue - right after the
    terrain's ~1500 draws ~65 queries a frame serialized the game's render thread with the driver. */
 struct {int valid;unsigned age;float projection[16];GLint draw_fbo,read_fbo,draw_buffer,source_read,viewport[4],vao,buffer,blend[6],depthfunc,polygon[2],samples;
  GLboolean color[4],write,enabled[8],blend_enabled,srgb,scissor;GLfloat offset_factor,offset_units;SavedBindings saved;
  /* perf: when the host state rules the pass out (an open query, a target it cannot use), nothing is asked of GL
     again for `hold` frames - a pass that cannot draw used to run every state query on every frame. */
  unsigned hold;int skip;} cache;
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
 FN(void,gen_framebuffers,(GLsizei,GLuint*));FN(void,delete_framebuffers,(GLsizei,const GLuint*));FN(void,framebuffer_texture,(GLenum,GLenum,GLenum,GLuint,GLint));
 FN(GLenum,framebuffer_status,(GLenum));FN(void,blit,(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum));
 FN(void,gen_queries,(GLsizei,GLuint*));FN(void,delete_queries,(GLsizei,const GLuint*));FN(void,query_counter,(GLuint,GLenum));FN(void,query_u64,(GLuint,GLenum,uint64_t*));
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
 p->use_program(p->program[0]);
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
 if((p->program[0]&&program==(GLint)p->program[0])||(p->program[1]&&program==(GLint)p->program[1])||(p->vao&&vao==(GLint)p->vao)||(p->vbo&&buffer==(GLint)p->vbo))return 0;
 if(p->resolve_fbo&&p->delete_framebuffers)p->delete_framebuffers(1,&p->resolve_fbo);
 if(p->timer[0]&&p->delete_queries)p->delete_queries(2,p->timer);
 GLuint textures[UNITS]={p->heights,p->background,p->clods,p->state,p->soft,p->occ};for(unsigned k=0;k<UNITS;k++)if(textures[k])glDeleteTextures(1,textures+k);
 GLuint samplers[4]={p->linear,p->nearest,p->mipped,p->wrapped};for(unsigned k=0;k<4;k++)if(samplers[k]&&p->delete_samplers)p->delete_samplers(1,samplers+k);
 for(unsigned k=0;k<2;k++)if(p->program[k]&&p->delete_program)p->delete_program(p->program[k]);if(p->vao&&p->delete_vao)p->delete_vao(1,&p->vao);
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
 LOAD(gen_framebuffers,"glGenFramebuffers");LOAD(delete_framebuffers,"glDeleteFramebuffers");LOAD(framebuffer_texture,"glFramebufferTexture2D");
 LOAD(framebuffer_status,"glCheckFramebufferStatus");LOAD(blit,"glBlitFramebuffer");
#undef LOAD
 {PROC f[4]={wglGetProcAddress("glGenQueries"),wglGetProcAddress("glDeleteQueries"),wglGetProcAddress("glQueryCounter"),wglGetProcAddress("glGetQueryObjectui64v")};int all=1;
  for(unsigned k=0;k<4;k++)if((uintptr_t)f[k]<=3||(uintptr_t)f[k]==UINTPTR_MAX)all=0;
  if(all){memcpy(&p->gen_queries,&f[0],sizeof(PROC));memcpy(&p->delete_queries,&f[1],sizeof(PROC));memcpy(&p->query_counter,&f[2],sizeof(PROC));memcpy(&p->query_u64,&f[3],sizeof(PROC));
   p->gen_queries(2,p->timer);if(!p->timer[0]||!p->timer[1]){p->timer[0]=p->timer[1]=0;}}}
 /* shaders: 0 vertex, 1 colour fragment, 2 tyre-sink fragment (the same body with SINK defined) */
 gl_stage=4;GLuint shaders[3]={0};int good=1;
 for(unsigned k=0;k<3;k++){shaders[k]=p->shader(k?X_FRAGMENT_SHADER:X_VERTEX_SHADER);if(!shaders[k]){good=0;break;}
  const char *source[3]={k?mxdirt_fragment_version:mxdirt_vertex_shader,k==2?mxdirt_fragment_sink:"\n",mxdirt_fragment_body};
  p->source(shaders[k],k?3:1,source,NULL);p->compile(shaders[k]);GLint status=0;p->shader_iv(shaders[k],X_COMPILE_STATUS,&status);if(!status){good=0;break;}}
 for(unsigned k=0;k<2&&good;k++){p->program[k]=p->program_create();if(!p->program[k])good=0;
  else{p->attach(p->program[k],shaders[0]);p->attach(p->program[k],shaders[1+k]);p->link(p->program[k]);GLint status=0;p->program_iv(p->program[k],X_LINK_STATUS,&status);good=status;}}
 for(unsigned k=0;k<3;k++)if(shaders[k])p->delete_shader(shaders[k]);if(!good)goto fail;
 gl_stage=5;for(unsigned j=0;j<2;j++)for(unsigned k=0;k<U_COUNT;k++)if((p->uniform[j][k]=p->location(p->program[j],uniform_names[k]))<0&&!j)goto fail;
 gl_stage=6;GLfloat max_aniso=0;glGetFloatv(X_MAX_MAX_ANISOTROPY,&max_aniso);
 p->linear=make_sampler(p,GL_LINEAR,GL_LINEAR,X_CLAMP_TO_EDGE,1.f);p->nearest=make_sampler(p,GL_NEAREST,GL_NEAREST,X_CLAMP_TO_EDGE,1.f);
 p->mipped=make_sampler(p,GL_LINEAR_MIPMAP_LINEAR,GL_LINEAR,GL_REPEAT,max_aniso>=4.f?4.f:1.f);
 p->wrapped=make_sampler(p,GL_LINEAR,GL_LINEAR,GL_REPEAT,1.f); /* toroidal soft-layer window */
 if(!p->linear||!p->nearest||!p->mipped||!p->wrapped)goto fail;
 /* Names only; storage is allocated inside the draw scope after unpack state
    has been normalized. Binding once makes each name a GL_TEXTURE_2D. */
 gl_stage=7;GLint previous_texture=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous_texture);GLuint names[UNITS]={0};glGenTextures(UNITS,names);
 p->heights=names[0];p->background=names[1];p->clods=names[2];p->state=names[3];p->soft=names[4];p->occ=names[5];
 for(unsigned k=0;k<UNITS;k++)if(!names[k]){glBindTexture(GL_TEXTURE_2D,(GLuint)previous_texture);goto fail;}
 for(unsigned k=0;k<UNITS;k++)glBindTexture(GL_TEXTURE_2D,names[k]);glBindTexture(GL_TEXTURE_2D,(GLuint)previous_texture);
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
/* Allocates the SIDE x SIDE window texture on the bound unit once (with everything), then sends only the
   rectangles the CPU side listed since the last upload (n > MXDIRT_RECTS: everything) and clears the list.
   Returns 0 if storage failed. */
static int upload(int *ready,GLint internal,GLenum format,GLenum type,const void *data,size_t texel_bytes,MxDirtDirty *dirty){
 const int side=MXDIRT_SIDE;
 if(!*ready){void *neutral=NULL;if(!data){neutral=calloc((size_t)side*side,texel_bytes);if(!neutral)return 0;}
  glTexImage2D(GL_TEXTURE_2D,0,internal,side,side,0,format,type,data?data:neutral);free(neutral);glTexParameteri(GL_TEXTURE_2D,X_TEXTURE_MAX_LEVEL,0);
  GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);if(width!=side)return 0;*ready=1;
  if(data&&dirty)dirty->n=0;return 1;}
 if(!data||!dirty||!dirty->n)return 1;
 if(dirty->n>MXDIRT_RECTS)glTexSubImage2D(GL_TEXTURE_2D,0,0,0,side,side,format,type,data);
 else{glPixelStorei(GL_UNPACK_ROW_LENGTH,side);
  for(uint32_t k=0;k<dirty->n;k++){const MxDirtRect *r=dirty->r+k;if((int)r->x+r->w>side||(int)r->y+r->h>side)continue;
   glTexSubImage2D(GL_TEXTURE_2D,0,r->x,r->y,r->w,r->h,format,type,(const unsigned char*)data+((size_t)r->y*side+r->x)*texel_bytes);}
  glPixelStorei(GL_UNPACK_ROW_LENGTH,0);}
 dirty->n=0;return 1;
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
  if(p->slot_data[slot]!=(data!=NULL)){p->slot_data[slot]=data!=NULL;p->occ_dirty=1;}
 }
 out[0]=(float)tx0*tile_m;out[1]=(float)tz0*tile_m;out[2]=out[0]+SOFT_SLOTS*tile_m;out[3]=out[1]+SOFT_SLOTS*tile_m;
}
int mxdirt_gl_draw(MxDirtGl *p,const MxDirtCameraScope *scope,const MxDirtWindow *w,const MxDirtLook *look,const MxDirtState *state,const MxDirtSoftInput *soft){
 if(!owner(p)||!scope||!w||!w->texels||!look||scope->generation!=p->generation||scope->thread!=p->thread||scope->context!=p->context)return -1;
 if(!scope->serial||!scope->complete)return 0;
 if(w->shift_x>=(uint32_t)MXDIRT_SIDE||w->shift_z>=(uint32_t)MXDIRT_SIDE)return -1;
 const float *values[]={&look->strength,&look->base_loose,&look->fade_start,&look->fade_end,&w->spacing_x,&w->spacing_z,&w->height_scale,w->texel0,w->texel0+1,w->texel0+2,w->bounds,w->bounds+1,w->bounds+2,w->bounds+3,w->camera,w->camera+1,w->camera+2};
 for(unsigned k=0;k<sizeof(values)/sizeof(*values);k++)if(!isfinite(*values[k]))return -1;
 int state_on=state&&state->valid&&state->rgba;
 int soft_on=soft&&soft->tile&&soft->serial&&soft->side==SOFT_TILE&&soft->tiles_x&&soft->tiles_z&&soft->tiles_x<=65536&&soft->tiles_z<=65536&&
  isfinite(soft->cell)&&soft->cell>=.005f&&soft->cell<=.2f&&isfinite(soft->thickness)&&soft->thickness>0.f&&soft->thickness<=.1f&&isfinite(soft->unit)&&soft->unit>=.0001f&&soft->unit<=.005f&&isfinite(soft->origin_x)&&isfinite(soft->origin_z);
 if(look->strength<0||look->strength>2||look->base_loose<0||look->base_loose>1||!(look->fade_start>=0)||!(look->fade_end>look->fade_start)||!(w->spacing_x>0)||!(w->spacing_z>0)||!(w->height_scale>0))return -1;
 for(unsigned k=0;k<16;k++)if(!isfinite(scope->modelview[k])||!isfinite(scope->projection[k]))return 0;
 if(look->strength<=0)return 1;
 static const GLenum caps[]={GL_DEPTH_TEST,GL_CULL_FACE,GL_ALPHA_TEST,GL_STENCIL_TEST,GL_COLOR_LOGIC_OP,GL_POLYGON_OFFSET_FILL,X_SAMPLE_ALPHA_TO_COVERAGE,X_SAMPLE_ALPHA_TO_ONE};
 if(p->cache.hold){p->cache.hold--;return 0;}
 const int check=!p->cache.valid||++p->cache.age>=MXDIRT_STATE_RECHECK;
 if(check){p->cache.valid=0;
 /* Still the projection the native terrain was drawn with (the host re-reads its camera after this). */
 float pr[16];glGetFloatv(GL_PROJECTION_MATRIX,pr);if(memcmp(pr,scope->projection,sizeof(pr))){p->cache.skip=1;return 0;}
 GLboolean feedback=0;glGetBooleanv(X_TRANSFORM_FEEDBACK_ACTIVE,&feedback);if(feedback||glIsEnabled(X_RASTERIZER_DISCARD)){p->cache.skip=2;p->cache.hold=MXDIRT_STATE_RECHECK;return 0;}
 /* No open occlusion/primitive/timer query would count our draw. */
 const GLenum targets[]={0x8914,0x8c2f,0x8c87,0x8c88,0x88bf};for(unsigned k=0;k<5;k++){GLint active=0;p->query(targets[k],X_QUERY_ACTIVE,&active);if(active){p->cache.skip=3;p->cache.hold=MXDIRT_STATE_RECHECK;return 0;}}
 GLint draw_fbo=0,read_fbo=0,draw_buffer=0,samples=0,viewport[4],limit=0;
 glGetIntegerv(X_DRAW_FRAMEBUFFER_BINDING,&draw_fbo);glGetIntegerv(X_READ_FRAMEBUFFER_BINDING,&read_fbo);glGetIntegerv(X_DRAW_BUFFER0,&draw_buffer);glGetIntegerv(X_SAMPLES,&samples);glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);
 if(draw_buffer==GL_NONE||samples<0||viewport[0]<0||viewport[1]<0||viewport[2]<1||viewport[3]<1||viewport[2]>limit||viewport[3]>limit||(uint64_t)viewport[2]*viewport[3]>16777216){
  p->cache.skip=4;p->cache.hold=MXDIRT_STATE_RECHECK;return 0;}
 p->cache.samples=samples;p->cache.scissor=glIsEnabled(GL_SCISSOR_TEST);
 save_bindings(p,&p->cache.saved);
 p->cache.blend_enabled=p->is_indexed(GL_BLEND,0);
 glGetIntegerv(X_VERTEX_ARRAY_BINDING,&p->cache.vao);glGetIntegerv(X_ARRAY_BUFFER_BINDING,&p->cache.buffer);
 const GLenum factors[]={X_BLEND_SRC_RGB,X_BLEND_DST_RGB,X_BLEND_SRC_ALPHA,X_BLEND_DST_ALPHA,X_BLEND_EQUATION_RGB,X_BLEND_EQUATION_ALPHA};for(unsigned k=0;k<6;k++)p->get_indexed(factors[k],0,p->cache.blend+k);
 p->boolean_indexed(GL_COLOR_WRITEMASK,0,p->cache.color);glGetBooleanv(GL_DEPTH_WRITEMASK,&p->cache.write);glGetIntegerv(GL_DEPTH_FUNC,&p->cache.depthfunc);glGetIntegerv(GL_POLYGON_MODE,p->cache.polygon);
 for(unsigned k=0;k<8;k++)p->cache.enabled[k]=glIsEnabled(caps[k]);
 p->cache.srgb=glIsEnabled(X_FRAMEBUFFER_SRGB);glGetFloatv(X_POLYGON_OFFSET_FACTOR,&p->cache.offset_factor);glGetFloatv(X_POLYGON_OFFSET_UNITS,&p->cache.offset_units);
 {GLint prev=0;p->bind_framebuffer(X_READ_FRAMEBUFFER,(GLuint)draw_fbo);glGetIntegerv(GL_READ_BUFFER,&prev);p->bind_framebuffer(X_READ_FRAMEBUFFER,(GLuint)read_fbo);p->cache.source_read=prev;}
 memcpy(p->cache.projection,pr,sizeof(pr));p->cache.draw_fbo=draw_fbo;p->cache.read_fbo=read_fbo;p->cache.draw_buffer=draw_buffer;memcpy(p->cache.viewport,viewport,sizeof(viewport));
 p->cache.age=0;p->cache.valid=1;p->cache.skip=0;}
 else if(memcmp(p->cache.projection,scope->projection,sizeof(p->cache.projection))){p->cache.valid=0;p->cache.skip=1;return 0;} /* the host's camera changed: re-query next frame */
 const GLint draw_fbo=p->cache.draw_fbo,read_fbo=p->cache.read_fbo,draw_buffer=p->cache.draw_buffer,*viewport=p->cache.viewport,vao=p->cache.vao,buffer=p->cache.buffer,*blend=p->cache.blend,depthfunc=p->cache.depthfunc,*polygon=p->cache.polygon;
 const GLboolean *color=p->cache.color,write=p->cache.write,*enabled=p->cache.enabled,blend_enabled=p->cache.blend_enabled,srgb=p->cache.srgb;const GLfloat offset_factor=p->cache.offset_factor,offset_units=p->cache.offset_units;
 const SavedBindings *saved=&p->cache.saved;
 const GLuint textures[UNITS]={p->heights,p->background,p->clods,p->state,p->soft,p->occ},samplers[UNITS]={p->wrapped,p->nearest,p->mipped,p->wrapped,p->wrapped,p->nearest}; /* heights and state are toroidal like the soft window */
 const int timed=check&&p->timer[0];
 if(timed){if(p->timing){uint64_t a=0,b=0;p->query_u64(p->timer[0],0x8866,&a);p->query_u64(p->timer[1],0x8866,&b);if(b>=a){p->gpu_ms=(double)(b-a)*1e-6;p->gpu_fresh=1;}}
  p->query_counter(p->timer[0],0x8e28);p->timing=1;}
 int result=-1;apply_bindings(p,textures,samplers);
 p->active_texture(X_TEXTURE0+2);
 if(!p->clods_ready){unsigned char *rgba=malloc((size_t)MXDIRT_CLOD_SIDE*MXDIRT_CLOD_SIDE*4);if(!rgba)goto restore;mxdirt_clods(rgba);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,MXDIRT_CLOD_SIDE,MXDIRT_CLOD_SIDE,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);free(rgba);p->generate_mipmap(GL_TEXTURE_2D);
  GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);if(width!=MXDIRT_CLOD_SIDE)goto restore;p->clods_ready=1;}
 /* Unit3 per-texel sim state; same layout as the heights. Neutral = untouched. */
 p->active_texture(X_TEXTURE0+3);
 if(!upload(&p->state_ready,GL_RGBA8,GL_RGBA,GL_UNSIGNED_BYTE,state_on?state->rgba:NULL,4,state_on?state->dirty:NULL))goto restore;
 /* Unit4 soft layer window: neutral once, then only tiles that entered the window or changed. */
 p->active_texture(X_TEXTURE0+4);float soft_window[4]={0,0,0,0};
 if(!p->soft_ready){unsigned char *n=malloc((size_t)SOFT_SIDE*SOFT_SIDE*2);if(!n)goto restore;
  for(size_t i=0;i<(size_t)SOFT_SIDE*SOFT_SIDE;i++){n[2*i]=128;n[2*i+1]=0;}
  glTexImage2D(GL_TEXTURE_2D,0,X_RG8,SOFT_SIDE,SOFT_SIDE,0,X_RG,GL_UNSIGNED_BYTE,n);free(n);glTexParameteri(GL_TEXTURE_2D,X_TEXTURE_MAX_LEVEL,0);
  GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);if(width!=SOFT_SIDE)goto restore;
  memset(p->soft_slot,0,sizeof(p->soft_slot));memset(p->soft_slot_serial,0,sizeof(p->soft_slot_serial));memset(p->slot_data,0,sizeof(p->slot_data));p->occ_dirty=1;p->soft_ready=1;}
 if(soft_on)soft_update(p,soft,scope->modelview,w->camera,soft_window);
 /* Unit5 soft tile occupancy (see the shader). The shortcut needs every soft sample of a pixel to lie within
    one tile of the two points the shader tests: the march covers 5 x half its height range sideways at most
    (dv <= 5), the shading 4.4 cm x 1/.12 along the sun plus 2.5 cells around; each plus bilinear reach. */
 p->active_texture(X_TEXTURE0+5);
 if(!p->occ_ready){static unsigned char full[SOFT_SLOTS*SOFT_SLOTS];memset(full,255,sizeof(full));
  glTexImage2D(GL_TEXTURE_2D,0,X_R8,SOFT_SLOTS,SOFT_SLOTS,0,GL_RED,GL_UNSIGNED_BYTE,full);glTexParameteri(GL_TEXTURE_2D,X_TEXTURE_MAX_LEVEL,0);
  GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);if(width!=SOFT_SLOTS)goto restore;p->occ_ready=1;p->occ_dirty=1;p->occ_fast=-1;}
 if(soft_on){
  const float tile_m=soft->cell*SOFT_TILE,up=fminf(127.f*soft->unit,soft->thickness*1.7f),down=fminf(127.f*soft->unit,soft->thickness*2.f);
  const float reach_march=5.f*.5f*(up+(up+down)/16.f)+2.f*soft->cell,reach_shade=.044f/.12f+4.5f*soft->cell;
  const int fast=tile_m>=1.05f*fmaxf(reach_march,reach_shade);
  if(fast!=p->occ_fast){p->occ_fast=fast;p->occ_dirty=1;}
  if(p->occ_dirty){static unsigned char wide[SOFT_SLOTS*SOFT_SLOTS];
   for(int j=0;j<SOFT_SLOTS;j++)for(int i=0;i<SOFT_SLOTS;i++){unsigned char any=!fast;
    for(int dj=-1;dj<=1&&!any;dj++)for(int di=-1;di<=1&&!any;di++)any=p->slot_data[((j+dj)&(SOFT_SLOTS-1))*SOFT_SLOTS+((i+di)&(SOFT_SLOTS-1))];
    wide[j*SOFT_SLOTS+i]=any?255:0;}
   glTexSubImage2D(GL_TEXTURE_2D,0,0,0,SOFT_SLOTS,SOFT_SLOTS,GL_RED,GL_UNSIGNED_BYTE,wide);p->occ_dirty=0;}
 }
 p->active_texture(X_TEXTURE0);
 if(!upload(&p->heights_ready,X_RG16,X_RG,GL_UNSIGNED_SHORT,w->texels,4,w->dirty))goto restore;
 p->active_texture(X_TEXTURE0+1);
 /* Snapshot the native colour of this viewport from the actual DRAW target, then restore the source FBO's
    read selector, the read binding and (resolve path) the draw binding and scissor test.
    A multisampled FBO - the game's MSAA - is read through a resolve blit into our own FBO; a blit obeys
    the scissor test, and the colour mask and sRGB conversion are set aside for it (the common restore
    below puts both back). Every other target is copied as before. */
 const int resolve=draw_fbo&&p->cache.samples>0;const GLint source_read=p->cache.source_read;
 const GLenum source_buffer=draw_buffer==GL_FRONT_AND_BACK?GL_FRONT:(GLenum)draw_buffer;
 if(resolve){p->color(0,1,1,1,1);glDisable(X_FRAMEBUFFER_SRGB);if(p->cache.scissor)glDisable(GL_SCISSOR_TEST);}
 p->bind_framebuffer(X_READ_FRAMEBUFFER,(GLuint)draw_fbo);glReadBuffer(source_buffer);
 int snapshot=1;
 if(p->width!=viewport[2]||p->height!=viewport[3]||p->resolving!=resolve){
  /* Storage at this size. A resolve blit may insist on the source's own colour format: the first format
     that blits without a GL error is kept (asked once per size, not per frame). */
  static const GLint formats[]={GL_RGBA8,GL_RGB8,0x881a,0x8c43,0x8059,0x8c3a,0x881b}; /* RGBA16F, SRGB8_ALPHA8, RGB10_A2, R11F_G11F_B10F, RGB16F */
  p->width=p->height=0;snapshot=0;
  if(resolve&&!p->resolve_fbo)p->gen_framebuffers(1,&p->resolve_fbo);
  for(unsigned k=0;k<(resolve?sizeof(formats)/sizeof(*formats):1u)&&!snapshot&&(!resolve||p->resolve_fbo);k++){
   glTexImage2D(GL_TEXTURE_2D,0,formats[k],viewport[2],viewport[3],0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);glTexParameteri(GL_TEXTURE_2D,X_TEXTURE_MAX_LEVEL,0);
   GLint width=0,height=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
   if(width!=viewport[2]||height!=viewport[3])continue;
   if(!resolve){snapshot=1;break;}
   for(unsigned e=0;e<16&&glGetError()!=GL_NO_ERROR;e++){}
   p->bind_framebuffer(X_DRAW_FRAMEBUFFER,p->resolve_fbo);p->framebuffer_texture(X_DRAW_FRAMEBUFFER,X_COLOR_ATTACHMENT0,GL_TEXTURE_2D,p->background,0);
   if(p->framebuffer_status(X_DRAW_FRAMEBUFFER)==X_FRAMEBUFFER_COMPLETE){
    p->blit(viewport[0],viewport[1],viewport[0]+viewport[2],viewport[1]+viewport[3],0,0,viewport[2],viewport[3],GL_COLOR_BUFFER_BIT,GL_NEAREST);
    snapshot=glGetError()==GL_NO_ERROR;}
   p->bind_framebuffer(X_DRAW_FRAMEBUFFER,(GLuint)draw_fbo);
  }
  if(snapshot){p->width=viewport[2];p->height=viewport[3];p->resolving=resolve;}
 }
 else if(resolve){p->bind_framebuffer(X_DRAW_FRAMEBUFFER,p->resolve_fbo);
  p->blit(viewport[0],viewport[1],viewport[0]+viewport[2],viewport[1]+viewport[3],0,0,viewport[2],viewport[3],GL_COLOR_BUFFER_BIT,GL_NEAREST);
  p->bind_framebuffer(X_DRAW_FRAMEBUFFER,(GLuint)draw_fbo);}
 if(snapshot&&!resolve)glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,viewport[0],viewport[1],viewport[2],viewport[3]);
 glReadBuffer((GLenum)source_read);p->bind_framebuffer(X_READ_FRAMEBUFFER,(GLuint)read_fbo);
 if(resolve&&p->cache.scissor)glEnable(GL_SCISSOR_TEST);
 if(!snapshot){ /* no storage (resource error) or a target that cannot be resolved: leave the ground native */
  if(!resolve)goto restore;
  result=0;p->cache.skip=5;p->cache.hold=MXDIRT_STATE_RECHECK;goto restore;}
 const int sink=soft_on&&soft->sink;
 for(unsigned j=0;j<(sink?2u:1u);j++){const GLint *u=p->uniform[j];p->use_program(p->program[j]);
  p->matrix(u[U_VIEW],1,GL_FALSE,scope->modelview);p->matrix(u[U_PROJECTION],1,GL_FALSE,scope->projection);
  p->ui(u[U_HEIGHTS],0);p->ui(u[U_BACKGROUND],1);p->ui(u[U_CLODS],2);p->u2i(u[U_VIEWPORT],viewport[0],viewport[1]);
  p->u3f(u[U_TEXEL0],w->texel0[0],w->texel0[1],w->texel0[2]);p->u3f(u[U_CAMERA],w->camera[0],w->camera[1],w->camera[2]);
  p->u2f(u[U_SPACING],w->spacing_x,w->spacing_z);p->u1f(u[U_HEIGHT_SCALE],w->height_scale);p->u2f(u[U_FADE],look->fade_start,look->fade_end);
  p->u4f(u[U_BOUNDS],w->bounds[0],w->bounds[1],w->bounds[2],w->bounds[3]);p->u1f(u[U_STRENGTH],look->strength);p->u1f(u[U_LOOSE],look->base_loose);
  p->ui(u[U_STATE],3);p->ui(u[U_STATE_ON],state_on);
  p->ui(u[U_SOFT],4);p->ui(u[U_SOFT_ON],soft_on);
  p->u2f(u[U_SOFT_ORIGIN],soft_on?soft->origin_x:0.f,soft_on?soft->origin_z:0.f);
  p->u4f(u[U_SOFT_WINDOW],soft_window[0],soft_window[1],soft_window[2],soft_window[3]);
  p->ui(u[U_SOFT_MODE],soft_on?(soft->solid?1:0):0);
  p->u1f(u[U_SOFT_CELL],soft_on?soft->cell:.025f);p->u1f(u[U_SOFT_DEPTH],soft_on?soft->thickness:.03f);p->u1f(u[U_SOFT_UNIT],soft_on?soft->unit:.001f);
  p->ui(u[U_SOFT_OCC],5);p->u1f(u[U_SOFT_TILE],soft_on?soft->cell*SOFT_TILE:.8f);
  p->u2f(u[U_SHIFT],(float)w->shift_x,(float)w->shift_z);}
 p->use_program(p->program[0]);
 p->disable_indexed(GL_BLEND,0);p->color(0,1,1,1,0);
 glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);for(unsigned k=1;k<8;k++)glDisable(caps[k]);glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
 glDisable(X_FRAMEBUFFER_SRGB);glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(-1.f,-1.f);
 p->bind_vao(p->vao);glDrawElements(GL_TRIANGLES,GRID_INDICES,GL_UNSIGNED_INT,(void*)0);result=1;
 /* Tyre sink: a second, depth-only draw of the soft layer where it stands above the mesh, at the depth of
    its surface (the shader discards everything else), so the bike drawn after the terrain is hidden where
    it is down in the loose soil. Colour is not touched; native depth elsewhere is not touched. */
 if(sink){p->use_program(p->program[1]);p->color(0,0,0,0,0);glDepthMask(GL_TRUE);
  /* perf: only the mesh rows that cross the soft window (the shader discards everything outside it); the
     index buffer runs row by row, so that is one contiguous range */
  const float z0=soft->origin_z+soft_window[1],z1=soft->origin_z+soft_window[3];
  int j0=(int)floorf((z0-w->texel0[2])/w->spacing_z)-MXDIRT_MARGIN-1,j1=(int)ceilf((z1-w->texel0[2])/w->spacing_z)-MXDIRT_MARGIN+1;
  if(j0<0)j0=0;if(j1>MXDIRT_CELLS)j1=MXDIRT_CELLS;
  if(j1>j0)glDrawElements(GL_TRIANGLES,(GLsizei)((j1-j0)*MXDIRT_CELLS*6),GL_UNSIGNED_INT,(void*)((size_t)j0*MXDIRT_CELLS*6*sizeof(uint32_t)));}
restore:
 if(blend_enabled)p->enable_indexed(GL_BLEND,0);else p->disable_indexed(GL_BLEND,0);p->blend(0,(GLenum)blend[0],(GLenum)blend[1],(GLenum)blend[2],(GLenum)blend[3]);p->equation(0,(GLenum)blend[4],(GLenum)blend[5]);p->color(0,color[0],color[1],color[2],color[3]);
 for(unsigned k=0;k<8;k++)toggle(caps[k],enabled[k]);toggle(X_FRAMEBUFFER_SRGB,srgb);glPolygonOffset(offset_factor,offset_units);glDepthFunc((GLenum)depthfunc);glDepthMask(write);
 glPolygonMode(GL_FRONT,(GLenum)polygon[0]);glPolygonMode(GL_BACK,(GLenum)polygon[1]);p->bind_vao((GLuint)vao);p->bind_buffer(X_ARRAY_BUFFER,(GLuint)buffer);
 restore_bindings(p,saved);if(result!=1)p->cache.valid=0; /* anything odd: full queries next frame */
 if(timed)p->query_counter(p->timer[1],0x8e28);
 return result;
}
int mxdirt_gl_gpu(MxDirtGl *p,double *ms,int *width,int *height){
 if(width)*width=p?(int)p->cache.viewport[2]:0;if(height)*height=p?(int)p->cache.viewport[3]:0;
 if(!p||!ms||!p->gpu_fresh)return 0;p->gpu_fresh=0;*ms=p->gpu_ms;return 1;
}
void mxdirt_gl_target(const MxDirtGl *p,int *draw_fbo,int *samples,int *skip){
 if(draw_fbo)*draw_fbo=p?(int)p->cache.draw_fbo:0;if(samples)*samples=p?(int)p->cache.samples:0;if(skip)*skip=p?p->cache.skip:0;
}
