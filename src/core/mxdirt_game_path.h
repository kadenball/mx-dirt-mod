#ifndef MXDIRT_GAME_PATH_H
#define MXDIRT_GAME_PATH_H
#include <windows.h>
#include <string.h>
#include <wchar.h>
/* INI file keys may name an absolute path or a path relative to the game
   folder (the folder holding the MX Bikes executable), e.g.
   plugins\mx_dirt\x.bin, so an install does not depend on where
   Steam or the user's profile lives. Resolves in place and returns the new
   length, or 0 when the key is empty or the result would not fit below cap-1
   (the callers' existing too-long rejection). Absolute paths pass unchanged. */
static inline DWORD mxb_game_path_a(char *path,DWORD n,size_t cap){
 if(!n||n>=cap-1)return 0;
 if((n>=3&&path[1]==':'&&(path[2]=='\\'||path[2]=='/'))||(n>=3&&path[0]=='\\'&&path[1]=='\\'))return n;
 char dir[MAX_PATH];DWORD d=GetModuleFileNameA(NULL,dir,MAX_PATH);if(!d||d>=MAX_PATH)return 0;
 char *slash=strrchr(dir,'\\');if(!slash)return 0;d=(DWORD)(slash+1-dir);
 if((size_t)d+n>=cap-1)return 0;
 memmove(path+d,path,(size_t)n+1);memcpy(path,dir,d);return d+n;
}
static inline DWORD mxb_game_path_w(wchar_t *path,DWORD n,size_t cap){
 if(!n||n>=cap-1)return 0;
 if((n>=3&&path[1]==L':'&&(path[2]==L'\\'||path[2]==L'/'))||(n>=3&&path[0]==L'\\'&&path[1]==L'\\'))return n;
 static wchar_t dir[32768];DWORD d=GetModuleFileNameW(NULL,dir,32768);if(!d||d>=32768)return 0;
 wchar_t *slash=wcsrchr(dir,L'\\');if(!slash)return 0;d=(DWORD)(slash+1-dir);
 if((size_t)d+n>=cap-1)return 0;
 wmemmove(path+d,path,(size_t)n+1);wmemcpy(path,dir,d);return d+n;
}
#endif
