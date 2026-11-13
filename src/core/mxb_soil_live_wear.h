#ifndef MXB_SOIL_LIVE_WEAR_H
#define MXB_SOIL_LIVE_WEAR_H
/* Live wear level: how much each of your own laps wears the track ([soil] live_wear_percent).
   k = 0 ("Realistic", the default) is the 1.1.0 live tuning: realistic one-rider growth, the line packs in and
   bumps stay small. k = 1 ("Medium") is tuned on Maryland so ~15 of your laps wear
   it about like the "Practice" pre-wear (25 virtual riders), measured on what the game
   actually rides after publication: bump count/heights, deep cells, rut depth.
   Bumps grow faster per pass (deeper rows, deeper braking troughs, stronger swing), the
   groove itself cuts a little less. The per-pass dig limit stays at Low's 1.2 cm x erode
   x (1+k): a higher limit dug 22 cm holes on the first fresh lap.
   Offline harness: scratchpad lt.c + pub.py (publication smoothing) vs pw_dump.c. */
#include <math.h>
#ifndef MXB_LW_DEPTH
#define MXB_LW_DEPTH -.2f  /* groove cut depth per k */
#endif
#ifndef MXB_LW_BIAS
#define MXB_LW_BIAS 2.f    /* extra bump-row depth per pass per k */
#endif
#ifndef MXB_LW_CAP
#define MXB_LW_CAP 1.f     /* raise of the per-pass bump dig limit per k */
#endif
#ifndef MXB_LW_GAIN
#define MXB_LW_GAIN 1.f    /* stronger cut modulation by the suspension swing per k */
#endif
#ifndef MXB_LW_FLOOR
#define MXB_LW_FLOOR 2.f   /* deeper washboard trough bound per k */
#endif
typedef struct{float k;}MxbLiveWear;
/* percent 0..200: 0 = Low (k 0), 100 = Medium (k 1) */
static inline void mxb_live_wear_init(MxbLiveWear *l,float k){l->k=isfinite(k)?fmaxf(0.f,fminf(2.f,k)):0.f;}
static inline float mxb_live_wear_depth(const MxbLiveWear *l){return fmaxf(.5f,1.f+MXB_LW_DEPTH*l->k);}
/* bump-row bias before the per-pass cap (cap = 1.2 cm x erode at k 0) */
static inline float mxb_live_wear_bias(const MxbLiveWear *l,float bias,float dn){
    bias*=1.f+MXB_LW_BIAS*l->k;const float cap=.012f*dn*(1.f+MXB_LW_CAP*l->k);
    return fmaxf(-cap,fminf(cap,bias));
}
static inline float mxb_live_wear_floor(const MxbLiveWear *l,float floor){return floor*(1.f+MXB_LW_FLOOR*l->k);}
/* g = 1 + modulation from the bump model */
static inline float mxb_live_wear_gain(const MxbLiveWear *l,float g){return 1.f+(g-1.f)*(1.f+MXB_LW_GAIN*l->k);}
/* the cut gain's range (.75..1.3 at k 0) */
static inline float mxb_live_wear_clamp(const MxbLiveWear *l,float g){
    const float lo=.75f-.15f*l->k,hi=1.3f+.2f*l->k;return fmaxf(lo,fminf(hi,g));
}
#endif
