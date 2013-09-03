#ifndef SYSTEMWATCH_WIN_H
#define SYSTEMWATCH_WIN_H

#include "systemwatch.h"

#include <qt_windows.h>

class WinSystemWatch : public SystemWatch
{
public:
	WinSystemWatch();
	~WinSystemWatch();

private:
#ifdef HAVE_QT5
    class EventFilter;
    EventFilter *d;
#else
	class MessageWindow;
	MessageWindow *d;
#endif
	bool processWinEvent(MSG *m, long* result);
};

#endif
