/*
 * parser.cpp - parse an XMPP "document"
 * Copyright (C) 2020  Sergey Ilinykh
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

#include "parser.h"

#include <queue>

namespace XMPP {

//----------------------------------------------------------------------------
// Event
//----------------------------------------------------------------------------
class Parser::Event::Private : public QSharedData {
public:
    int                  type;
    QString              ns, ln, qn;
    QXmlStreamAttributes a;
    QDomElement          e;
    QString              str;

    QXmlStreamNamespaceDeclarations nsPrefixes;
};

Parser::Event::Event() { }

Parser::Event::Event(const Event &from) : d(from.d) { }

Parser::Event &Parser::Event::operator=(const Event &from)
{
    d = from.d;
    return *this;
}

Parser::Event::~Event() { }

bool Parser::Event::isNull() const { return d == nullptr; }

void Parser::Event::ensureD()
{
    if (!d)
        d = new Private();
}

int Parser::Event::type() const
{
    if (isNull())
        return -1;
    return d->type;
}

QString Parser::Event::nsprefix(const QString &s) const
{
    Q_ASSERT(d != nullptr);
    auto it
        = std::find_if(d->nsPrefixes.cbegin(), d->nsPrefixes.cend(), [&](auto const &v) { return v.prefix() == s; });
    if (it == d->nsPrefixes.cend())
        return QString();
    return it->namespaceUri().toString();
}

QString Parser::Event::namespaceURI() const
{
    Q_ASSERT(d != nullptr);
    return d->ns;
}

QString Parser::Event::localName() const
{
    Q_ASSERT(d != nullptr);
    return d->ln;
}

QString Parser::Event::qName() const
{
    Q_ASSERT(d != nullptr);
    return d->qn;
}

QXmlStreamAttributes Parser::Event::atts() const
{
    Q_ASSERT(d != nullptr);
    return d->a;
}

QString Parser::Event::actualString() const
{
    Q_ASSERT(d != nullptr);
    return d->str;
}

QDomElement Parser::Event::element() const
{
    Q_ASSERT(d != nullptr);
    return d->e;
}

void Parser::Event::setDocumentOpen(const QString &namespaceURI, const QString &localName, const QString &qName,
                                    const QXmlStreamAttributes &atts, const QXmlStreamNamespaceDeclarations &nsPrefixes)
{
    ensureD();
    d->type       = DocumentOpen;
    d->ns         = namespaceURI;
    d->ln         = localName;
    d->qn         = qName;
    d->a          = atts;
    d->nsPrefixes = nsPrefixes;
}

void Parser::Event::setDocumentClose(const QString &namespaceURI, const QString &localName, const QString &qName)
{
    ensureD();
    d->type = DocumentClose;
    d->ns   = namespaceURI;
    d->ln   = localName;
    d->qn   = qName;
}

void Parser::Event::setElement(const QDomElement &elem)
{
    ensureD();
    d->type = Element;
    d->e    = elem;
}

void Parser::Event::setError()
{
    ensureD();
    d->type = Error;
}

void Parser::Event::setActualString(const QString &str)
{
    ensureD();
    d->str = str;
}

//----------------------------------------------------------------------------
// Parser
//----------------------------------------------------------------------------
class Parser::Private {
public:
    QDomDocument          doc;
    QDomElement           curElement;
    QDomElement           element; // root part
    std::list<QByteArray> in;
    QXmlStreamReader      reader;
    const char *          completeTag    = nullptr; // this is basically a workaround for bugs like QTBUG-14661
    int                   completeOffset = 0;
    bool                  streamOpened   = false;
    bool                  readerStarted  = false;
    std::queue<Event>     events;
    QString               streamQName;

    void pushDataToReader()
    {
        if (completeTag) {
            readerStarted = true;
            while (!in.empty()) {
                if (in.front().constData() != completeTag) {
                    reader.addData(in.front());
                    in.erase(in.begin());
                } else {
                    // Qt has some bugs, so ensure we push data only ending with '>'
                    if (completeOffset == in.front().size() - 1) {
                        reader.addData(in.front());
                        in.erase(in.begin());
                    } else {
                        QByteArray part = in.front().left(completeOffset + 1);
                        reader.addData(part);
                        in.front().remove(0, completeOffset + 1);
                    }
                    completeTag = nullptr;
                    break;
                }
            }
        }
    }

    void handleStartElement()
    {
        auto    ns   = reader.namespaceUri().toString();
        QString name = reader.name().toString();
        if (streamOpened) {
            QDomElement newEl;
            if (ns.isEmpty())
                newEl = doc.createElement(name);
            else
                newEl = doc.createElementNS(ns, name);
            if (curElement.isNull()) {
                curElement = newEl;
                element    = newEl;
            } else {
                curElement = curElement.appendChild(newEl).toElement();
            }

            const auto &attrs = reader.attributes();
            for (auto const &a : attrs) {
                QDomAttr da;
                if (a.namespaceUri().isEmpty())
                    da = doc.createAttribute(a.name().toString());
                else
                    da = doc.createAttributeNS(a.namespaceUri().toString(), a.name().toString());
                da.setPrefix(a.prefix().toString());
                da.setValue(a.value().toString());
                if (a.namespaceUri().isEmpty())
                    curElement.setAttributeNode(da);
                else
                    curElement.setAttributeNodeNS(da);
            }
        } else {
            Event e;
            streamQName = reader.qualifiedName().toString();
            e.setDocumentOpen(ns, name, streamQName, reader.attributes(), reader.namespaceDeclarations());
            events.push(e);
            streamOpened = true;
        }
    }

    void handleEndElement()
    {
        if (curElement.isNull() && reader.qualifiedName() == streamQName) {
            Event e;
            e.setDocumentClose(reader.namespaceUri().toString(), reader.name().toString(), streamQName);
            events.push(e);
            return;
        }
        Q_ASSERT_X(!curElement.isNull(), "xml parser", "XML reader hasn't reported error for invalid element close");
        Q_ASSERT_X(
            curElement.namespaceURI() == reader.namespaceUri() && curElement.tagName() == reader.name(), "xml parser",
            qPrintable(
                QString("XML reader hasn't reported open/close tags mismatch. expected close for <%1 xmlns=\"%2\"> "
                        "but got close for <%3 xmlns=\"%4\">")
                    .arg(curElement.tagName(), curElement.namespaceURI(), reader.name().toString(),
                         reader.namespaceUri().toString())));
#if 0
        if (element.isNull()) {
            Event e;
            qWarning("xml parser: closing not existing element: %s", qPrintable(reader.qualifiedName().toString()));
            e.setError();
            return;
        }

        if (!(element.namespaceURI() == reader.namespaceUri() && element.tagName() == reader.name())) {
            Event e;
            qWarning("XML reader hasn't reported open/close tags mismatch: %s vs %s", qPrintable(element.tagName()),
                     qPrintable(reader.qualifiedName().toString()));
            e.setError();
            return;
        }
#endif
        if (curElement.parentNode().isNull()) {
            Event e;
            e.setElement(curElement);
            events.push(e);
        }
        curElement = curElement.parentNode().toElement();
    }

    void handleText()
    {
        if (curElement.isNull()) {
            if (!reader.isWhitespace())
                qWarning("Text node out of element (ignored): %s", qPrintable(reader.text().toString()));
            return;
        }
        auto node = doc.createTextNode(reader.text().toString());
        curElement.appendChild(node);
    }

    void collectEvents()
    {
        auto tt = reader.readNext();
        while (tt != QXmlStreamReader::NoToken && tt != QXmlStreamReader::Invalid) {
            if (tt == QXmlStreamReader::StartElement) {
                handleStartElement();
            } else if (tt == QXmlStreamReader::EndElement) {
                handleEndElement();
            } else if (tt == QXmlStreamReader::Characters) {
                handleText();
            } else {
                Q_ASSERT_X(tt != QXmlStreamReader::EntityReference, "xml parser",
                           qPrintable(QString("unexpected xml entity: %1").arg(reader.text())));
            }
            tt = reader.readNext();
        }
        if (tt == QXmlStreamReader::Invalid) {
            if (reader.error() == QXmlStreamReader::PrematureEndOfDocumentError)
                return;
            qDebug("xml parser error: %s", qPrintable(reader.errorString()));
            Event e;
            e.setError();
            events.push(e);
        }
    }

    Parser::Event readNext()
    {
        Event e;
        pushDataToReader();
        if (!readerStarted)
            return e;
        collectEvents();
        if (!events.empty()) {
            e = events.front();
            events.pop();
        }
        return e;
    }
};

Parser::Parser() { reset(); }

Parser::~Parser() { }

void Parser::reset() { d.reset(new Private); }

void Parser::appendData(const QByteArray &a)
{
    if (a.isEmpty())
        return;
    d->in.push_back(a);
    for (int i = a.size() - 1; i >= 0; --i) {
        if (a.at(i) == '>') { // this may happend in CDATA too, but let's hope Qt handles it properly
            d->completeTag    = a.constData();
            d->completeOffset = i;
            break;
        }
    }
}

Parser::Event Parser::readNext() { return d->readNext(); }

QByteArray Parser::unprocessed() const
{
    QByteArray ret;
    for (auto const &a : d->in) {
        ret += a;
    }
    return ret;
}

QStringView Parser::encoding() const { return d->reader.documentEncoding(); }

}
