#ifndef X11INFO_H
#define X11INFO_H

typedef struct _XDisplay Display;
#ifdef HAVE_QT5
typedef struct xcb_connection_t xcb_connection_t;
#endif

class X11Info
{
	static Display *_display;
#ifdef HAVE_QT5
	static xcb_connection_t *_xcb;
#endif
	static int _xcbPreferredScreen;

public:
	static Display* display();
	static unsigned long appRootWindow(int screen = -1);
#ifdef HAVE_QT5
	static xcb_connection_t* xcbConnection();
	static inline int xcbPreferredScreen() { return _xcbPreferredScreen; }
#endif
};

#endif // X11INFO_H
