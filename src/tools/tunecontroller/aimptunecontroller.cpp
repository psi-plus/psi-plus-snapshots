/*
 * aimptunecontroller.cpp
 * Copyright (C) 2012 Vitaly Tonkacheyev
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "aimptunecontroller.h"
#include "plugins/aimp/third-party/AIMPSDKCommon.h"
#include "plugins/aimp/third-party/AIMPSDKRemote.h"

/**
 * \class AIMPTuneController
 * \brief A controller class for AIMP3 player.
 */

static const int PLAYING = 2;
static const int STOPPED = 0;
static const WCHAR* AIMP_REMOTE_CLASS = (WCHAR *)L"AIMP2_RemoteInfo";

AIMPTuneController::AIMPTuneController()
: PollingTuneController(),
  _tuneSent(false)
{
	startPoll();
}

HWND AIMPTuneController::findAimp() const
{
	return FindWindow(AIMP_REMOTE_CLASS, AIMP_REMOTE_CLASS);
}

int AIMPTuneController::getAimpStatus(const HWND &aimp) const
{
	if (aimp) {
		return (int)SendMessage(aimp, WM_AIMP_PROPERTY, AIMP_RA_PROPERTY_PLAYER_STATE | AIMP_RA_PROPVALUE_GET, 0);
	}
	return STOPPED;
}

void AIMPTuneController::check()
{
	HWND aimp = findAimp();
	if (getAimpStatus(aimp) == PLAYING) {
		sendTune(getTune());
	}
	else {
		clearTune();
	}
	PollingTuneController::check();
}

Tune AIMPTuneController::currentTune() const
{
	return _currentTune;
}

Tune AIMPTuneController::getTune() const
{
	QString title = "";
	QString album = "";
	QString artist = "";
	QString url="";
	HANDLE aFile=OpenFileMapping(FILE_MAP_READ, TRUE, AIMP_REMOTE_CLASS);
	TAIMPFileInfo *aInfo=(TAIMPFileInfo *)MapViewOfFile(aFile, FILE_MAP_READ, 0, 0, AIMPRemoteAccessMapFileSize);
	if (aInfo != NULL) {
		wchar_t *str = (wchar_t *)((DWORD)aInfo+ aInfo->StructSize);
		album = QString::fromWCharArray(str, aInfo->AlbumLength);
		str += aInfo->AlbumLength;
		artist = QString::fromWCharArray(str, aInfo->ArtistLength);
		str += aInfo->ArtistLength + aInfo->DateLength;
		url = QString::fromWCharArray(str, aInfo->FileNameLength);
		str += aInfo->FileNameLength + aInfo->GenreLength;
		title = QString::fromWCharArray(str, aInfo->TitleLength);
	}
	UnmapViewOfFile(aInfo);
	CloseHandle(aFile);
	Tune tune = Tune();
	if (!title.isEmpty()) {
		tune.setName(title);
	}
	if (!artist.isEmpty()) {
		tune.setArtist(artist);
	}
	if (!album.isEmpty()) {
		tune.setAlbum(album);
	}
	if (!url.isEmpty()) {
		tune.setURL(url);
	}
	return tune;
}

void AIMPTuneController::sendTune(const Tune &tune)
{
	if (tune != _currentTune && !tune.isNull()) {
		_currentTune = tune;
		_tuneSent = true;
	}
}

void AIMPTuneController::clearTune()
{
	if (_tuneSent) {
		_currentTune = Tune();
		_tuneSent = false;
	}
}
