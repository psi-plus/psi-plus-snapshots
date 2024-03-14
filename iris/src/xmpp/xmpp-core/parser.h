/*
 * parser.h - parse an XMPP "document"
 * Copyright (C) 2003-2020  Justin Karneges, Sergey Ilinykh
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

#ifndef PARSER_H
#define PARSER_H

#include <QDomElement>
#include <QExplicitlySharedDataPointer>
#include <QXmlStreamAttributes>

#include <memory>

namespace XMPP {

class Parser {
public:
    struct NSPrefix {
        QString name;
        QString value;
    };

    class Event {
    public:
        enum Type { DocumentOpen, DocumentClose, Element, Error };
        Event();
        Event(const Event &);
        Event &operator=(const Event &);
        ~Event();

        bool isNull() const;
        int  type() const;

        // for document open
        QString nsprefix(const QString &s = QString()) const;

        // for document open / close
        QString              namespaceURI() const;
        QString              localName() const;
        QString              qName() const;
        QXmlStreamAttributes atts() const;

        // for element
        QDomElement element() const;

        // for any
        QString actualString() const;

        // setup
        void setDocumentOpen(const QString &namespaceURI, const QString &localName, const QString &qName,
                             const QXmlStreamAttributes &atts, const QXmlStreamNamespaceDeclarations &nsPrefixes);
        void setDocumentClose(const QString &namespaceURI, const QString &localName, const QString &qName);
        void setElement(const QDomElement &elem);
        void setError();
        void setActualString(const QString &);

    private:
        void ensureD();
        class Private;
        QExplicitlySharedDataPointer<Private> d;
    };

    Parser();
    ~Parser();

    void        reset();
    void        appendData(const QByteArray &a);
    Event       readNext();
    QByteArray  unprocessed() const;
    QStringView encoding() const;

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

#endif // PARSER_H
