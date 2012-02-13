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
	QMap <QString, bool> regions;
	void mouseEnterEvent(int mouse_x, int mouse_y, QRect geom){
		int top = geom.top();
		int bottom = geom.bottom();
		int left = geom.left();
		int right = geom.right();
		if(mouse_y <= bottom
			&& mouse_y >= (bottom - 10)
			&& qAbs(mouse_x - left) < 10) {
			setMouseRegion("leftbottom");
		}
		else if (mouse_x > left
			&& (mouse_x - left) > 10
			&& mouse_x < right
			&& (right - mouse_x) > 10
			&& qAbs(mouse_y - bottom) < 10) {
			setMouseRegion("bottom");
		}
		else if (mouse_y <= bottom
			&& mouse_y >= (bottom - 10)
			&& qAbs(mouse_x - right) < 10){
			setMouseRegion("rightbottom");
		}
		else if (mouse_x <= right
			&& qAbs(mouse_x - right) < 7
			&&  mouse_y > (top + 10)
			&& mouse_y < (bottom - 10)) {
			setMouseRegion("right");
		}
		else if (mouse_x >= left
			&& qAbs(mouse_x - left) < 7
			&&  mouse_y > (top + 10)
			&& mouse_y < (bottom - 10)) {
			setMouseRegion("left");
		}
		else if (mouse_y >= top
			&& qAbs(mouse_y - top)<10
			&& mouse_x > (left + 10)
			&& mouse_x < (right -10)){
			setMouseRegion("top");
		}
		else if (mouse_y >= top
			&& mouse_y < (top + 10)
			&& qAbs(mouse_x - right) < 10){
			setMouseRegion("righttop");
		}
		else if (mouse_y >= top
			&& mouse_y < (top + 10)
			&& qAbs(mouse_x - left) < 10){
			setMouseRegion("lefttop");
		}
		else {
			setMouseRegion("");
		}
	}
	void setMouseRegion(QString region)
	{
		foreach(QString item, regions.keys()) {
			regions.insert(item, false);
		}
			if (!region.isEmpty()) {
			regions.insert(region, true);
		}
	}
	bool getMouseRegion()
	{
		foreach(bool value, regions) {
			if (value)
				return value;
		}
		return false;
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
#ifdef Q_WS_MAC
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

	void setWindowBorder(bool isDecorated){
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
	void mousePressEvent(QMouseEvent *event){
		QWidget *window = BaseClass::window();
		if (!border && (event->button()==Qt::LeftButton)) {
			mouseEnterEvent(event->globalPos().x(), event->globalPos().y(), window->geometry());
			if (getMouseRegion()) {
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
		QPoint pg = event->globalPos();
		QWidget *window = BaseClass::window();
		int ypath = 0;
		int xpath = 0;
		int right = window->geometry().right();
		int left =  window->geometry().left();
		int top =  window->geometry().top();
		int bottom = window->geometry().bottom();
		if (isLeftButton && regions.value("leftbottom") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeBDiagCursor));
			if (pg.y() < bottom) {
				ypath = -(bottom - pg.y());
			}
			else if (pg.y() > bottom) {
				ypath =  pg.y() - bottom;
			}
			if (pg.x() < left) {
				xpath = left - pg.x();
			}
			else if (pg.x() > left) {
				xpath = -(pg.x() - left);
			}
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = -(window->width() - window->minimumWidth());
			}
			window->setGeometry(window->x() - xpath, window->y(), window->width() + xpath, window->height() + ypath);

		}
		else if (isLeftButton && regions.value("rightbottom") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeFDiagCursor));
			if (pg.y() < bottom) {
				ypath = -(bottom - pg.y());
			}
			else {
				ypath =  pg.y() - bottom;
			}
			if (pg.x() < right) {
				xpath = - (right - pg.x());
			}
			else {
				xpath = pg.x() - right;
			}
			window->resize(window->width() + xpath, window->height() + ypath);

		}
		else if (isLeftButton && regions.value("lefttop") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeFDiagCursor));
			if (pg.y() > top) {
				ypath = -(pg.y() - top);
			}
			else {
				ypath =  top - pg.y();
			}
			if (pg.x() > left) {
				xpath = -(pg.x() - left);
			}
			else {
				xpath = left - pg.x();
			}
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = -(window->width() - window->minimumWidth());
			}
			if ((window->height() + ypath) < window->minimumHeight()) {
				ypath = -(window->height() - window->minimumHeight());
			}
			window->setGeometry(window->x() - xpath, window->y() - ypath, window->width() + xpath, window->height() + ypath);
		}
		else if (isLeftButton && regions.value("righttop") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeBDiagCursor));
			if (pg.y() > top) {
				ypath = -(pg.y() - top);
			}
			else {
				ypath =  top - pg.y();
			}
			if (pg.x() < right) {
				xpath = -(right - pg.x());
			}
			else {
				xpath = pg.x() - right;
			}
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = -(window->width() - window->minimumWidth());
			}
			if ((window->height() + ypath) < window->minimumHeight()) {
				ypath = -(window->height() - window->minimumHeight());
			}
			window->setGeometry(window->x(), window->y() - ypath, window->width() + xpath, window->height() + ypath);
		}
		else if (isLeftButton && regions.value("bottom") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeVerCursor));
			if (pg.y() < bottom) {
				ypath = -(bottom - pg.y());
			}
			else {
				ypath =  pg.y() - bottom;
			}
			window->resize(window->width(), window->height() + ypath);
		}
		else if (isLeftButton && regions.value("right") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeHorCursor));
			if (pg.x() < right) {
				xpath = -(right - pg.x());
			}
			else {
				xpath =  pg.x() - right;
			}
			window->resize(window->width() + xpath, window->height());
		}
		else if (isLeftButton && regions.value("left") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeHorCursor));
			if (pg.x() > left) {
				xpath = -(pg.x() - left);
			}
			else {
				xpath =  left - pg.x();
			}
			if ((window->width() + xpath) < window->minimumWidth()) {
				xpath = -(window->width() - window->minimumWidth());
			}
			window->setGeometry(window->x() - xpath, window->y(), window->width() + xpath, window->height());
		}
		else if (isLeftButton && regions.value("top") && isResize && !border) {
			window->setCursor(QCursor(Qt::SizeVerCursor));
			if (pg.y() > top) {
				ypath = -(pg.y() - top);
			}
			else {
				ypath =  top - pg.y();
			}
			if ((window->height() + ypath) < window->minimumHeight()) {
				ypath = -(window->height() - window->minimumHeight());
			}
			window->setGeometry(window->x(), window->y() - ypath, window->width(), window->height() + ypath);
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
