/* ******************************************** */
/*                                              */
/*                AIMP Plugins API              */
/*             v3.00.960 (01.12.2011)           */
/*                Common Objects                */
/*                                              */
/*              (c) Artem Izmaylov              */
/*                 www.aimp.ru                  */
/*             Mail: artem@aimp.ru              */
/*              ICQ: 345-908-513                */
/*                                              */
/* ******************************************** */

#ifndef AIMPSDKCommonH
#define AIMPSDKCommonH

#include <windows.h>
#include <unknwn.h>

#pragma pack(push, 1)
struct TAIMPFileInfo
{
	DWORD StructSize;
	//
	BOOL  Active;
	DWORD BitRate;
	DWORD Channels;
	DWORD Duration;
	INT64 FileSize;
	DWORD Rating;
	DWORD SampleRate;
	DWORD TrackNumber;
	//
	DWORD AlbumLength;
	DWORD ArtistLength;
	DWORD DateLength;
	DWORD FileNameLength;
	DWORD GenreLength;
	DWORD TitleLength;
	//
	PWCHAR Album;
	PWCHAR Artist;
	PWCHAR Date;
	PWCHAR FileName;
	PWCHAR Genre;
	PWCHAR Title;
};
#pragma pack(pop)

#endif
