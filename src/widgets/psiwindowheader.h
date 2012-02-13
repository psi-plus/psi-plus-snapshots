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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef PSIWINDOWHEADER_H
#define PSIWINDOWHEADER_H

#include "ui_psiwindowheader.h"

#include <QToolButton>
#include <QMouseEvent>
#include <QRect>

class PsiWindowHeader : public QWidget
{
	Q_OBJECT
public:
	PsiWindowHeader(QWidget* p);
	~PsiWindowHeader();

private:
	Ui::PsiWindowHeader ui_;
	QWidget *parent_;
	QPoint movepath;
	bool isDrag;
	bool isResize;
	bool inVRect;
	bool inLDRect;
	bool inRDRect;
	bool maximized;
	QRect oldSize;
	QSize defaultSize;
	void mouseEnterEvent(int mouse_x, int mouse_y, QRect geom);

private slots:
	void hidePressed();
	void closePressed();
	void maximizePressed();

protected:
	void mouseMoveEvent(QMouseEvent *e);
	void mousePressEvent(QMouseEvent *e);
	void mouseReleaseEvent(QMouseEvent *e);
	void mouseDoubleClickEvent(QMouseEvent *e);

};

#endif // PSIWINDOWHEADER_H
