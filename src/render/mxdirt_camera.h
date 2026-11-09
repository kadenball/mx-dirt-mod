#ifndef MXDIRT_CAMERA_H
#define MXDIRT_CAMERA_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
/* Camera of ONE native MX Bikes terrain draw, captured by the host at the first
   terrain glDrawElements* inside the outer terrain call: the actual native GL
   column-major modelview/projection, the GL context and thread it ran on.
   complete=1 only when exactly one terrain scope ran, its first draw was seen
   and nothing nested or failed. serial increases per captured terrain call. */
typedef struct {
 uint64_t generation,serial;
 HGLRC context;DWORD thread;
 uint32_t complete;
 float modelview[16],projection[16];
} MxDirtCameraScope;
#endif
