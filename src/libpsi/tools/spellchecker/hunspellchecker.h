/*
 * hunspellchecker.h
 *
 * Copyright (C) 2015  Ili'nykh Sergey, Vitaly Tonkacheyev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * You can also redistribute and/or modify this program under the
 * terms of the Psi License, specified in the accompanied COPYING
 * file, as published by the Psi Project; either dated January 1st,
 * 2005, or (at your option) any later version.
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
#ifndef HUNSPELLCHECKER_H
#define HUNSPELLCHECKER_H

#include <QList>
#include <QString>
#include <QLocale>
#include <QFileInfo>
#include <QStringList>
#include <QSharedPointer>
#include "spellchecker.h"

class Hunspell;
class QTextCodec;

typedef QSharedPointer<Hunspell> HunspellPtr;

class HunspellChecker : public SpellChecker
{
public:
	HunspellChecker();
	~HunspellChecker();
	virtual QList<QString> suggestions(const QString&);
	virtual bool isCorrect(const QString &word);
	virtual bool add(const QString &word);
	virtual bool available() const;
	virtual bool writable() const;
private:
	struct DictInfo
	{
		QLocale::Language language;
		QLocale::Country country;
		QString filename;
	};
	struct LangItem {
		HunspellPtr hunspell_;
		DictInfo info;
		QTextCodec *codec;
	};
	void getSupportedLanguages();
	void addLanguage(const QLocale &locale);
	void getDictPaths();
	bool scanDictPaths(const QString &language, QFileInfo &aff , QFileInfo &dic);

private:
	QList<LangItem> languages_;
	QStringList dictPaths_;
	QList<QLocale> supportedLangs_;
};

#endif // HUNSPELLCHECKER_H
