/*
 * advwidget.h - AdvancedWidget template class
 * Copyright (C) 2005-2007  Michail Pishchagin, 2017  Evgeny Khryukin
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#ifndef ADVWIDGET_H
#define ADVWIDGET_H

#include <QMouseEvent>
#include <QWidget>

class GAdvancedWidget : public QObject
{
	Q_OBJECT
public:
	GAdvancedWidget(QWidget *parent);

	static bool stickEnabled();
	static void setStickEnabled(bool val);

	static int stickAt();
	static void setStickAt(int val);

	static bool stickToWindows();
	static void setStickToWindows(bool val);

	QString geometryOptionPath() const;
	void setGeometryOptionPath(const QString& optionPath);

	void showWithoutActivation();

	bool flashing() const;
	void doFlash(bool on);

#ifdef Q_OS_WIN
	bool winEvent(MSG* msg, long* result);
#endif

	void moveEvent(QMoveEvent *e);
	void changeEvent(QEvent *event);


public:
	class Private;
private:
	Private *d;
};

template <class BaseClass>
class AdvancedWidget : public BaseClass
{
private:
	GAdvancedWidget *gAdvWidget;
#ifdef Q_OS_WIN
	Qt::WindowFlags deltaflags;
#endif
	QPoint movePath_;
	bool border_;
	Qt::WindowFrameSection region_;
	static const int resizeAccuracy_ = 10;
	enum class WinAction{None, Dragging, Resizing};
	WinAction action_;
	Qt::WindowFrameSection getMouseRegion(const int mouse_x, const int mouse_y, const QRect &geom) const
	{
		const int top = geom.top();
		const int bottom = geom.bottom();
		const int left = geom.left();
		const int right = geom.right();
		const int maxtop = top + resizeAccuracy_;
		const int minbottom = bottom -resizeAccuracy_;
		if(qAbs(bottom - mouse_y) < resizeAccuracy_
		   && qAbs(mouse_x - left) < resizeAccuracy_) {
				return Qt::BottomLeftSection;
		}
		else if (mouse_x > (left + resizeAccuracy_)
			 && mouse_x < (right - resizeAccuracy_)
			 && qAbs(mouse_y - bottom) < resizeAccuracy_)
		{
			return Qt::BottomSection;
		}
		else if (qAbs(bottom - mouse_y) < resizeAccuracy_
			 && qAbs(mouse_x - right) < resizeAccuracy_)
		{
			return Qt::BottomRightSection;
		}
		else if (qAbs(right - mouse_x) < resizeAccuracy_
			 &&  mouse_y > maxtop
			 && mouse_y < minbottom)
		{
			return Qt::RightSection;
		}
		else if (qAbs(mouse_x - left) < resizeAccuracy_
			 &&  mouse_y > maxtop
			 && mouse_y < minbottom)
		{
			return Qt::LeftSection;
		}
		else if (qAbs(mouse_y - top) < resizeAccuracy_
			 && mouse_x > (left + resizeAccuracy_)
			 && mouse_x < (right -resizeAccuracy_))
		{
			return Qt::TopSection;
		}
		else if (qAbs(top - mouse_y) < resizeAccuracy_
			 && qAbs(mouse_x - right) < resizeAccuracy_)
		{
			return Qt::TopRightSection;
		}
		else if (qAbs(top - mouse_y) < resizeAccuracy_
			 && qAbs(mouse_x - left) < resizeAccuracy_)
		{
			return Qt::TopLeftSection;
		}

		return Qt::NoSection;
	}
	void doWindowResize(QWidget *window, const QPoint &eventPos, Qt::WindowFrameSection region)
	{
		int ypath = 0;
		int xpath = 0;
		const QRect winGeom = window->geometry();
		const int right = winGeom.right();
		const int left =  winGeom.left();
		const int top =  winGeom.top();
		const int bottom = winGeom.bottom();

		switch (region) {
		case Qt::BottomLeftSection:
			ypath =  eventPos.y() - bottom;
			xpath = left - eventPos.x();
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = window->minimumWidth() - window->width();
			}
			window->setGeometry(window->x() - xpath, window->y(),
					    window->width() + xpath, window->height() + ypath);
			break;
		case Qt::BottomRightSection:
			ypath = eventPos.y() - bottom;
			xpath = eventPos.x() - right;
			window->resize(window->width() + xpath, window->height() + ypath);
			break;
		case Qt::TopLeftSection:
			ypath =  top - eventPos.y();
			xpath = left - eventPos.x();
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = window->minimumWidth() - window->width();
			}
			if ((window->height() + ypath) < window->minimumHeight()) {
				ypath = window->minimumHeight() - window->height();
			}
			window->setGeometry(window->x() - xpath, window->y() - ypath,
					    window->width() + xpath, window->height() + ypath);
			break;
		case Qt::TopRightSection:
			ypath =  top - eventPos.y();
			xpath = eventPos.x() - right;
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = window->minimumWidth() - window->width();
			}
			if ((window->height() + ypath) < window->minimumHeight()) {
				ypath = window->minimumHeight() - window->height();
			}
			window->setGeometry(window->x(), window->y() - ypath,
					    window->width() + xpath, window->height() + ypath);
			break;
		case Qt::BottomSection:
			ypath =  eventPos.y() - bottom;
			window->resize(window->width(), window->height() + ypath);
			break;
		case Qt::RightSection:
			xpath =  eventPos.x() - right;
			window->resize(window->width() + xpath, window->height());
			break;
		case Qt::LeftSection:
			xpath =  left - eventPos.x();
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = window->minimumWidth() - window->width();
			}
			window->setGeometry(window->x() - xpath, window->y(),
					    window->width() + xpath, window->height());
			break;
		case Qt::TopSection:
			ypath =  top - eventPos.y();
			if ((window->height() + ypath) < window->minimumHeight()) {
				ypath = window->minimumHeight() - window->height();
			}
			window->setGeometry(window->x(), window->y() - ypath,
					    window->width(), window->height() + ypath);
			break;
		case Qt::NoSection:
		default:
			break;
		}
	}
	void updateCursor(Qt::WindowFrameSection region, QWidget *window)
	{
		switch (region) {
		case Qt::BottomLeftSection:
			window->setCursor(QCursor(Qt::SizeBDiagCursor));
			break;
		case Qt::BottomRightSection:
			window->setCursor(QCursor(Qt::SizeFDiagCursor));
			break;
		case Qt::TopLeftSection:
			window->setCursor(QCursor(Qt::SizeFDiagCursor));
			break;
		case Qt::TopRightSection:
			window->setCursor(QCursor(Qt::SizeBDiagCursor));
			break;
		case Qt::BottomSection:
			window->setCursor(QCursor(Qt::SizeVerCursor));
			break;
		case Qt::RightSection:
			window->setCursor(QCursor(Qt::SizeHorCursor));
			break;
		case Qt::LeftSection:
			window->setCursor(QCursor(Qt::SizeHorCursor));
			break;
		case Qt::TopSection:
			window->setCursor(QCursor(Qt::SizeVerCursor));
			break;
		case Qt::NoSection:
		default:
			window->setCursor(QCursor(Qt::ArrowCursor));
			break;
		}
	}
	void enableMouseTracking(bool enabled)
	{
		BaseClass::setMouseTracking(enabled);
		QWidget *bw = BaseClass::window();
		QList<QWidget *> children = bw->findChildren<QWidget*>();
		foreach (QWidget *w, children) {
			w->setMouseTracking(enabled);
		}
		if (!enabled) {
			BaseClass::window()->setCursor(QCursor(Qt::ArrowCursor));
		}
	}
	bool isMaximized() const
	{
		return (BaseClass::window()->windowState() == Qt::WindowMaximized)||(BaseClass::window()->windowState() == Qt::WindowFullScreen);
	}

public:
	AdvancedWidget(QWidget *parent = 0, Qt::WindowFlags f = 0)
		: BaseClass(parent)
		, gAdvWidget(0)
	{
		if (f != 0)
			BaseClass::setWindowFlags(f);
		border_ = true;
		gAdvWidget = new GAdvancedWidget( this );
	}

	virtual ~AdvancedWidget()
	{
	}

	void setWindowIcon(const QIcon& icon)
	{
#ifdef Q_OS_MAC
		Q_UNUSED(icon);
#else
		BaseClass::setWindowIcon(icon);
#endif
	}

	static bool stickEnabled() { return GAdvancedWidget::stickEnabled(); }
	static void setStickEnabled( bool val ) { GAdvancedWidget::setStickEnabled( val ); }

	static int stickAt() { return GAdvancedWidget::stickAt(); }
	static void setStickAt( int val ) { GAdvancedWidget::setStickAt( val ); }

	static bool stickToWindows() { return GAdvancedWidget::stickToWindows(); }
	static void setStickToWindows( bool val ) { GAdvancedWidget::setStickToWindows( val ); }

	QString geometryOptionPath() const
	{
		if (gAdvWidget)
			return gAdvWidget->geometryOptionPath();
		return QString();
	}

	void setGeometryOptionPath(const QString& optionPath)
	{
		if (gAdvWidget)
			gAdvWidget->setGeometryOptionPath(optionPath);
	}

	bool flashing() const
	{
		if (gAdvWidget)
			return gAdvWidget->flashing();
		return false;
	}

	void showWithoutActivation()
	{
		if (gAdvWidget)
			gAdvWidget->showWithoutActivation();
	}

	virtual void doFlash( bool on )
	{
		if (gAdvWidget)
			gAdvWidget->doFlash( on );
	}

#ifdef Q_OS_WIN
	bool winEvent(MSG* msg, long* result)
	{
		if (gAdvWidget)
			return gAdvWidget->winEvent(msg, result);
		return BaseClass::winEvent(msg, result);
	}
#endif

	void moveEvent( QMoveEvent *e )
	{
		if (gAdvWidget)
			gAdvWidget->moveEvent(e);
	}

	void setWindowTitle( const QString &c )
	{
		BaseClass::setWindowTitle( c );
		windowTitleChanged();
	}

	void setWindowBorder(bool isDecorated)
	{
		Qt::WindowFlags flags = BaseClass::windowFlags();
#ifdef Q_OS_WIN
		if (deltaflags == 0) {
			deltaflags = flags;
		}
		if (isDecorated) {
			if (flags != deltaflags) {
				flags |= Qt::WindowTitleHint;
				flags &= ~Qt::FramelessWindowHint;
				deltaflags = 0;
				if (flags != BaseClass::windowFlags()) {
					setWindowFlags(flags);
				}
			}
		} else {
			flags &= ~Qt::WindowTitleHint;
			flags |= Qt::FramelessWindowHint;
			if (flags != BaseClass::windowFlags()) {
				setWindowFlags(flags);
			}

		}
#else
		if (isDecorated) {
			flags &= ~Qt::FramelessWindowHint;
		} else {
			flags |= Qt::FramelessWindowHint;
		}
		if (flags != BaseClass::windowFlags()) {
			setWindowFlags(flags);
		}
#endif
		border_ = isDecorated;
		enableMouseTracking(true);
	}
	bool isBorder() const
	{
		return border_;
	}

protected:
	virtual void windowTitleChanged()
	{
		doFlash(flashing());
	}

protected:
	void changeEvent(QEvent *event)
	{
		if (gAdvWidget) {
			gAdvWidget->changeEvent(event);
		}
		BaseClass::changeEvent(event);
	}

protected:
	void setWindowFlags(Qt::WindowFlags flags)
	{
		BaseClass::setWindowFlags(flags);
	}
	void mousePressEvent(QMouseEvent *event)
	{
		if (!border_ && (event->button()==Qt::LeftButton) && !isMaximized()) {
			QWidget *window = BaseClass::window();
			region_ = getMouseRegion(event->globalPos().x(), event->globalPos().y(), window->geometry());
			if (region_ != Qt::NoSection) {
				action_ = WinAction::Resizing;
			}
			else{
				movePath_ = event->globalPos() - window->pos();
				action_ = WinAction::Dragging;
			}
		}
		BaseClass::mousePressEvent(event);
	}
	void mouseMoveEvent(QMouseEvent *event)
	{
		if(!border_ && !isMaximized()) {
			bool isLeftButton = (event->buttons() & Qt::LeftButton);
			const QPoint pg = event->globalPos();
			QWidget *window = BaseClass::window();

			if(!isLeftButton) {
				Qt::WindowFrameSection region = getMouseRegion(pg.x(), pg.y(), window->geometry());

				updateCursor(region, window);
			}
			else if (isLeftButton && action_ == WinAction::Resizing) {
				doWindowResize(window, pg, region_);
			}
			else if(isLeftButton && action_ == WinAction::Dragging) {
				window->setCursor(QCursor(Qt::SizeAllCursor));
				window->move(pg - movePath_);
			}
		}

		BaseClass::mouseMoveEvent(event);
	}
	void mouseReleaseEvent(QMouseEvent *event)
	{
		if (!border_ && (event->button() == Qt::LeftButton)
		    && action_ == WinAction::Dragging && !isMaximized()) {
			QWidget *window = BaseClass::window();

			movePath_ = QPoint(0,0);
			action_ = WinAction::None;
			window->setCursor(QCursor(Qt::ArrowCursor));
		}

		BaseClass::mouseReleaseEvent(event);
	}
};

#endif
