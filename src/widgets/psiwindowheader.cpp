/*
 * psiwindowheader.cpp
 * Copyright (C) 2010-2017  Evgeny Khryukin, Vitaly Tonkacheyev
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


#include <QIcon>
#include <QApplication>

#include "psiwindowheader.h"
#include "psiiconset.h"

PsiWindowHeader::PsiWindowHeader(QWidget *p)
	: QWidget(p),
	  maximized_(false)
{
	parent_ = p->window();
	ui_.setupUi(this);
#ifdef Q_OS_MAC
	ui_.horiz->insertWidget(0, ui_.closeButton);
	ui_.horiz->insertWidget(1, ui_.hideButton);
	ui_.horiz->insertWidget(2, ui_.maximizeButton);
#endif
	ui_.hideButton->setIcon(qApp->style()->standardIcon(QStyle::SP_TitleBarMinButton));
	ui_.maximizeButton->setIcon(qApp->style()->standardIcon(QStyle::SP_TitleBarMaxButton));
	ui_.closeButton->setIcon(qApp->style()->standardIcon(QStyle::SP_TitleBarCloseButton));
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

	connect(ui_.hideButton, SIGNAL(clicked()), SLOT(hidePressed()));
	connect(ui_.closeButton, SIGNAL(clicked()), SLOT(closePressed()));
	connect(ui_.maximizeButton, SIGNAL(clicked()), SLOT(maximizePressed()));
	setMouseTracking(true);
	enableMouseTracking(true);
}

PsiWindowHeader::~PsiWindowHeader()
{
	//Disable mouse tracking on widget deletion
	enableMouseTracking(false);
}

void PsiWindowHeader::hidePressed()
{
	parent_->setWindowState(parent_->windowState() | Qt::WindowMinimized);
}

void PsiWindowHeader::closePressed()
{
	parent_->close();
}

void PsiWindowHeader::maximizePressed()
{
	if(parent_->window()->windowState() != Qt::WindowMaximized) {
		parent_->window()->showMaximized();
		maximized_ = true;
	}
	else {
		parent_->window()->showNormal();
		maximized_ = false;
	}
}

void PsiWindowHeader::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton) {
		maximizePressed();
		e->accept();
	}
}

void PsiWindowHeader::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && isVisible()) {
		region_ = getMouseRegion(e->globalPos().x(), e->globalPos().y(), parent_->window()->geometry());
		if (region_ != Qt::NoSection) {
			action_ = WinAction::Resizing;
		}
		else{
			movePath_ = e->globalPos() - parent_->window()->pos();
			action_ = WinAction::Dragging;
		}
		e->accept();
	}
}

void PsiWindowHeader::mouseMoveEvent(QMouseEvent *e)
{
	if(isVisible()) {
		bool isLeftButton = (e->buttons() & Qt::LeftButton);
		const QPoint pg = e->globalPos();
		if (!isLeftButton && !maximized_) {
			Qt::WindowFrameSection region = getMouseRegion(pg.x(), pg.y(), parent_->window()->geometry());
			updateCursor(region);
		}
		else if(isLeftButton && action_ == WinAction::Resizing && !maximized_) {
			doWindowResize(parent_->window(), pg, region_);
		}
		else if(isLeftButton && action_ == WinAction::Dragging && !maximized_) {
			setCursor(QCursor(Qt::SizeAllCursor));
			parent_->window()->move( pg - movePath_ );
		}
	}
	e->accept();
}

void PsiWindowHeader::doWindowResize(QWidget* window, const QPoint& eventPos, Qt::WindowFrameSection region)
{
	int ypath = 0;
	int xpath = 0;
	const QRect winGeom = window->geometry();
	const int right = winGeom.right();
	const int left =  winGeom.left();
	const int top =  winGeom.top();
	switch(region) {
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
	case(Qt::NoSection):
	default:
		break;
	}
}

Qt::WindowFrameSection PsiWindowHeader::getMouseRegion(const int mouse_x, const int mouse_y, const QRect &geom) const
{
	const int mouseAccuracy = 7;
	const int top = geom.top();
	const int left = geom.left();
	const int right = geom.right();
	const int maxtop = top + mouseAccuracy;
	if(qAbs(top - mouse_y) < mouseAccuracy
		&& qAbs(mouse_x - left) < mouseAccuracy) {
		return Qt::TopLeftSection;
	}
	else if(qAbs(top -mouse_y) < mouseAccuracy
		&& qAbs(mouse_x - right) < mouseAccuracy) {
		return Qt::TopRightSection;
	}
	else if (mouse_x > (left + mouseAccuracy)
		&& mouse_x < (right - mouseAccuracy)
		&& qAbs(mouse_y - top) < mouseAccuracy) {
		return Qt::TopSection;
	}
	else if (qAbs(right - mouse_x) < mouseAccuracy
			 &&  mouse_y > maxtop) {
		return Qt::RightSection;
	}
	else if (qAbs(mouse_x - left) < mouseAccuracy
		 &&  mouse_y > maxtop) {
		return Qt::LeftSection;
	}
	return Qt::NoSection;
}

void PsiWindowHeader::mouseReleaseEvent(QMouseEvent *e)
{
	if(isVisible()) {
		if (e->button() == Qt::LeftButton && action_ == WinAction::Dragging) {
			movePath_ = QPoint(0,0);
			action_ = WinAction::None;
			setCursor(QCursor(Qt::ArrowCursor));
		}
		int min_x = qMin(ui_.hideButton->geometry().left(), qMin(ui_.maximizeButton->geometry().left(), ui_.closeButton->geometry().left()));
		int max_x = qMax(ui_.hideButton->geometry().right(), qMax(ui_.maximizeButton->geometry().right(), ui_.closeButton->geometry().right()));
		if (e->button() == Qt::MidButton) {
			if (((e->x() > geometry().left() && e->x() < min_x)
				|| (e->x() < geometry().right() && e->x() > max_x ))
				&& e->y() > geometry().top()
				&& e->y() < geometry().bottom()) {
				hidePressed();
			}
		}
	}
	e->accept();
}

void PsiWindowHeader::updateCursor(Qt::WindowFrameSection region)
{
	switch (region) {
	case Qt::TopLeftSection:
		setCursor(QCursor(Qt::SizeFDiagCursor));
		break;
	case Qt::TopRightSection:
		setCursor(QCursor(Qt::SizeBDiagCursor));
		break;
	case Qt::RightSection:
		setCursor(QCursor(Qt::SizeHorCursor));
		break;
	case Qt::LeftSection:
		setCursor(QCursor(Qt::SizeHorCursor));
		break;
	case Qt::TopSection:
		setCursor(QCursor(Qt::SizeVerCursor));
		break;
	case Qt::NoSection:
	default:
		setCursor(QCursor(Qt::ArrowCursor));
		break;
	}
}

void PsiWindowHeader::enableMouseTracking(bool enabled)
{
	//Dirty hack to enable mouse tracking for psichatdlg
	foreach (QWidget *w, qApp->allWidgets()) {
		w->setMouseTracking(enabled);
	}
}
