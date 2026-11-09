#ifndef MXDIRT_WHEEL_POLICY_H
#define MXDIRT_WHEEL_POLICY_H

/* Normal-contact availability, used by mxdirt_wheel_sample. Call after the
   MX Bikes wheel function (exe+1a2fb0) on the verified owner, with coherent,
   separately validated point/normal/definition-radius/body/load. MX Bikes'
   caller (exe+1a5a10) computes/applies the normal force BEFORE that function,
   so first contact (current1,previous0,result1) already has fresh normal data
   although the tangential tyre model was skipped. A query miss may retain a
   stale point/normal.

   heightfield_qualified: current contact on the terrain heightfield (the
   wheel's selected-heightfield flag); meshes and other objects are excluded. */
typedef struct MxdirtWheelAvailability {
    int normal_contact;
    int tire_response_ready;
    int heightfield_qualified;
} MxdirtWheelAvailability;

/* MX Bikes' own current/previous/selected flags and the wheel function result.
   Rejects contradictory result/history rather than hiding a changed function
   or an incoherent capture. No output changes on rejection. */
static inline int mxdirt_wheel_availability(
    MxdirtWheelAvailability *out, int current, int previous,
    int selected_heightfield, int tire_result)
{
    MxdirtWheelAvailability r;
    if (!out || (current != 0 && current != 1) ||
        (previous != 0 && previous != 1) ||
        (selected_heightfield != 0 && selected_heightfield != 1) ||
        (tire_result != 0 && tire_result != 1) ||
        tire_result != !(current && previous)) return 0;
    r.normal_contact = current;
    r.tire_response_ready = tire_result == 0;
    r.heightfield_qualified = current && selected_heightfield;
    *out = r;
    return 1;
}
#endif
