/*
 * psitabbar.cpp - Tabbar child for Psi
 * Copyright (C) 2006  Kevin Smith
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

#include "psitabbar.h"
#include "psitabwidget.h"
#include <QMouseEvent>
#include <QApplication>
#include <QPainter>
#include "psioptions.h"
/**
 * Constructor
 */
PsiTabBar::PsiTabBar(PsiTabWidget *parent)
		: QTabBar(parent)
		, dragsEnabled_(true) {
	//setAcceptDrops(true);

#if QT_VERSION >= 0x040500
	setMovable(true);
	setTabsClosable(true);
	setSelectionBehaviorOnRemove ( QTabBar::SelectPreviousTab );
#endif
	currTab=-1;
}

/**
 * Destructor
 */
PsiTabBar::~PsiTabBar() {
}

/**
 * Returns the parent PsiTabWidget.
 */
PsiTabWidget* PsiTabBar::psiTabWidget() {
	return dynamic_cast<PsiTabWidget*> (parent());
}

/**
 * Overriding this allows us to emit signals for double clicks
 */
void PsiTabBar::mouseDoubleClickEvent(QMouseEvent *event) {
	const QPoint pos = event->pos();
	int tab = findTabUnder(pos);
	if (tab >= 0 && tab < count()) {
		emit mouseDoubleClickTab(tab);
	}
}

/*
 * Returns the index of the tab at a position, or -1 if out of bounds.
 */
int PsiTabBar::findTabUnder(const QPoint &pos) {
	for (int i = 0; i < count(); i++) {
		if (tabRect(i).contains(pos)) {
			return i;
		}
	}
	return -1;
} 

void PsiTabBar::mousePressEvent(QMouseEvent *event) {
	QTabBar::mousePressEvent(event);
	event->accept();
}

void PsiTabBar::mouseReleaseEvent ( QMouseEvent * event )
{
	if (event->button() == Qt::MidButton && findTabUnder(event->pos())!=-1) {
		emit mouseMiddleClickTab(findTabUnder(event->pos()));
		event->accept();
	}
	QTabBar::mouseReleaseEvent(event);

#if QT_VERSION >= 0x040500
	if ((dragTab_ != -1) && (event->button() != Qt::MidButton)) {
		this->setCurrentIndex(currentIndex());
	}
#else
	if ((dragTab_ != currTab)&&(currTab!=-1)&&(dragTab_!=-1)) {
		if (dragTab_ < currTab) {
			this->insertTab(isOnTheLeft ? currTab : currTab+1, this->tabIcon(dragTab_), this->tabText(dragTab_));
			this->removeTab(dragTab_);
			emit tabMoved(dragTab_, isOnTheLeft ? currTab-1 : currTab);
		}
		else {
			this->insertTab(isOnTheLeft ? currTab : currTab+1, this->tabIcon(dragTab_), this->tabText(dragTab_));
			this->removeTab(dragTab_+1);
			emit tabMoved(dragTab_, isOnTheLeft ? currTab : currTab+1);
		}
	}

	dragTab_ = -1;
	currTab = -1;
	this->update();
	event->accept();
#endif
};

/*
 * Used for starting drags of tabs
 */
void PsiTabBar::mouseMoveEvent(QMouseEvent *event) {
	if (!dragsEnabled_) {
		return;
	}
	if (!(event->buttons() & Qt::LeftButton)) {
		currTab=-1;
		return;
	}
	if ((event->pos() - dragStartPosition_).manhattanLength()
		< QApplication::startDragDistance()) {
		return;
	}

	if (dragTab_ != -1) {
#if QT_VERSION < 0x040500
	currTab = findTabUnder(event->pos());
	this->update(this->tabRect(currTab));

	if (tabRect(currTab).center().x() - event->pos().x() > 0)
		isOnTheLeft = 1;
	else
		isOnTheLeft = 0;
#endif
	}
#if QT_VERSION >= 0x040500
	QTabBar::mouseMoveEvent(event);
#else
	event->accept();
#endif
}

void PsiTabBar::contextMenuEvent(QContextMenuEvent *event) {
	event->accept();
	emit contextMenu(event, findTabUnder(event->pos()));
}

void PsiTabBar::wheelEvent(QWheelEvent *event) {
	if (PsiOptions::instance()->getOption("options.ui.tabs.disable-wheel-scroll").toBool())
		return;

	int numDegrees = event->delta() / 8;
	int numSteps = numDegrees / 15;

	int newIndex = currentIndex() - numSteps;

	while (newIndex < 0) {
		newIndex += count();
	}
	newIndex = newIndex % count();

	setCurrentIndex(newIndex);

	event->accept();	
}

/*
 * Enable/disable dragging of tabs
 */
void PsiTabBar::setDragsEnabled(bool enabled) {
	dragsEnabled_ = enabled;
}

void PsiTabBar::paintEvent(QPaintEvent *event)
{
	QTabBar::paintEvent(event);
#if QT_VERSION < 0x040500
	QPainter painter(this);
	QPen pen(Qt::cyan);
	pen.setWidth(4);
	pen.setStyle(Qt::SolidLine);
	painter.setPen(pen);
	if ( currTab != -1) {
		if (isOnTheLeft)
			painter.drawLine(tabRect(currTab).topLeft().x()+3, tabRect(currTab).topLeft().y(),
			tabRect(currTab).bottomLeft().x()+3, tabRect(currTab ).bottomLeft().y());
		else
			painter.drawLine(tabRect(currTab).topRight().x(), tabRect(currTab).topRight().y(),
			tabRect(currTab).bottomRight().x(), tabRect(currTab ).bottomRight().y());
	}
#endif
};

void PsiTabBar::resizeEvent(QResizeEvent * event)
{
	QTabBar::resizeEvent(event);
};
