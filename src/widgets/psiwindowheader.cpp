/*
 * psiwindowheader.cpp
 * Copyright (C) 2010  Khryukin Evgeny, Vitaly Tonkacheyev
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
#include <QDesktopWidget>

#include "psiwindowheader.h"
#include "psiiconset.h"

PsiWindowHeader::PsiWindowHeader(QWidget *p)
	: QWidget(p),
	  maximized(false),
	  defaultSize(QSize(320, 280))
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
}

PsiWindowHeader::~PsiWindowHeader()
{
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
	const QRect desktop = qApp->desktop()->availableGeometry(-1);
	if (!maximized) {
		if (parent_->window()->width() != qApp->desktop()->width()
			&& parent_->window()->height() != qApp->desktop()->height()) {
			oldSize = parent_->window()->geometry();
			parent_->window()->setGeometry(desktop);
			maximized = true;
		}
		else if (!oldSize.isNull() && !oldSize.isEmpty()) {
			parent_->window()->setGeometry(oldSize);
			maximized = false;
		}
		else {
			parent_->window()->resize(defaultSize);
			maximized = false;
		}
	} else {
		if (oldSize.top() < desktop.top()) {
			oldSize.setTop(desktop.top());
		}
		if (oldSize.left() < desktop.left() ) {
			oldSize.setLeft(desktop.left());
		}
		if (oldSize.right() > desktop.right()) {
			oldSize.setRight(desktop.right());
		}
		if (oldSize.bottom() > desktop.bottom()) {
			oldSize.setBottom(desktop.bottom());
		}
		parent_->window()->setGeometry(oldSize);
		maximized = false;
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
	if (e->button() == Qt::LeftButton) {
		mouseEnterEvent(e->pos().x(),e->pos().y(),geometry());
		if (inVRect || inLDRect || inRDRect) {
			isResize = true;
		}
		else{
			movepath = e->pos();
			isResize = false;
		}
		isDrag = true;
		e->accept();
	}
}

void PsiWindowHeader::mouseMoveEvent(QMouseEvent *e)
{
	bool isLeftButton = (e->buttons() & Qt::LeftButton);
	const QPoint pg = e->globalPos();
	int ypath = 0;
	int xpath = 0;
	if (isLeftButton && inLDRect && isResize && !maximized) {
		setCursor(QCursor(Qt::SizeFDiagCursor));
		ypath = parent_->window()->y() - pg.y() ;
		xpath = parent_->window()->x() - pg.x();
		if ((parent_->window()->width() + xpath) < parent_->window()->minimumWidth()) {
			xpath = parent_->window()->minimumWidth() - parent_->window()->width();
		}
		if ((parent_->window()->height() + ypath) < parent_->window()->minimumHeight()) {
			ypath = parent_->window()->minimumHeight() - parent_->window()->height();
		}
		parent_->window()->setGeometry(parent_->window()->x() - xpath,
						parent_->window()->y() - ypath,
						parent_->window()->width() + xpath,
						parent_->window()->height() + ypath);

	}
	else if (isLeftButton && inVRect && isResize && !maximized) {
		setCursor(QCursor(Qt::SizeVerCursor));
		ypath = parent_->window()->y() - pg.y();
		if ((parent_->window()->height() + ypath) < parent_->window()->minimumHeight()) {
			ypath = parent_->window()->minimumHeight() - parent_->window()->height();
		}
		parent_->window()->setGeometry(parent_->window()->x(),
						parent_->window()->y() - ypath,
						parent_->window()->width(),
						parent_->window()->height() + ypath);
	}
	else if (isLeftButton && inRDRect && isResize && !maximized) {
		setCursor(QCursor(Qt::SizeBDiagCursor));
		ypath = parent_->window()->y() - pg.y();
		xpath = pg.x() - parent_->window()->geometry().right();
		if ((parent_->window()->height() + ypath) < parent_->window()->minimumHeight()) {
			ypath = parent_->window()->minimumHeight() - parent_->window()->height();
		}
		parent_->window()->setGeometry(parent_->window()->x(),
						parent_->window()->y() - ypath,
						parent_->window()->width() + xpath,
						parent_->window()->height() + ypath);

	}
	else if(isLeftButton && isDrag &&!isResize && !maximized) {
		setCursor(QCursor(Qt::ArrowCursor));
		parent_->window()->move( e->globalPos() - movepath );
	}
	e->accept();
}

void PsiWindowHeader::mouseEnterEvent(const int mouse_x, const int mouse_y, const QRect &geom)
{
	if(mouse_y <= geom.top()+7
		&& qAbs(mouse_x - geom.left()) <= 4) {
		inLDRect = true;
		inRDRect = false;
		inVRect = false;
	}
	else if(mouse_y <= geom.top()+7
		&& qAbs(mouse_x - geom.right()) <= 4) {
		inRDRect = true;
		inLDRect = false;
		inVRect = false;
	}
	else if (mouse_x > (geom.left() + 4)
		&& mouse_x < (geom.right() - 4)
		&& qAbs(mouse_y - geom.top()) <= 4) {
		inVRect = true;
		inLDRect = false;
		inRDRect = false;
	}
	else {
		inVRect = false;
		inLDRect = false;
		inRDRect = false;
	}
}

void PsiWindowHeader::mouseReleaseEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && isDrag) {
		movepath.setX(0);
		movepath.setY(0);
		isDrag = false;
		isResize = false;
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
	e->accept();
}
