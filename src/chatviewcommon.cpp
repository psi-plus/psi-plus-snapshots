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
			quint32 num = qHash(nickwoun);
			int s = ((char*)&num)[2];
			int v = ((char*)&num)[3];
			QColor precolor = QColor::fromHsv((num >> 6) % 360,
					s < 150? 250 : s, v > 210? 210 : v < 100? 150 : v);
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
