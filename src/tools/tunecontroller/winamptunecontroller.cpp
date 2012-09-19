/*
 * winamptunecontroller.cpp
 * Copyright (C) 2006  Remko Troncon,
 * 2012  Vitaly Tonkacheyev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <windows.h>

#ifdef Q_CC_MSVC
#pragma warning(push)
#pragma warning(disable: 4100)
#endif

// this file generates eight C4100 warnings, when compiled with MSVC2003
#include "plugins/winamp/third-party/wa_ipc.h"

#ifdef Q_CC_MSVC
#pragma warning(pop)
#endif

#include "winamptunecontroller.h"

#define TAGSIZE 100
#define FILENAMESIZE 512

static const int NormInterval = 3000;
static const int AntiscrollInterval = 100;
const wchar_t *ALBUM_KEY = L"album";
const wchar_t *ARTIST_KEY = L"artist";
const wchar_t *TITLE_KEY = L"title";

/**
 * \class WinAmpController
 * \brief A controller for WinAmp.
 */


/**
 * \brief Constructs the controller.
 */
WinAmpController::WinAmpController()
: PollingTuneController(),
  antiscrollCounter_(0)
{
	startPoll();
	setInterval(NormInterval);
}

template <typename char_type> const size_t length (const char_type * begin)
{
	const char_type * end = begin;
	for (; *end; ++end);
	return end - begin;
}

/**
 * Polls for new song info.
 */
void WinAmpController::check()
{
	Tune tune;
	HWND winamp = FindWindow(L"Winamp v1.x", NULL);
	if (winamp && SendMessage(winamp, WM_WA_IPC, 0, IPC_ISPLAYING) == 1) {
		tune = getTune(winamp);
	}
	prevTune_ = tune;
	setInterval(NormInterval);
	PollingTuneController::check();
}

Tune WinAmpController::getTune(const HWND &hWnd)
{
	Tune tune = Tune();
	QString album, artist, title, url;
//this part of code taken from pidgin-musictracker from http://code.google.com/p/pidgin-musictracker/
	int position = (int)SendMessage(hWnd, WM_WA_IPC, 0, IPC_GETLISTPOS);
	if (position != -1) {
		DWORD processId;
		GetWindowThreadProcessId(hWnd, &processId);
		LPCVOID address = (LPCVOID) SendMessage(hWnd, WM_WA_IPC, position, IPC_GETPLAYLISTFILEW);
		if ((uint)address > 1) {
			wchar_t fileNameW[FILENAMESIZE];
			HANDLE hP = OpenProcess(PROCESS_ALL_ACCESS, 0, processId);
			ReadProcessMemory(hP, address, fileNameW, FILENAMESIZE, 0);
//
			url = QString::fromWCharArray(fileNameW);
			if (!url.isEmpty()){
				wchar_t albumW[TAGSIZE], artistW[TAGSIZE], titleW[TAGSIZE];
				if (getData(hP, hWnd, fileNameW, ALBUM_KEY, albumW)) {
					album = QString::fromWCharArray(albumW);
					if (!album.isEmpty()) {
						tune.setAlbum(album);
					}
				}
				if (getData(hP, hWnd, fileNameW, ARTIST_KEY, artistW)) {
					artist = QString::fromWCharArray(artistW);
					if (!artist.isEmpty()) {
						tune.setArtist(artist);
					}
				}
				if (getData(hP, hWnd, fileNameW, TITLE_KEY, titleW)) {
					title = QString::fromWCharArray(titleW);
					if (title.isEmpty()) {
						int index = url.replace("/", "\\").lastIndexOf("\\");
						if (index > 0) {
							QString filename = url.right(url.length()-index-1);
							index = filename.lastIndexOf(".");
							title = (index > 0) ? filename.left(index) : filename;
						}
						else {
							title = url;
						}
					}
					tune.setName(title);
				}
				tune.setURL(url);
				tune.setTrack(QString::number(position + 1));
				tune.setTime(SendMessage(hWnd, WM_WA_IPC, 1, IPC_GETOUTPUTTIME));
			}
			CloseHandle(hP);
		}
		else {
			QPair<bool, QString> trackpair(getTrackTitle(hWnd));
			if (!trackpair.first) {
				// getTrackTitle wants us to retry in a few ms...
				int interval = AntiscrollInterval;
				if (++antiscrollCounter_ > 10) {
					antiscrollCounter_ = 0;
					interval = NormInterval;
				}
				setInterval(interval);
				return Tune();
			}
			antiscrollCounter_ = 0;
			tune.setName(trackpair.second);
			tune.setURL(trackpair.second);
			tune.setTrack(QString::number(position + 1));
			tune.setTime(SendMessage(hWnd, WM_WA_IPC, 1, IPC_GETOUTPUTTIME));
		}
	}
	return tune;
}

