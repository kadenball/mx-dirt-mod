#ifndef MXB_WHEEL_SAMPLE_H
#define MXB_WHEEL_SAMPLE_H
#include <stdint.h>
#include <stddef.h>
#include "mxdirt_contact.h"
#include "mxdirt_wheel_policy.h"
typedef int (*MxbWheelRead)(void *,uint64_t,void *,size_t);
typedef struct {
    uint64_t image_base,bike,generation,sequence;
    int wheel,result,terrain_id;
    float dt;
} MxbWheelSampleRequest;
typedef struct {
    uint64_t body,owner,definition;
    int current,previous,eligible,material;
    MxdirtWheelAvailability availability;
    uint64_t generation,sequence;
} MxbWheelSampleMetadata;
/* Call only AFTER the original wheel-force function and BEFORE integration,
   on the validated physics owner. Read callback provides checked exact copies;
   it does not establish thread/lifecycle exclusion. The wheel's actual body
   pointer and definition radius are captured; tire profile width is unused.
   active means fresh current normal contact, including current1/previous0/
   result1. availability separately reports native tangential-model readiness
   and supported heightfield qualification; no cached tire radius, rolling,
   slip, force or peak value is consumed. Metadata stamps the same physics
   generation/sequence for validation at the outer controller boundary.
   terrain_id1=client,2=host; identity is checked against canonical owner+ce8.
   Outputs remain untouched on invalid fields/read/identity failure. */
int mxb_wheel_sample(MxbWheelRead,void *,const MxbWheelSampleRequest *,
    MxdirtPhysicsSample *,MxbWheelSampleMetadata *);
#endif
