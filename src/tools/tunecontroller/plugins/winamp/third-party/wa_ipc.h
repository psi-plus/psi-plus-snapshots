/*
** Copyright (C) 1997-2008 Nullsoft, Inc.
**
** This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held 
** liable for any damages arising from the use of this software. 
**
** Permission is granted to anyone to use this software for any purpose, including commercial applications, and to 
** alter it and redistribute it freely, subject to the following restrictions:
**
**   1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. 
**      If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
**
**   2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
**
**   3. This notice may not be removed or altered from any source distribution.
**
*/

/*
** This Winamp SDK file (wa_ipc.h) was changed specially for Psi+ winamptunecontroller
** Unused methods, comments and structures was removed from this file
** Do not use it in another projects
*/

#ifndef _WA_IPC_H_
#define _WA_IPC_H_

#include <windows.h>
#include <stddef.h>
#if (_MSC_VER <= 1200)
typedef int intptr_t;
#endif

#define WM_WA_IPC WM_USER
#define IPC_ISPLAYING 104
/* int res = SendMessage(hwnd_winamp,WM_WA_IPC,0,IPC_ISPLAYING);
** This is sent to retrieve the current playback state of Winamp.
** If it returns 1, Winamp is playing.
** If it returns 3, Winamp is paused.
** If it returns 0, Winamp is not playing.
*/
#define IPC_GETOUTPUTTIME 105
/* int res = SendMessage(hwnd_winamp,WM_WA_IPC,mode,IPC_GETOUTPUTTIME);
** If mode = 0 then it will return the position (in ms) of the currently playing track.
** Will return -1 if Winamp is not playing.
** If mode = 1 then it will return the current track length (in seconds).
** Will return -1 if there are no tracks (or possibly if Winamp cannot get the length).
** If mode = 2 then it will return the current track length (in milliseconds).
** Will return -1 if there are no tracks (or possibly if Winamp cannot get the length).
*/
#define IPC_GETLISTPOS 125
#define IPC_GETPLAYLISTFILE 211
#define IPC_GETPLAYLISTFILEW 214
#define IPC_GET_EXTENDED_FILE_INFO 290

typedef struct {
	const char *filename;
	const char *metadata;
	char *ret;
	size_t retlen;
} extendedFileInfoStruct;

typedef struct { //
	const wchar_t *filename;
	const wchar_t *metadata;
	wchar_t *ret;
	size_t retlen;
} extendedFileInfoStructW;

#define IPC_GET_EXTENDED_FILE_INFOW 3026
#endif//_WA_IPC_H_
