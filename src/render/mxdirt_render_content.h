#ifndef MXDIRT_RENDER_CONTENT_H
#define MXDIRT_RENDER_CONTENT_H
#include <stdint.h>
#include <wchar.h>
#include "mxdirt_camera.h"
#include "mxdirt_terrain_scope.h"
/* Host/content boundary. The host (mxdirt_render_host.c) owns the native
   hooks; this module owns the configuration, the link to the core plugin, the
   terrain identity check and the Dirt Material pass. All calls arrive on the
   SDK Startup thread, which is also the game's render thread. */
enum { MXDIRT_EVENT_BEGIN=1,MXDIRT_RUN_INIT,MXDIRT_RUN_START,MXDIRT_RUN_STOP,MXDIRT_RUN_DEINIT,MXDIRT_EVENT_RETIRE,MXDIRT_SHUTDOWN };
/* Once at Startup. ini_path is the absolute plugins\mx_dirt_render.ini path.
   Returns 1 when [renderer] enabled=1 and the configuration is accepted. */
int mxdirt_content_startup(const wchar_t *ini_path);
/* 1 when the Dirt Material pass is configured on (dirt_material=1 and valid
   strength/loose/fade keys). The host installs its terrain hooks only then. */
int mxdirt_content_dirt_enabled(void);
/* Current core generation (MxDirtFrame.generation) or 0 when the core is
   absent, not live, on the wrong thread or ABI-incompatible. No GL calls. */
uint64_t mxdirt_content_generation(void);
/* After the complete native terrain call returned. scope carries the native
   camera of that draw; terrain the native descriptor read at the call. Returns
   1 drawn, 0 skipped, negative on failure (the pass then stays off for this
   event). Never changes native forwarding. */
int mxdirt_content_draw(const MxDirtCameraScope *scope,const MxDirtTsTerrain *terrain);
/* SDK lifecycle in original order on the Startup owner, deferred by the host
   until native wrappers unwind. GL cleanup is retried when the owner context
   is not current; nothing is force-deleted. */
void mxdirt_content_lifecycle(uint32_t event,uint64_t generation);
/* One timestamped line in the content log (no-op when content_log is empty).
   Bounded to a few thousand lines per session. */
void mxdirt_content_note(const char *fmt,...);
#endif
