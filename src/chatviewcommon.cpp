/*
 * chatviewcommon.cpp - shared part of any chatview
 * Copyright (C) 2010 Rion
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

#include <QWidget>
#include <QColor>
#include <QRegExp>
#include <QApplication>

#include "chatviewcommon.h"
#include "psioptions.h"

void ChatViewCommon::setLooks(QWidget *w)
{
	QPalette pal = w->palette(); // copy widget's palette to non const QPalette
	pal.setColor(QPalette::Inactive, QPalette::HighlightedText,
				 pal.color(QPalette::Active, QPalette::HighlightedText));
	pal.setColor(QPalette::Inactive, QPalette::Highlight,
				 pal.color(QPalette::Active, QPalette::Highlight));
	w->setPalette(pal);        // set the widget's palette
}

bool ChatViewCommon::updateLastMsgTime(QDateTime t)
{
	bool doInsert = t.date() != _lastMsgTime.date();
	_lastMsgTime = t;
	return doInsert;
}

QString ChatViewCommon::getMucNickColor(const QString &nick, bool isSelf, QStringList validList)
{
	const char* rgbBlack = "#000000";
	QString nickwoun = nick; // nick without underscores
	nickwoun.replace(QRegExp("^_*"), "");
	nickwoun.replace(QRegExp("_*$"), "");

	int sender;
	if(isSelf || nickwoun.isEmpty()) {
		sender = -1;
	} else {
		if (!_nicks.contains(nickwoun)) {
			//not found in map
			_nicks.insert(nickwoun,_nickNumber);
			_nickNumber++;
		}
		sender=_nicks[nickwoun];
	}

	QStringList nickColors = validList.isEmpty()
		? PsiOptions::instance()->getOption("options.ui.look.colors.muc.nick-colors").toStringList()
		: validList;

	if(!PsiOptions::instance()->getOption("options.ui.muc.use-nick-coloring").toBool()) {
		return rgbBlack;
	} else {
		if (PsiOptions::instance()->getOption("options.ui.muc.use-hash-nick-coloring").toBool()) {
			/* Hash-driven colors */
			Q_ASSERT(nickwoun.size());
			quint32 hash = qHash(nickwoun); // main hash
			QColor bg = QApplication::palette().color(QPalette::Base); // background color
			int bgH = bg.hue() >= 0? bg.hue() : 60; // 60 == yellow
			int bgV = bg.lightness() >= 0? bg.lightness() : 255; // 255 == white
			int bgA = bg.alpha() >= 0? bg.alpha() : 255; // 255 == fully opaque color
			int t = 15*(hash%21); // scale to 0-300 range with limited palette
			int h = bgH + t + 60 ; // do not use colors close to background color
			while (h > 359) h -= 360; // use only 0-359 range
			int v = bgV > 127?
					100: // for bright themes
					155; // for dark themes
			int a = bgA; // use the same transparency as in background color
			int s = 255; // use only clear colors
			QColor precolor = QColor::fromHsv(h, s, v, a);
			return precolor.name();
		} else {
			/* Colors from list */
			if (nickColors.empty()) {
				return rgbBlack;
			}
			if(sender == -1 || nickColors.size() == 1) {
				return nickColors[nickColors.size()-1];
			} else {
				int n = sender % (nickColors.size()-1);
				return nickColors[n];
			}
		}
	}
}
