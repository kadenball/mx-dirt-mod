#ifndef MXDIRT_PATCHES_H
#define MXDIRT_PATCHES_H
/* Two small MX Bikes engine fixes the dirt publication relies on (mxbikes.exe,
   the one supported build):
   - exe+1f61e6, inside the deformation add (entry exe+1f60c0): a signed,
     saturating merge, so published deposits (negative DFM) are not clipped.
   - exe+1f1f65, material-layer lookup: clamps the layer depth to >= 0 via a
     near trampoline (maxss xmm3,xmm4), so raised dirt keeps its material.
   Install at a stopped lifecycle boundary (EventInit), remove at EventDeinit /
   Shutdown. If another plugin already installed the same patches, they are
   accepted as present but neither re-patched nor removed by us. The trampoline
   is never freed (a call may still be returning through it). */
enum {
    MXDIRT_PATCHES_FAILED=0,
    MXDIRT_PATCHES_INSTALLED=1,      /* at least one site patched by us, both present */
    MXDIRT_PATCHES_FOREIGN=2         /* both already present, installed by another plugin */
};
/* Returns one of the values above; *why names the refusal or "already patched
   by another plugin". Never writes when a signature does not match. */
int mxdirt_patches_install(unsigned char *exe,const char **why);
/* Restores only the sites this module patched and that still hold our bytes. */
void mxdirt_patches_remove(void);
/* Both fixes currently present in the image (ours or another plugin's). */
int mxdirt_patches_present(unsigned char *exe);
/* Sites currently owned by this module (bit0 add, bit1 material). */
unsigned mxdirt_patches_owned(void);
#endif
