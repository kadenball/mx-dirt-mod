#include "mxdirt_wheel_sample.h"
#include <math.h>
#include <float.h>
static int read_at(MxbWheelRead read,void *user,uint64_t base,uint64_t offset,void *out,size_t size) {
    if(!read||!base||!size||offset>UINT64_MAX-base||size-1>UINT64_MAX-base-offset)return 0;
    return read(user,base+offset,out,size)==1;
}
static int i32(const void *p){int32_t v;memcpy(&v,p,4);return v;}
static int finite_array(const float *p,size_t n){for(size_t j=0;j<n;j++)if(!isfinite(p[j]))return 0;return 1;}
int mxb_wheel_sample(MxbWheelRead read,void *user,const MxbWheelSampleRequest *r,
    MxdirtPhysicsSample *out,MxbWheelSampleMetadata *metadata) {
    if(!r||!out||!metadata||!r->image_base||!r->bike||!r->generation||!r->sequence||
       r->wheel<0||r->wheel>1||r->result<0||r->result>1||r->terrain_id<1||r->terrain_id>2||
       !isfinite(r->dt)||r->dt<=0.f||r->dt>.1f||r->image_base>UINT64_MAX-0x56e5e8)return 0;
    const uint64_t offset=0x60f8+(uint64_t)r->wheel*0xe310;
    if(r->bike>UINT64_MAX-offset)return 0;uint64_t w=r->bike+offset;
    MxbWheelSampleMetadata m;memset(&m,0,sizeof(m));
    uint64_t grid=r->image_base+0x568488+(uint64_t)(r->terrain_id-1)*0x6160;
    if(!read_at(read,user,r->bike,8,&m.owner,8)||m.owner!=grid-0xce8||
       !read_at(read,user,w,MXB_WHEEL_BODY_POINTER,&m.body,8)||
       !read_at(read,user,w,0xbff0,&m.definition,8))return 0;
    unsigned char body[MXB_BODY_CAPTURE_BYTES],contact[0xf0];double radius,load;
    if(!read_at(read,user,m.body,MXB_BODY_CAPTURE_OFFSET,body,sizeof(body))||
       !read_at(read,user,w,0xc6d8,contact,sizeof(contact))||
       !read_at(read,user,m.definition,0xa0,&radius,8))return 0;
    memcpy(&load,contact+0xe8,8);
    if(!isfinite(radius)||radius<=0.||radius>FLT_MAX||!isfinite(load)||fabs(load)>FLT_MAX)return 0;
    m.material=i32(contact+4);m.previous=i32(contact+0x50);m.current=i32(contact+0x54);m.eligible=i32(contact+0x58);
    if(!mxdirt_wheel_availability(&m.availability,m.current,m.previous,m.eligible,r->result))return 0;
    MxdirtPhysicsSample s;memset(&s,0,sizeof(s));
    if(!mxb_wheel_decode_body(&s.body,body,sizeof(body)))return 0;
    memcpy(s.point_metres,contact+0x38,12);memcpy(s.normal,contact+0x44,12);
    if(!finite_array(s.point_metres,3)||!finite_array(s.normal,3)||
       !finite_array(s.body.center,3)||!finite_array(s.body.rotation,12)||
       !finite_array(s.body.linear_world,3)||!finite_array(s.body.angular_world,3))return 0;
    s.radius_metres=(float)radius;s.normal_force_newtons=(float)load;s.physics_dt=r->dt;
    s.active=m.availability.normal_contact;
    if(s.active&&load<0.)return 0;
    s.generation=m.generation=r->generation;s.sequence=m.sequence=r->sequence;*out=s;*metadata=m;return 1;
}
