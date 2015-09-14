/*
 * advwidget.h - AdvancedWidget template class
 * Copyright (C) 2005-2007  Michail Pishchagin
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
	QPoint movepath;
	bool isResize;
	bool isDrag;
	bool border;
	Qt::WindowFrameSection region;
	void mouseEnterEvent(const int mouse_x, const int mouse_y, const QRect &geom)
	{
		const int top = geom.top();
		const int bottom = geom.bottom();
		const int left = geom.left();
		const int right = geom.right();
		const int delta = 10;
		const int maxtop = top + delta;
		const int minbottom = bottom -delta;
		if(mouse_y <= bottom
				&& mouse_y >= minbottom
				&& qAbs(mouse_x - left) < delta) {
			region = Qt::BottomLeftSection;
		}
		else if (mouse_x > (left + delta)
			 && mouse_x < (right - delta)
			 && qAbs(mouse_y - bottom) < delta) {
			region = Qt::BottomSection;
		}
		else if ((bottom - mouse_y) < delta
			     && qAbs(mouse_x - right) < delta){
			region = Qt::BottomRightSection;
		}
		else if ((right - mouse_x) < delta
			 &&  mouse_y > maxtop
			 && mouse_y < minbottom) {
			region = Qt::RightSection;
		}
		else if ((mouse_x - left) < delta
			 &&  mouse_y > maxtop
			 && mouse_y < minbottom) {
			region = Qt::LeftSection;
		}
		else if ((mouse_y - top) < delta
			 && mouse_x > (left + delta)
			 && mouse_x < (right -delta)){
			region = Qt::TopSection;
		}
		else if ((top - mouse_y) < delta
			 && qAbs(mouse_x - right) < delta){

			region = Qt::TopRightSection;
		}
		else if ((top - mouse_y) < delta
			 && qAbs(mouse_x - left) < delta){
			region = Qt::TopLeftSection;
		}
		else {
			region = Qt::NoSection;
		}
	}

public:
	AdvancedWidget(QWidget *parent = 0, Qt::WindowFlags f = 0)
		: BaseClass(parent)
		, gAdvWidget(0)
	{
		if (f != 0)
			BaseClass::setWindowFlags(f);
		border = true;
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
		border = isDecorated;
	}
	bool isBorder(){
		return border;
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
	void setWindowFlags(Qt::WindowFlags flags){
		BaseClass::setWindowFlags(flags);
	}
	void mousePressEvent(QMouseEvent *event)
	{
		QWidget *window = BaseClass::window();
		if (!border && (event->button()==Qt::LeftButton)) {
			mouseEnterEvent(event->globalPos().x(), event->globalPos().y(), window->geometry());
			if (region != Qt::NoSection) {
				isResize = true;
			}
			else{
				movepath = event->globalPos() - window->pos();
				isResize = false;
			}
			isDrag = true;
			BaseClass::mousePressEvent(event);
		}
	}
	void mouseMoveEvent(QMouseEvent *event)
	{
		bool isLeftButton = (event->buttons() & Qt::LeftButton);
		const QPoint pg = event->globalPos();
		QWidget *window = BaseClass::window();
		int ypath = 0;
		int xpath = 0;
		const int right = window->geometry().right();
		const int left =  window->geometry().left();
		const int top =  window->geometry().top();
		const int bottom = window->geometry().bottom();
		if (isLeftButton && isResize && !border) {
			switch (region) {
			case Qt::BottomLeftSection:
				window->setCursor(QCursor(Qt::SizeBDiagCursor));
				ypath =  pg.y() - bottom;
				xpath = left - pg.x();
				if ((window->width() + xpath) < window->minimumWidth()) {
					xpath = window->minimumWidth() - window->width();
				}
				window->setGeometry(window->x() - xpath, window->y(),
						    window->width() + xpath, window->height() + ypath);
				break;
			case Qt::BottomRightSection:
				window->setCursor(QCursor(Qt::SizeFDiagCursor));
				ypath = pg.y() - bottom;
				xpath = pg.x() - right;
				window->resize(window->width() + xpath, window->height() + ypath);
				break;
			case Qt::TopLeftSection:
				window->setCursor(QCursor(Qt::SizeFDiagCursor));
				ypath =  top - pg.y();
				xpath = left - pg.x();
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
				window->setCursor(QCursor(Qt::SizeBDiagCursor));
				ypath =  top - pg.y();
				xpath = pg.x() - right;
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
				window->setCursor(QCursor(Qt::SizeVerCursor));
				ypath =  pg.y() - bottom;
				window->resize(window->width(), window->height() + ypath);
				break;
			case Qt::RightSection:
				window->setCursor(QCursor(Qt::SizeHorCursor));
				xpath =  pg.x() - right;
				window->resize(window->width() + xpath, window->height());
				break;
			case Qt::LeftSection:
				window->setCursor(QCursor(Qt::SizeHorCursor));
				xpath =  left - pg.x();
				if ((window->width() + xpath) < window->minimumWidth()) {
					xpath = window->minimumWidth() - window->width();
				}
				window->setGeometry(window->x() - xpath, window->y(),
						    window->width() + xpath, window->height());
				break;
			case Qt::TopSection:
				window->setCursor(QCursor(Qt::SizeVerCursor));
				ypath =  top - pg.y();
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
		else if(isLeftButton && isDrag && !isResize && !border) {
			window->setCursor(QCursor(Qt::ArrowCursor));
			window->move( event->globalPos() - movepath );
		}
		BaseClass::mouseMoveEvent(event);
	}
	void mouseReleaseEvent(QMouseEvent *event)
	{
		QWidget *window = BaseClass::window();
		if (!border && (event->button() == Qt::LeftButton) && isDrag) {
			movepath = QPoint(0,0);
			isDrag = false;
			isResize = false;
			window->setCursor(QCursor(Qt::ArrowCursor));
			BaseClass::mouseReleaseEvent(event);
		}
	}
};

#endif
