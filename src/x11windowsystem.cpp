#include "x11windowsystem.h"

#include <QX11Info>
#include <X11/Xlib.h>

const long MAX_PROP_SIZE = 100000;
X11WindowSystem* X11WindowSystem::_instance = 0;

X11WindowSystem::X11WindowSystem()
{
	const int atomsCount = 10;
	const char* names[atomsCount] = {
		"_NET_CLIENT_LIST_STACKING",
		"_NET_FRAME_EXTENTS",

		"_NET_WM_WINDOW_TYPE",
		"_NET_WM_WINDOW_TYPE_NORMAL",
		"_NET_WM_WINDOW_TYPE_DIALOG",
		"_NET_WM_WINDOW_TYPE_UTILITY",
		"_NET_WM_WINDOW_TYPE_SPLASH",

		"_NET_WM_STATE",
		"_NET_WM_STATE_ABOVE",
		"_NET_WM_STATE_HIDDEN"
	};
	Atom atoms[atomsCount], *atomsp[atomsCount] = {
		&net_client_list_stacking,
		&net_frame_extents,

		&net_wm_window_type,
		&net_wm_window_type_normal,
		&net_wm_window_type_dialog,
		&net_wm_window_type_utility,
		&net_wm_window_type_splash,

		&net_wm_state,
		&net_wm_state_above,
		&net_wm_state_hidden
	};
	int i = atomsCount;
	while (i--)
		atoms[i] = 0;

	XInternAtoms(QX11Info::display(), (char**)names, atomsCount, true, atoms);

	i = atomsCount;
	while (i--)
		*atomsp[i] = atoms[i];

	if (net_wm_window_type_normal != None)
		normalWindows.insert(net_wm_window_type_normal);
	if (net_wm_window_type_dialog != None)
		normalWindows.insert(net_wm_window_type_dialog);
	if (net_wm_window_type_utility != None)
		normalWindows.insert(net_wm_window_type_utility);
	if (net_wm_window_type_splash != None)
		normalWindows.insert(net_wm_window_type_splash);

	if (net_wm_state_hidden != None)
		ignoredWindowStates.insert(net_wm_state_hidden);
}

X11WindowSystem* X11WindowSystem::instance()
{
	if (!_instance)
		_instance = new X11WindowSystem();
	return X11WindowSystem::_instance;
}

// Get window coords relative to desktop window
QRect X11WindowSystem::windowRect(Window win)
{
	Window w_unused;
	int x, y;
	unsigned int w, h, junk;
	XGetGeometry(QX11Info::display(), win, &w_unused, &x, &y, &w, &h, &junk, &junk);
	XTranslateCoordinates(QX11Info::display(), win, QX11Info::appRootWindow(), 0, 0, &x, &y, &w_unused);

	Atom type_ret;
	int format_ret;
	unsigned char *data_ret;
	unsigned long nitems_ret, unused;
	const Atom XA_CARDINAL = (Atom) 6;
	if (net_frame_extents != None && XGetWindowProperty(QX11Info::display(), win, net_frame_extents,
														0l, 4l, False, XA_CARDINAL, &type_ret, &format_ret,
														&nitems_ret, &unused, &data_ret) == Success)
	{
		if (type_ret == XA_CARDINAL && format_ret == 32 && nitems_ret == 4) {
			//Struts array: 0 - left, 1 - right, 2 - top, 3 - bottom
			long *d = (long *) data_ret;
			x -= d[0];
			y -= d[2];
			w += d[0] + d[1];
			h += d[2] + d[3];
		}
		if ( data_ret )
			XFree(data_ret);
	}

	return QRect(x, y, w, h);
}

// Determine if window is obscured by other windows
bool X11WindowSystem::isWindowObscured(QWidget *widget, bool alwaysOnTop)
{
	if (net_wm_state_above != None)
	{
		if (!alwaysOnTop)
			ignoredWindowStates.insert(net_wm_state_above);
		else
			ignoredWindowStates.remove(net_wm_state_above);
	}

	//TODO Is it correct to use QX11Info::appRootWindow() as root window?
	Q_ASSERT(widget);
	QWidget* w = widget->window();
	Window win = w->winId();

	const Atom XA_WINDOW= (Atom) 33;
	Atom type_ret;
	int format_ret;
	unsigned char *data_ret;
	unsigned long nitems_ret, unused;

	if (net_client_list_stacking != None)
	{
		QRect winRect = windowRect(win);
		if (XGetWindowProperty(QX11Info::display(), QX11Info::appRootWindow(), net_client_list_stacking,
					   0, MAX_PROP_SIZE, False, XA_WINDOW, &type_ret,
					   &format_ret, &nitems_ret, &unused, &data_ret) == Success) {
			if (type_ret == XA_WINDOW && format_ret == 32) {
				Window *wins = (Window *) data_ret;

				//Enumerate windows in reverse order (from most foreground window)
				while (nitems_ret--)
				{
					Window current = wins[nitems_ret];

					//We are not interested in underlying windows
					if (current == win)
						break;

					//If our window in not alwaysOnTop ignore such windows, because we can't raise on top of them
					if (windowHasAnyOfStates(current, ignoredWindowStates))
						continue;

					if (!windowHasOnlyTypes(current, normalWindows))
						continue;

					QRect rect = windowRect(current);
					if (winRect.intersects(rect))
						return true;
				}
			}
			if (data_ret)
				XFree(data_ret);
		}
	}
	return false;
}

//If window has any type other than allowed_types return false, else return true
bool X11WindowSystem::windowHasOnlyTypes(Window win, const QSet<Atom> &allowedTypes)
{
	const Atom XA_ATOM = (Atom) 4;
	Atom type_ret;
	int format_ret;
	unsigned char *data_ret;
	unsigned long nitems_ret, unused;

	if (net_wm_window_type != None && XGetWindowProperty(QX11Info::display(), win, net_wm_window_type,
														 0l, 2048l, False, XA_ATOM, &type_ret,
														 &format_ret, &nitems_ret, &unused, &data_ret) == Success) {
		if (type_ret == XA_ATOM && format_ret == 32 && nitems_ret > 0) {
			Atom *types = (Atom *) data_ret;
			for (unsigned long i = 0; i < nitems_ret; i++)
			{
				if (!allowedTypes.contains(types[i]))
				{
					return false;
				}
			}
		}
		if (data_ret)
			XFree(data_ret);
		return true;
	}
	else
		return false;
}

//If window has any of filteredStates return
bool X11WindowSystem::windowHasAnyOfStates(Window win, const QSet<Atom> &filteredStates)
{
	const Atom XA_ATOM = (Atom) 4;
	Atom type_ret;
	int format_ret;
	unsigned char *data_ret;
	unsigned long nitems_ret, unused;
	if (net_wm_state != None && XGetWindowProperty(QX11Info::display(), win, net_wm_state, 0l, 2048l,
												   False, XA_ATOM, &type_ret, &format_ret,
												   &nitems_ret, &unused, &data_ret) == Success) {
		if (type_ret == XA_ATOM && format_ret == 32 && nitems_ret > 0) {
			Atom *states = (Atom *) data_ret;
			for (unsigned long i = 0; i < nitems_ret; i++) {

				if (filteredStates.contains(states[i]))
					return true;
			}
		}
		if ( data_ret )
			XFree(data_ret);
	}
	return false;
}
