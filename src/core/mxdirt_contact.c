#include "mxdirt_contact.h"
#include <math.h>
#include <string.h>

static int finite3(const float v[3]) {
    return isfinite(v[0])&&isfinite(v[1])&&isfinite(v[2]);
}
static float dot3(const float a[3],const float b[3]) {
    return (a[0]*b[0]+a[1]*b[1])+a[2]*b[2];
}
static void cross3(float out[3],const float a[3],const float b[3]) {
    out[0]=a[1]*b[2]-a[2]*b[1];
    out[1]=a[2]*b[0]-a[0]*b[2];
    out[2]=a[0]*b[1]-a[1]*b[0];
}
static int unit3(const float v[3]) {
    return finite3(v)&&fabsf(dot3(v,v)-1.f)<.005f;
}
static int boolean(int v) {return v==0||v==1;}

int mxdirt_contact_init(MxdirtContactBridge *b,const MxdirtContactConfig *c,int wheel) {
    if(!b||!c||wheel<0||wheel>1||!c->generation||!finite3(c->origin_metres)||
       !isfinite(c->native_length_per_metre)||c->native_length_per_metre<=0.f)return 0;
    MxdirtContactBridge next;memset(&next,0,sizeof(next));next.config=*c;
    next.wheel=wheel;next.initialized=1;*b=next;return 1;
}

int mxdirt_contact_physics(MxdirtContactBridge *b,const MxdirtPhysicsSample *s) {
    if(!b||!s||!b->initialized||s->generation!=b->config.generation||
       (b->has_physics&&s->sequence<=b->physics_sequence)||!boolean(s->active)||
       !isfinite(s->physics_dt)||s->physics_dt<=0.f||s->physics_dt>.1f||
       !finite3(s->body.center)||!finite3(s->body.linear_world)||
       !finite3(s->body.angular_world))return 0;
    for(int k=0;k<12;k++)if(!isfinite(s->body.rotation[k]))return 0;
    MxdirtContactBridge next=*b;
    float target=0.f;
    if(s->active) {
        if(!finite3(s->point_metres)||!unit3(s->normal)||
           !isfinite(s->radius_metres)||s->radius_metres<=0.f||
           !isfinite(s->normal_force_newtons)||s->normal_force_newtons<0.f)return 0;
        /* Longitudinal = axle x normal, lateral = normal x longitudinal. */
        float axle[3]={s->body.rotation[0],s->body.rotation[4],s->body.rotation[8]};
        if(!unit3(axle))return 0;
        cross3(next.longitudinal,axle,s->normal);
        float length=sqrtf(dot3(next.longitudinal,next.longitudinal));
        if(!isfinite(length)||length<=1.e-6f)return 0;
        for(int k=0;k<3;k++)next.longitudinal[k]/=length;
        cross3(next.lateral,s->normal,next.longitudinal);
        /* Contact on the tyre circle below the reported point. */
        float lever[3];for(int k=0;k<3;k++)lever[k]=s->point_metres[k]-s->body.center[k];
        length=sqrtf(dot3(lever,lever));if(!isfinite(length)||length<=1.e-6f)return 0;
        float physical_point[3],velocity[3];
        for(int k=0;k<3;k++) {
            next.radius_vector_metres[k]=(lever[k]/length)*s->radius_metres;
            physical_point[k]=s->body.center[k]+next.radius_vector_metres[k];
        }
        mxb_wheel_point_velocity(velocity,&s->body,physical_point);
        for(int k=0;k<3;k++)velocity[k]*=b->config.native_length_per_metre;
        next.contact_velocity_native[0]=dot3(velocity,next.lateral);
        next.contact_velocity_native[1]=dot3(velocity,s->normal);
        next.contact_velocity_native[2]=dot3(velocity,next.longitudinal);
        target=s->normal_force_newtons;next.has_geometry=1;
        if(next.load_count<UINT32_MAX) {
            next.load_sum_n+=s->normal_force_newtons;next.load_count++;
            next.load_slow_n=next.load_slow_n>0.f?next.load_slow_n+(s->normal_force_newtons-next.load_slow_n)*fminf(1.f,s->physics_dt/.6f):s->normal_force_newtons;
        }
    } else memset(next.contact_velocity_native,0,sizeof(next.contact_velocity_native));
    /* First-order load filter: toward the measured force in contact, toward 0 in the air. */
    next.load_n+=(target-next.load_n)*(fminf(s->physics_dt,MXDIRT_LOAD_TAU_S)/MXDIRT_LOAD_TAU_S);
    if(!isfinite(next.load_n)||next.load_n<0.f||!finite3(next.contact_velocity_native)||
       !finite3(next.radius_vector_metres))return 0;
    next.physics_sequence=s->sequence;next.has_physics=1;*b=next;return 1;
}

int mxdirt_contact_frame(MxdirtContactBridge *b,const MxdirtControllerFrame *f,MxdirtContact *out) {
    if(!b||!f||!out||!b->initialized||!b->has_physics||!b->has_geometry||
       f->generation!=b->config.generation||
       f->physics_sequence!=b->physics_sequence||
       (b->has_controller&&f->sequence<=b->controller_sequence)||
       !finite3(f->controller_center_metres)||!finite3(f->center_velocity_metres)||
       !isfinite(f->dt)||f->dt<0.f||f->dt>.1f||
       !isfinite(f->terrain_span_native)||f->terrain_span_native<=0.f||
       !boolean(f->qualified_contact)||!boolean(f->deformation_enabled))return 0;
    MxdirtContactBridge next=*b;MxdirtContact c;memset(&c,0,sizeof(c));
    float center_velocity[3];
    for(int k=0;k<3;k++)center_velocity[k]=f->center_velocity_metres[k]*b->config.native_length_per_metre;
    if(!finite3(center_velocity))return 0;
    const float direction=dot3(b->longitudinal,center_velocity)<0.f?-1.f:1.f;
    for(int k=0;k<3;k++) {
        c.point[k]=((f->controller_center_metres[k]-b->config.origin_metres[k])+
            b->radius_vector_metres[k])*b->config.native_length_per_metre;
        c.forward[k]=b->longitudinal[k]*direction;
        c.lateral[k]=b->lateral[k];
        c.center_velocity[k]=center_velocity[k];
        c.slip[k]=b->contact_velocity_native[k];
    }
    if(!finite3(c.point))return 0;
    c.direction=direction;c.load_n=b->load_n;c.dt=f->dt;
    c.contact=f->qualified_contact;c.deformation=f->deformation_enabled;
    c.frame_mean_n=next.load_count?next.load_sum_n/(float)next.load_count:0.f;c.frame_count=next.load_count;
    c.frame_slow_n=next.load_slow_n;next.load_sum_n=0.f;next.load_count=0;
    next.controller_sequence=f->sequence;next.has_controller=1;*b=next;*out=c;return 1;
}
