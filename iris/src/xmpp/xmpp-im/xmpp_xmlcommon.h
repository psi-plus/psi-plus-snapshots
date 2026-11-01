/*
 * xmlcommon.h - helper functions for dealing with XML
 * Copyright (C) 2001-2002  Justin Karneges
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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_XMLCOMMON_H
#define XMPP_XMLCOMMON_H

#include <QDomDocument>
#include <QDomNode>
#include <QtGlobal>
#include <QList>

class QColor;
class QDateTime;
class QRect;
class QSize;

class XDomNodeList {
public:
    XDomNodeList();
    XDomNodeList(const XDomNodeList &from);
    XDomNodeList(const QDomNodeList &from);
    ~XDomNodeList();
    XDomNodeList &operator=(const XDomNodeList &from);

    QDomNode at(int index) const { return item(index); }
    int      count() const { return (int)length(); }
    bool     isEmpty() const;
    QDomNode item(int index) const;
    uint     length() const;
    int      size() const { return (int)length(); }

    void append(const QDomNode &i);

    bool operator==(const XDomNodeList &a) const;

    bool operator!=(const XDomNodeList &a) const { return !operator==(a); }

private:
    QList<QDomNode> list;
};

QDateTime    stamp2TS(const QString &ts);
bool         stamp2TS(const QString &ts, QDateTime *d);
QString      TS2stamp(const QDateTime &d);
QDomElement  textTag(QDomDocument *doc, const QString &name, const QString &content);
QDomElement  textTagNS(QDomDocument *doc, const QString &ns, const QString &name, const QString &content);
QString      tagContent(const QDomElement &e);
// Match the expanded XML name (namespace + local name), regardless of prefix.
bool         isElementNS(const QDomElement &e, const QString &nsURI, const QString &localName);
XDomNodeList childElementsByTagNameNS(const QDomElement &e, const QString &nsURI, const QString &localName);
// Find the first direct child by local name, regardless of namespace.
// For protocol recognition, check the returned element's namespaceURI().
QDomElement  firstChildElementByLocalName(const QDomElement &e, const QString &localName);
QDomElement  createIQ(QDomDocument *doc, const QString &type, const QString &to, const QString &id);
QDomElement  queryTag(const QDomElement &e);
QString      queryNS(const QDomElement &e);
void         getErrorFromElement(const QDomElement &e, const QString &baseNS, int *code, QString *str);
QDomElement  addCorrectNS(const QDomElement &e);

namespace XMLHelper {

// Parse XMPP XML with namespace processing under both Qt 5 and Qt 6.
// Qt 6.5 replaced the bool overload with ParseOptions/ParseResult.
// The explicit bool return also works with Qt 6.5's explicit conversion.
template<typename Source>
inline bool parseDocument(QDomDocument &document, const Source &xml, bool useNamespaces = true)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const auto options = useNamespaces ? QDomDocument::ParseOption::UseNamespaceProcessing
                                       : QDomDocument::ParseOption::Default;
    return bool(document.setContent(xml, options));
#else
    return document.setContent(xml, useNamespaces);
#endif
}


// QDomElement findSubTag(const QDomElement &e, const QString &name, bool *found);
bool hasSubTag(const QDomElement &e, const QString &name);

QDomElement emptyTag(QDomDocument *doc, const QString &name);
QDomElement emptyTagNS(QDomDocument *doc, const QString &ns, const QString &name);
QString     subTagText(const QDomElement &e, const QString &name);

QDomElement textTag(QDomDocument &doc, const QString &name, const QString &content);
QDomElement textTag(QDomDocument &doc, const QString &name, qint64 content);
QDomElement textTag(QDomDocument &doc, const QString &name, bool content);
QDomElement textTag(QDomDocument &doc, const QString &name, QSize &s);
QDomElement textTag(QDomDocument &doc, const QString &name, QRect &r);
QDomElement textTagNS(QDomDocument *doc, const QString &ns, const QString &name, const QString &content);
QDomElement textTagNS(QDomDocument *doc, const QString &ns, const QString &name, const QByteArray &content);
void        setTagText(QDomElement &e, const QString &text);
QDomElement stringListToXml(QDomDocument &doc, const QString &name, const QStringList &l);

void readEntry(const QDomElement &e, const QString &name, QString *v);
void readNumEntry(const QDomElement &e, const QString &name, int *v);
void readBoolEntry(const QDomElement &e, const QString &name, bool *v);
void readSizeEntry(const QDomElement &e, const QString &name, QSize *v);
void readRectEntry(const QDomElement &e, const QString &name, QRect *v);
void readColorEntry(const QDomElement &e, const QString &name, QColor *v);

void xmlToStringList(const QDomElement &e, const QString &name, QStringList *v);

void setBoolAttribute(QDomElement e, const QString &name, bool b);
void readBoolAttribute(QDomElement e, const QString &name, bool *v);

// QString tagContent(const QDomElement &e); // obsolete;
QString sanitizedLang(const QString &lang);

} // namespace XMLHelper

#endif // XMPP_XMLCOMMON_H
