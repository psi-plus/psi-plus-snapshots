#include "x11info.h"

#ifdef HAVE_QT5
# include <X11/Xlib.h>
# include <xcb/xcb.h>
# include <QtGlobal>
#else
# include <QX11Info>
#endif


Display* X11Info::display()
{
#ifdef HAVE_QT5
	if (!_display) {
		_display = XOpenDisplay(NULL);
	}
	return _display;
#else
	return QX11Info::display();
#endif
}

unsigned long X11Info::appRootWindow(int screen)
{
#ifdef HAVE_QT5
	return screen == -1?
				XDefaultRootWindow(display()) :
				XRootWindowOfScreen(XScreenOfDisplay(display(), screen));
#else
	return QX11Info::appRootWindow(screen);
#endif
}

#ifdef HAVE_QT5
xcb_connection_t *X11Info::xcbConnection()
{
	if (!_xcb) {
		_xcb = xcb_connect(NULL, &_xcbPreferredScreen);
		Q_ASSERT(_xcb);
	}
	return _xcb;
}

xcb_connection_t* X11Info::_xcb = 0;
#endif

Display* X11Info::_display = 0;
int X11Info::_xcbPreferredScreen = 0;
