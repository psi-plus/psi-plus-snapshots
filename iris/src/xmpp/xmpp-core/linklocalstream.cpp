/* Copyright (C) 2026 Psi contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "parser.h"
#include "xmpp_linklocalstream.h"
#include "xmpp_stanza.h"
#include <iris/bytestream.h>

#include <QAbstractSocket>
#include <QDomDocument>
#include <QPointer>
#include <QQueue>
#include <QTimer>

namespace XMPP {
namespace {
    const auto    StreamNs = QStringLiteral("http://etherx.jabber.org/streams");
    const auto    ClientNs = QStringLiteral("jabber:client");
    constexpr int MaxInput = 1024 * 1024;

    bool validEndpoint(const Jid &jid) { return jid.isValid() && !jid.node().isEmpty() && jid.isBare(); }
}

class LinkLocalStream::Private : public QObject {
public:
    LinkLocalStream           *q;
    QPointer<QIODevice>        io;
    Jid                        local, peer;
    Direction                  direction;
    Parser                     parser;
    QDomDocument               document;
    QQueue<QPair<Stanza, int>> received;
    QTimer                     deadline;
    bool                       started = false, headerSent = false, headerReceived = false, ready = false;
    bool                       closing = false, closed = false, remoteClosed = false, versionOne = false;
    int                        pendingBytes = 0, queuedChars = 0, condition = GenericStreamError;
    QString                    errorText;

    Private(LinkLocalStream *stream, QIODevice *transport, const Jid &from, const Jid &to, Direction role) :
        QObject(stream), q(stream), io(transport), local(from), peer(to), direction(role)
    {
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, this, [this] {
            fail(ErrProtocol, ConnectionTimeout, QStringLiteral("Link-local stream handshake timed out"));
        });
        if (io) {
            Q_ASSERT(io->thread() == thread());
            connect(io, &QIODevice::readyRead, this, [this] { pump(); });
            connect(io, &QObject::destroyed, this, [this] {
                io = nullptr;
                disconnected();
            });
            if (auto socket = qobject_cast<QAbstractSocket *>(io.data()))
                connect(socket, &QAbstractSocket::disconnected, this, [this] { disconnected(); });
            else if (auto stream = qobject_cast<ByteStream *>(io.data()))
                connect(stream, &ByteStream::connectionClosed, this, [this] { disconnected(); });
            else
                connect(io, &QIODevice::aboutToClose, this, [this] { disconnected(); });
        }
        QTimer::singleShot(0, this, [this] { start(); });
    }

    void stopTransport()
    {
        if (!io)
            return;
        if (auto socket = qobject_cast<QAbstractSocket *>(io.data()))
            socket->disconnectFromHost(); // flush the mirrored closing tag
        else
            io->close();
    }

    void disconnected()
    {
        if (closed)
            return;
        const bool clean = closing && remoteClosed;
        closed           = true;
        deadline.stop();
        const QPointer<Private> guard(this);
        if (!clean && errorText.isEmpty()) {
            condition = GenericStreamError;
            errorText = QStringLiteral("Link-local transport closed before the XML stream handshake completed");
            emit q->error(ErrProtocol);
        }
        if (guard)
            emit q->connectionClosed();
        if (guard && clean)
            emit q->delayedCloseFinished();
    }

    void fail(int error, int cond, const QString &text)
    {
        if (closed)
            return;
        condition = cond;
        errorText = text;
        closed    = true;
        deadline.stop();
        received.clear();
        const QPointer<Private> guard(this);
        stopTransport();
        if (guard)
            emit q->error(error);
        if (guard)
            emit q->connectionClosed();
    }

    bool send(const QByteArray &data)
    {
        if (closed)
            return false;
        const QPointer<Private> guard(this);
        const qint64            written = io ? io->write(data) : -1;
        if (!guard)
            return false;
        if (written != data.size()) {
            fail(ErrProtocol, GenericStreamError, QStringLiteral("Cannot write to link-local transport"));
            return false;
        }
        return true;
    }

    bool sendHeader(bool modern)
    {
        if (headerSent)
            return !closed;
        headerSent = true;
        return send(QStringLiteral("<?xml version='1.0'?><stream:stream xmlns='jabber:client' "
                                   "xmlns:stream='http://etherx.jabber.org/streams' from=\"%1\" to=\"%2\"%3>")
                        .arg(local.full().toHtmlEscaped(), peer.full().toHtmlEscaped(),
                             modern ? QStringLiteral(" version='1.0'") : QString())
                        .toUtf8());
    }

    void start()
    {
        if (closed)
            return;
        if (!io || !io->isOpen() || !io->isReadable() || !io->isWritable() || !validEndpoint(local)
            || (direction == Direction::Initiator && !validEndpoint(peer))) {
            fail(ErrProtocol, InvalidFrom, QStringLiteral("Invalid link-local stream endpoints or transport"));
            return;
        }
        deadline.start(10000);
        started = true;
        if (direction == Direction::Initiator && !sendHeader(true))
            return;
        pump(); // accepted sockets may already contain the opening header
    }

    void establish()
    {
        ready = true;
        deadline.stop();
        emit q->connected();
    }

    void event(const Parser::Event &event)
    {
        if (event.type() == Parser::Event::Error) {
            fail(ErrParse, InvalidXml, QStringLiteral("Malformed link-local XML stream"));
            return;
        }
        if (event.type() == Parser::Event::DocumentOpen) {
            const auto attributes = event.atts();
            const Jid  from(attributes.value(QStringLiteral("from")).toString());
            const auto toText = attributes.value(QStringLiteral("to")).toString();
            // XEP-0174 recommends header addresses. The expected peer may
            // supply a missing 'from', but an incoming unknown peer may not.
            const Jid resolvedPeer = from.isEmpty() ? peer : from;
            if (headerReceived || event.namespaceURI() != StreamNs || event.localName() != QLatin1String("stream")
                || event.nsprefix() != ClientNs || !validEndpoint(resolvedPeer)
                || (!attributes.value(QStringLiteral("from")).isEmpty() && !validEndpoint(from))
                || (!peer.isEmpty() && peer != resolvedPeer) || (!toText.isEmpty() && Jid(toText) != local)) {
                fail(ErrProtocol, InvalidFrom, QStringLiteral("Link-local stream header does not match its endpoints"));
                return;
            }
            peer               = resolvedPeer;
            headerReceived     = true;
            const auto version = attributes.value(QStringLiteral("version")).toString();
            if (!version.isEmpty() && version != QLatin1String("1.0")) {
                fail(ErrProtocol, PolicyViolation, QStringLiteral("Unsupported link-local stream version"));
                return;
            }
            versionOne = version == QLatin1String("1.0");
            if (direction == Direction::Recipient) {
                if (!sendHeader(versionOne))
                    return;
                if (versionOne && !send("<stream:features/>"))
                    return;
                establish();
            } else if (!versionOne)
                establish();
            return;
        }
        if (event.type() == Parser::Event::DocumentClose) {
            remoteClosed = true;
            if (!closing) {
                closing = true;
                if (!send("</stream:stream>"))
                    return;
            }
            stopTransport();
            return;
        }
        if (event.type() != Parser::Event::Element)
            return;
        const auto element = event.element();
        if (!ready && headerReceived && element.namespaceURI() == StreamNs
            && element.tagName() == QLatin1String("features") && direction == Direction::Initiator) {
            const auto tls = element.firstChildElement(QStringLiteral("starttls"));
            if (!tls.firstChildElement(QStringLiteral("required")).isNull()) {
                fail(ErrProtocol, PolicyViolation,
                     QStringLiteral("Peer requires STARTTLS; this link-local stream does not negotiate TLS"));
                return;
            }
            establish();
            return;
        }
        if (!ready || element.namespaceURI() != ClientNs
            || (element.tagName() != QLatin1String("message") && element.tagName() != QLatin1String("iq"))) {
            fail(ErrProtocol, PolicyViolation, QStringLiteral("Unexpected element in link-local stream"));
            return;
        }
        auto stanza = Stanza::fromXml(element);
        if ((!stanza.from().isEmpty() && stanza.from() != peer) || (!stanza.to().isEmpty() && stanza.to() != local)) {
            fail(ErrProtocol, InvalidFrom, QStringLiteral("Link-local stanza attempted third-party addressing"));
            return;
        }
        if (stanza.from().isEmpty())
            stanza.setFrom(peer);
        if (stanza.to().isEmpty())
            stanza.setTo(local);
        const int chars = element.text().size() + Stream::xmlToString(element).size();
        queuedChars += chars;
        if (queuedChars > MaxInput || received.size() >= 256) {
            fail(ErrProtocol, ResourceConstraint, QStringLiteral("Link-local stanza queue exceeded its limit"));
            return;
        }
        received.enqueue(qMakePair(stanza, chars));
        emit q->readyRead();
    }

    void pump()
    {
        if (!started || closed || !io)
            return;
        const QPointer<Private> guard(this);
        while (io && io->bytesAvailable() > 0 && !closed) {
            const auto input = io->read(4096);
            if (input.isEmpty())
                break;
            pendingBytes += input.size();
            if (pendingBytes > MaxInput) {
                fail(ErrProtocol, ResourceConstraint, QStringLiteral("Link-local XML element exceeded its limit"));
                return;
            }
            parser.appendData(input);
            for (auto next = parser.readNext(); !next.isNull(); next = parser.readNext()) {
                pendingBytes = 0;
                event(next);
                if (!guard || closed)
                    return;
            }
        }
    }
};

LinkLocalStream::LinkLocalStream(QIODevice *transport, const Jid &local, const Jid &peer, Direction direction,
                                 QObject *parent) :
    Stream(parent), d(new Private(this, transport, local, peer, direction))
{
}
LinkLocalStream::~LinkLocalStream() { delete d; }
Jid           LinkLocalStream::localJid() const { return d->local; }
Jid           LinkLocalStream::peerJid() const { return d->peer; }
QDomDocument &LinkLocalStream::doc() const { return d->document; }
QString       LinkLocalStream::baseNS() const { return ClientNs; }
bool          LinkLocalStream::old() const { return d->headerReceived && !d->versionOne; }
bool          LinkLocalStream::stanzaAvailable() const { return !d->received.isEmpty(); }
Stanza        LinkLocalStream::read()
{
    if (d->received.isEmpty())
        return {};
    auto item = d->received.dequeue();
    d->queuedChars -= item.second;
    return item.first;
}
void LinkLocalStream::write(const Stanza &stanza)
{
    if (!d->ready || d->closing || d->closed)
        return;
    if (stanza.isNull() || stanza.element().namespaceURI() != ClientNs
        || (stanza.kind() != Stanza::Message && stanza.kind() != Stanza::IQ)
        || (!stanza.from().isEmpty() && stanza.from() != d->local)
        || (!stanza.to().isEmpty() && stanza.to() != d->peer)) {
        d->fail(ErrProtocol, InvalidFrom, QStringLiteral("Invalid outgoing link-local stanza"));
        return;
    }
    auto outgoing = Stanza::fromXml(stanza.element());
    outgoing.setFrom(d->local);
    outgoing.setTo(d->peer);
    const QPointer<LinkLocalStream> guard(this);
    if (d->send(outgoing.toString().toUtf8()) && guard)
        emit stanzaWritten();
}
void LinkLocalStream::close()
{
    if (d->closed || d->closing)
        return;
    if (!d->headerSent) {
        d->closing = d->remoteClosed = true;
        d->stopTransport();
        return;
    }
    d->closing = true;
    d->deadline.start(5000);
    d->send("</stream:stream>");
}
int                     LinkLocalStream::errorCondition() const { return d->condition; }
QString                 LinkLocalStream::errorText() const { return d->errorText; }
QHash<QString, QString> LinkLocalStream::errorLangText() const { return {}; }
QDomElement             LinkLocalStream::errorAppSpec() const { return {}; }
}