QPair<bool, QString> WinAmpController::getTrackTitle(const HWND &waWnd) const
{
	TCHAR waTitle[2048];
	QString title;

	// Get WinAmp window title. It always contains name of the track
	SendMessage (waWnd, WM_GETTEXT, static_cast<WPARAM> (sizeof (waTitle) / sizeof (waTitle[0])), reinterpret_cast<LPARAM> (waTitle));
	// Now, waTitle contains WinAmp window title
	title = QString ((const QChar *) waTitle, length<TCHAR> ((const TCHAR *) waTitle));
	if (title[0] == '*' || (title.length () && title[title.length() - 1] == '*')) {
		// request to be called again soon.
		return QPair<bool, QString>(false, QString());
	}

	// Check whether there is a need to do the all stuff
	if (!title.length()) {
		return QPair<bool, QString>(true,title);
	}

	QString winamp (" - Winamp ***");
	int winampLength = winamp.length();

	// Is title scrolling on the taskbar enabled?
	title += title + title;
	int waLast = title.indexOf (winamp, -1);
	if (waLast != -1) {
		if (title.length()) {
			title.remove (waLast, title.length () - waLast);
		}
		int waFirst;
		while ((waFirst = title.indexOf (winamp)) != -1) {
			title.remove (0, waFirst + winampLength);
		}
	}
	else {
		title = QString ((const QChar *) waTitle, length<TCHAR> ((const TCHAR *) waTitle)); // Title is not scrolling
	}

	// Remove leading and trailing spaces
	title  = title.trimmed();

	// Remove trailing " - Winamp" from title
	if (title.length ()) {
		winamp = " - Winamp";
		winampLength = winamp.length ();
		int waFirst = title.indexOf (winamp);
		if (waFirst != -1)
		{
			title.remove (waFirst, waFirst + winampLength);
		}
	}

	// Remove track number from title
	if (title.length ()) {
		QString dot(". ");
		int dotFirst = title.indexOf (dot);
		if (dotFirst != -1) {
			// All symbols before the dot are digits?
			bool allDigits = true;
			for (int pos = dotFirst; pos > 0; --pos) {
				allDigits = allDigits && title[pos].isNumber();
			}
			if (allDigits) {
				title.remove(0, dotFirst + dot.length ());
			}
		}
	}

	// Remove leading and trailing spaces
	if (title.length ()) {
		while (title.length () && title[0] == ' ') {
			title.remove (0, 1);
		}
		while (title.length () && title[title.length () - 1] == ' ') {
			title.remove (title.length () - 1, 1);
		}
	}

	return QPair<bool, QString>(true,title);
}

bool WinAmpController::getData(const HANDLE& hProcess, const HWND& hWnd, const wchar_t *filename, const wchar_t *metadata, wchar_t *wresult)
{
//this part of code taken from pidgin-musictracker plugin from http://code.google.com/p/pidgin-musictracker/
	char *winamp_info = (char *)VirtualAllocEx(hProcess, NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
	if (!winamp_info)
	{
		return false;
	}
	wchar_t *winamp_filename = (wchar_t*)(winamp_info+1024);
	wchar_t *winamp_metadata = (wchar_t*)(winamp_info+2048);
	wchar_t *winamp_value = (wchar_t*)(winamp_info+3072);
	extendedFileInfoStructW info;
	info.filename = winamp_filename;
	info.metadata = winamp_metadata;
	info.ret = winamp_value;
	info.retlen = 1024/sizeof(wchar_t);
	WriteProcessMemory(hProcess, winamp_info, &info, sizeof(info), NULL);
	WriteProcessMemory(hProcess, winamp_filename, filename, sizeof(wchar_t)*(wcslen(filename)+1), NULL);
	WriteProcessMemory(hProcess, winamp_metadata, metadata, sizeof(wchar_t)*(wcslen(metadata)+1), NULL);
	int rc = (int)SendMessage(hWnd, WM_WA_IPC, (WPARAM)winamp_info, IPC_GET_EXTENDED_FILE_INFOW);
	SIZE_T bytesRead;
	ReadProcessMemory(hProcess, winamp_value, wresult, (TAGSIZE-1)*sizeof(wchar_t), &bytesRead);
	wresult[bytesRead/sizeof(wchar_t)] = 0;
	VirtualFreeEx(hProcess, winamp_info, 0, MEM_RELEASE);
//
	return (bool(rc));
}

Tune WinAmpController::currentTune() const
{
	return prevTune_;
}
