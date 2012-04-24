/* ******************************************** */
/*                                              */
/*                AIMP Plugins API              */
/*             v3.00.960 (01.12.2011)           */
/*                 Remote Access                */
/*                                              */
/*              (c) Artem Izmaylov              */
/*                 www.aimp.ru                  */
/*             Mail: artem@aimp.ru              */
/*              ICQ: 345-908-513                */
/*                                              */
/* ******************************************** */

/*
** This AIMP SDK file (AIMPSDKRemote.h) was changed specially for Psi+ aimptunecontroller
** Unused methods, comments and structures was removed from this file
** Do not use it in another projects
*/

#ifndef AIMPSDKRemoteH
#define AIMPSDKRemoteH

#include <windows.h>

const char AIMPRemoteAccessClass[] = "AIMP2_RemoteInfo";
const int  AIMPRemoteAccessMapFileSize = 2048;
const int WM_AIMP_PROPERTY = WM_USER + 0x77;
const int AIMP_RA_PROPVALUE_GET = 0;
const int AIMP_RA_PROPERTY_MASK = 0xFFFFFFF0;
// !! ReadOnly
// Returns current player state
//  0 = Stopped
//  1 = Paused
//  2 = Playing
const int AIMP_RA_PROPERTY_PLAYER_STATE = 0x40;

#endif
