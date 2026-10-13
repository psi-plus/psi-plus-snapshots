/*
 * jingle-sxe.cpp - Shared XML Editing (XEP-0284) over Jingle
 * Copyright (C) 2026  Sergey Ilinykh
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

#include "jingle-sxe.h"

#include "jingle-nstransportslist.h"
#include "jingle-session.h"
#include "xmpp_client.h"
#include "xmpp_task.h"

#include <QNetworkDatagram>
#include <QPointer>
#include <QSet>
#include <QTextStream>
#include <QUrl>

namespace XMPP { namespace Jingle { namespace SXE {
    const QString NS_TRANSPORT(QStringLiteral("urn:xmpp:jingle:transports:sxe"));
    const QString NS_SVG_WHITEBOARD(QStringLiteral("urn:xmpp:jingle:apps:svg-whiteboard:0"));
    const QString NS_CONNECTIONS(QStringLiteral("urn:xmpp:sxe:0#connections"));

    namespace {
        const QString NS_SVG(QStringLiteral("http://www.w3.org/2000/svg"));
        const QString NS_HINTS(QStringLiteral("urn:xmpp:hints"));

        // Character budget of edits packed into one <sxe/>. Servers commonly limit stanza size.
        constexpr int MaxSxePayload = 16384;
        // The same on a connection. SCTP messages are limited to 256 KiB (MAX_MESSAGE_SIZE of the
        // association); the budget counts characters, which take up to three bytes in UTF-8.
        constexpr int MaxConnectionPayload = 65536;
        constexpr int MaxConnectionMessage = 262144;

        const TransportFeatures ConnectionFeatures = TransportFeature::MessageOriented | TransportFeature::Reliable
            | TransportFeature::Ordered | TransportFeature::DataOriented;

        // Jingle transports that can carry SXE payload as messages, besides the SXE transport itself.
        QStringList connectionTransports(XMPP::Jingle::Manager *jm)
        {
            if (!jm)
                return {};
            auto list = jm->availableTransports(ConnectionFeatures);
            list.removeAll(NS_TRANSPORT);
            return list;
        }
        // Prologs beyond this size are not stored. They are untrusted and never parsed.
        constexpr int MaxPrologSize = 65536;

        QList<Jid> parseHosts(const QDomElement &transportEl)
        {
            QList<Jid> hosts;
            for (auto host = transportEl.firstChildElement(QStringLiteral("host")); !host.isNull();
                 host      = host.nextSiblingElement(QStringLiteral("host"))) {
                Jid jid(host.text().trimmed());
                if (jid.isValid() && !jid.isEmpty())
                    hosts.append(jid);
            }
            return hosts;
        }

        QString encodeProlog(const QString &prolog)
        {
            return QStringLiteral("data:text/xml,") + QString::fromLatin1(QUrl::toPercentEncoding(prolog));
        }

        QString decodeProlog(const QString &uri)
        {
            if (!uri.startsWith(QLatin1String("data:"), Qt::CaseInsensitive) || uri.size() > MaxPrologSize * 4)
                return {};
            const auto comma = uri.indexOf(QLatin1Char(','));
            if (comma < 0)
                return {};
            const auto meta = uri.mid(5, comma - 5);
            const auto data = uri.mid(comma + 1);
            QString    prolog;
            if (meta.endsWith(QLatin1String(";base64"), Qt::CaseInsensitive))
                prolog = QString::fromUtf8(QByteArray::fromBase64(data.toLatin1()));
            else
                prolog = QUrl::fromPercentEncoding(data.toUtf8());
            return prolog.size() > MaxPrologSize ? QString() : prolog;
        }

        int editSize(const QDomElement &edit)
        {
            int        size  = 32;
            const auto attrs = edit.attributes();
            for (int i = 0; i < attrs.count(); ++i)
                size += int(attrs.item(i).nodeValue().size()) + 16;
            return size;
        }
    } // namespace

    QDomDocument emptyWhiteboardDocument()
    {
        QDomDocument doc;
        auto         svg = doc.createElementNS(NS_SVG, QStringLiteral("svg"));
        svg.setAttribute(QStringLiteral("version"), QStringLiteral("1.2"));
        svg.setAttribute(QStringLiteral("viewBox"), QStringLiteral("0 0 600 400"));
        svg.setAttribute(QStringLiteral("baseProfile"), QStringLiteral("tiny"));
        doc.appendChild(svg);
        return doc;
    }

    //----------------------------------------------------------------------------
    // Transport
    //----------------------------------------------------------------------------
    class Transport::Private {
    public:
        QList<Jid>   hosts;
        bool         pendingUpdate = false;
        QString      encryption;   // agreed protection of the payload
        QDomDocument holdDocument; // copies of payload waiting in the queues

        QDomDocument &document() { return holdDocument; }

        // Protection is asynchronous: keep the order of edits in both directions.
        struct Queue {
            bool                                              busy = false;
            QList<std::function<void(std::function<void()>)>> jobs;

            void run(const std::function<void(std::function<void()>)> &job)
            {
                if (busy) {
                    jobs.append(job);
                    return;
                }
                busy = true;
                job([this]() { next(); });
            }
            void next()
            {
                if (jobs.isEmpty()) {
                    busy = false;
                    return;
                }
                auto job = jobs.takeFirst();
                job([this]() { next(); });
            }
        };
        Queue outgoing;
        Queue incoming;
    };

    PayloadProtector::~PayloadProtector() = default;

    namespace {
        class PreparedSxeUpdate : public XMPP::Jingle::Transport::PreparedUpdate {
        public:
            QList<Jid> hosts;
            QString    encryption;
        };

        PayloadProtector *protectorOf(TransportManagerPad *pad)
        {
            auto manager = pad ? qobject_cast<TransportManager *>(pad->manager()) : nullptr;
            return manager ? manager->payloadProtector() : nullptr;
        }
    }

    Transport::Transport(TransportManagerPad *pad, Origin creator) :
        XMPP::Jingle::Transport(pad, creator), d(std::make_unique<Private>())
    {
    }

    Transport::~Transport() { setParent(nullptr); }

    void Transport::prepare()
    {
        if (isLocal()) {
            d->hosts           = { _pad->session()->manager()->client()->jid() };
            const auto session = _pad->session();
            if (auto protector = protectorOf(_pad);
                protector && session->isEndToEndPreferred() && protector->canProtect(session->peer()))
                d->encryption = protector->method();
        } else if (d->hosts.isEmpty()) {
            onFinish(Reason::FailedTransport, QStringLiteral("SXE transport without a host"));
            return;
        }
        setState(State::ApprovedToSend);
        d->pendingUpdate = true;
        emit updated();
    }

    void Transport::start() { setState(State::Active); }

    Transport::PrepareUpdateResult Transport::prepareUpdate(const QDomElement &transportEl)
    {
        if (_state == State::Finished)
            return { PrepareUpdateStatus::Invalid, {}, {} };
        auto hosts = parseHosts(transportEl);
        if (hosts.isEmpty())
            return { PrepareUpdateStatus::Invalid, {}, {} }; // XEP-0284 requires at least one host
        // Payload protection both sides know; an answer must keep the offered one.
        const auto encryption = transportEl.attribute(QStringLiteral("encryption"));
        auto       protector  = protectorOf(_pad);
        if (!encryption.isEmpty() && (!protector || protector->method() != encryption))
            return { PrepareUpdateStatus::Invalid, {}, {} };
        if (isLocal() && _state >= State::Unacked && encryption != d->encryption)
            return { PrepareUpdateStatus::Invalid, {}, {} };
        auto prepared        = std::make_unique<PreparedSxeUpdate>();
        prepared->hosts      = std::move(hosts);
        prepared->encryption = encryption;
        return { PrepareUpdateStatus::Ready, std::move(prepared), {} };
    }

    bool Transport::commitPreparedUpdate(PreparedUpdatePtr update)
    {
        auto prepared = dynamic_cast<PreparedSxeUpdate *>(update.get());
        if (!prepared || _state == State::Finished)
            return false;
        d->hosts = prepared->hosts;
        if (isRemote())
            d->encryption = prepared->encryption;
        if (_state == State::Created && isRemote())
            setState(State::Pending);
        else if (isLocal() && _state == State::Pending)
            setState(State::Accepted); // the answer of the peer
        return true;
    }

    bool Transport::update(const QDomElement &transportEl)
    {
        auto prepared = prepareUpdate(transportEl);
        return prepared && commitPreparedUpdate(std::move(prepared.update));
    }

    bool Transport::hasUpdates() const { return d->pendingUpdate; }

    OutgoingTransportInfoUpdate Transport::takeOutgoingUpdate(bool ensureTransportElement)
    {
        OutgoingTransportInfoUpdate upd;
        if (!d->pendingUpdate && !ensureTransportElement)
            return upd;

        auto doc = _pad->session()->manager()->client()->doc();
        auto tel = doc->createElementNS(NS_TRANSPORT, QStringLiteral("transport"));
        if (!d->encryption.isEmpty())
            tel.setAttribute(QStringLiteral("encryption"), d->encryption);
        for (const auto &host : std::as_const(d->hosts)) {
            auto el = doc->createElement(QStringLiteral("host"));
            el.appendChild(doc->createTextNode(host.full()));
            tel.appendChild(el);
        }
        if (!d->pendingUpdate) {
            std::get<0>(upd) = tel;
            return upd;
        }

        d->pendingUpdate = false;
        if (_state == State::ApprovedToSend)
            setState(State::Unacked);
        return OutgoingTransportInfoUpdate { tel, [this, guard = QPointer<Transport>(this)](Task *task) {
                                                if (!guard || !task || !task->success() || _state != State::Unacked)
                                                    return;
                                                setState(isLocal() ? State::Pending : State::Accepted);
                                            } };
    }

    bool Transport::isValid() const { return bool(d); }

    TransportFeatures Transport::features() const
    {
        return TransportFeature::Reliable | TransportFeature::Ordered | TransportFeature::MessageOriented;
    }

    int Transport::maxSupportedChannelsPerComponent(TransportFeatures) const { return 0; }

    Connection::Ptr Transport::addChannel(TransportFeatures, const QString &, int) { return {}; }

    QList<Connection::Ptr> Transport::channels() const { return {}; }

    QList<Jid> Transport::hosts() const { return d->hosts; }

    QString Transport::encryption() const { return d->encryption; }

    void Transport::sendSxe(const QDomElement &sxe)
    {
        if (_state == State::Finished)
            return;
        auto session = _pad->session();
        auto client  = session->manager()->client();
        auto doc     = client->doc();
        if (!d->encryption.isEmpty()) {
            auto protector = protectorOf(_pad);
            if (!protector || protector->method() != d->encryption) {
                qWarning("jingle-sxe: dropping payload: its protection is unavailable");
                return;
            }
            // The protected payload travels next to an empty marker that routes the message.
            const auto          copy = d->document().importNode(sxe, true).toElement();
            QPointer<Transport> guard(this);
            d->outgoing.run([guard, protector, copy](std::function<void()> finished) {
                if (!guard) {
                    finished();
                    return;
                }
                const auto peer = guard->_pad->session()->peer();
                protector->protect(peer, copy, [guard, copy, finished](const QDomElement &protectedPayload) {
                    if (guard && guard->_state != State::Finished && !protectedPayload.isNull()) {
                        auto client  = guard->_pad->session()->manager()->client();
                        auto doc     = client->doc();
                        auto message = doc->createElement(QStringLiteral("message"));
                        message.setAttribute(QStringLiteral("to"), guard->_pad->session()->peer().full());
                        message.setAttribute(QStringLiteral("id"), client->genUniqueId());
                        auto marker = doc->createElementNS(NS, QStringLiteral("sxe"));
                        marker.setAttribute(QStringLiteral("session"), copy.attribute(QStringLiteral("session")));
                        message.appendChild(marker);
                        message.appendChild(doc->importNode(protectedPayload, true));
                        message.appendChild(doc->createElementNS(NS_HINTS, QStringLiteral("no-store")));
                        message.appendChild(doc->createElementNS(NS_HINTS, QStringLiteral("no-copy")));
                        client->send(message);
                    } else if (guard && protectedPayload.isNull()) {
                        qWarning("jingle-sxe: payload could not be protected and was not sent");
                    }
                    finished();
                });
            });
            return;
        }
        auto message = doc->createElement(QStringLiteral("message"));
        message.setAttribute(QStringLiteral("to"), session->peer().full());
        message.setAttribute(QStringLiteral("id"), client->genUniqueId());
        message.appendChild(sxe.ownerDocument() == *doc ? sxe : doc->importNode(sxe, true).toElement());
        // The state of a session is useless outside of it: keep it out of archives and offline storage.
        message.appendChild(doc->createElementNS(NS_HINTS, QStringLiteral("no-store")));
        message.appendChild(doc->createElementNS(NS_HINTS, QStringLiteral("no-copy")));
        client->send(message);
    }

    void Transport::receive(const QDomElement &message, const QDomElement &sxe, const Jid &from)
    {
        if (_state == State::Finished)
            return;
        const bool marker = sxe.firstChildElement().isNull();
        if (d->encryption.isEmpty()) {
            if (!marker)
                emit sxeReceived(sxe, from);
            return;
        }
        if (!marker) {
            qWarning("jingle-sxe: dropping unprotected payload of a protected session");
            return;
        }
        auto protector = protectorOf(_pad);
        if (!protector || protector->method() != d->encryption)
            return;
        const auto          copy = d->document().importNode(message, true).toElement();
        const auto          sid  = sxe.attribute(QStringLiteral("session"));
        QPointer<Transport> guard(this);
        d->incoming.run([guard, protector, copy, sid, from](std::function<void()> finished) {
            protector->unprotect(from, copy, [guard, sid, from, finished](const QDomElement &inner) {
                if (guard && guard->_state != State::Finished && !inner.isNull() && inner.namespaceURI() == NS
                    && inner.localName() == QLatin1String("sxe") && inner.attribute(QStringLiteral("session")) == sid)
                    emit guard->sxeReceived(inner, from);
                else if (guard && inner.isNull())
                    qWarning("jingle-sxe: dropping payload that could not be unprotected");
                finished();
            });
        });
    }

    //----------------------------------------------------------------------------
    // TransportPad
    //----------------------------------------------------------------------------
    TransportPad::TransportPad(TransportManager *manager, Session *session) :
        TransportManagerPad(session), _manager(manager), _session(session)
    {
    }

    TransportPad::~TransportPad()
    {
        setParent(nullptr);
        destroyOwnedObjects();
    }

    QString TransportPad::ns() const { return NS_TRANSPORT; }

    Session *TransportPad::session() const { return _session; }

    XMPP::Jingle::TransportManager *TransportPad::manager() const { return _manager; }

    //----------------------------------------------------------------------------
    // TransportManager
    //----------------------------------------------------------------------------
    namespace {
        // Routes received <sxe/> payloads of established Jingle sessions to their transports
        // before the stanza reaches the chat message path.
        class IncomingSxeTask : public Task {
        public:
            IncomingSxeTask(Task *parent, XMPP::Jingle::Manager *jm) : Task(parent), jm(jm) { }

            bool take(const QDomElement &stanza) override
            {
                if (stanza.tagName() != QLatin1String("message") || !jm)
                    return false;
                QDomElement sxe;
                for (auto child = stanza.firstChildElement(QStringLiteral("sxe")); !child.isNull();
                     child      = child.nextSiblingElement(QStringLiteral("sxe"))) {
                    if (child.namespaceURI() == NS) {
                        sxe = child;
                        break;
                    }
                }
                if (sxe.isNull())
                    return false;

                const Jid from(stanza.attribute(QStringLiteral("from")));
                auto      session = jm->session(from, sxe.attribute(QStringLiteral("session")));
                auto      pad     = session ? session->transportPad(NS_TRANSPORT) : nullptr;
                if (!pad)
                    return false; // not a session of ours; let other handlers see it

                if (stanza.attribute(QStringLiteral("type")) == QLatin1String("error")) {
                    qDebug("jingle-sxe: payload bounced for session %s", qPrintable(session->sid()));
                    return true;
                }

                QList<QPointer<Transport>> transports;
                for (auto child : pad->children()) {
                    if (auto t = qobject_cast<Transport *>(child))
                        transports.append(t);
                }
                for (const auto &t : std::as_const(transports)) {
                    if (t && t->state() != State::Finished)
                        t->receive(stanza, sxe, from);
                }
                return true;
            }

        private:
            QPointer<XMPP::Jingle::Manager> jm;
        };
    }

    class TransportManager::Private {
    public:
        XMPP::Jingle::Manager *jingleManager = nullptr;
        QPointer<Task>         incomingTask;
        PayloadProtector      *protector = nullptr;
    };

    TransportManager::TransportManager(QObject *parent) :
        XMPP::Jingle::TransportManager(parent), d(std::make_unique<Private>())
    {
    }

    TransportManager::~TransportManager()
    {
        delete d->incomingTask.data();
        if (d->jingleManager)
            d->jingleManager->unregisterTransport(NS_TRANSPORT);
    }

    TransportFeatures TransportManager::features() const
    {
        return TransportFeature::Reliable | TransportFeature::Ordered | TransportFeature::MessageOriented;
    }

    bool TransportManager::canMakeConnection(TransportFeatures, const QString &)
    {
        return false; // carries SXE payload only, never generic application connections
    }

    void TransportManager::setJingleManager(XMPP::Jingle::Manager *jm)
    {
        delete d->incomingTask.data();
        d->jingleManager = jm;
        if (jm && jm->client())
            d->incomingTask = new IncomingSxeTask(jm->client()->rootTask(), jm);
    }

    QPointer<XMPP::Jingle::Transport> TransportManager::newTransport(TransportManagerPad *pad, Origin creator)
    {
        return new Transport(pad, creator);
    }

    TransportManagerPad *TransportManager::pad(Session *session) { return new TransportPad(this, session); }

    QStringList TransportManager::ns() const { return { NS_TRANSPORT }; }

    QStringList TransportManager::discoFeatures() const
    {
        QStringList features { NS, NS_TRANSPORT };
        if (d->protector)
            features += d->protector->discoFeatures();
        return features;
    }

    TransportManager *TransportManager::of(XMPP::Jingle::Manager *manager)
    {
        return manager ? manager->findChild<TransportManager *>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
    }

    void TransportManager::setPayloadProtector(PayloadProtector *protector) { d->protector = protector; }

    PayloadProtector *TransportManager::payloadProtector() const { return d->protector; }

    //----------------------------------------------------------------------------
    // Pad
    //----------------------------------------------------------------------------
    Pad::Pad(Manager *manager, Session *session) : ApplicationManagerPad(session), _manager(manager), _session(session)
    {
    }

    Pad::~Pad()
    {
        setParent(nullptr);
        destroyOwnedObjects();
    }

    QString Pad::ns() const { return _manager->descriptionNamespace(); }

    Session *Pad::session() const { return _session; }

    XMPP::Jingle::ApplicationManager *Pad::manager() const { return _manager; }

    QString Pad::generateContentName(Origin)
    {
        const QString base = ns() == NS_SVG_WHITEBOARD ? QStringLiteral("whiteboard") : QStringLiteral("sxe");
        QString       name = base;
        // Unique across both creators: BUNDLE groups name contents without their creator.
        for (int i = 2; _session->content(name, Origin::Initiator) || _session->content(name, Origin::Responder); ++i)
            name = QStringLiteral("%1-%2").arg(base).arg(i);
        return name;
    }

    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    class Application::Private {
    public:
        struct Buffered {
            Jid         from;
            QString     id;
            QDomElement state;
        };

        Application *q = nullptr;
        // Non-owning: a consumer may reparent the document to keep it after the session.
        QPointer<Document>                 document;
        std::optional<XMPP::Stanza::Error> lastError;
        Reason                             lastReason;
        SyncState                          sync             = SyncState::Offline;
        bool                               peerSynchronized = false; // host: the joiner has the state
        quint64                            sxeCounter       = 0;
        QSet<QString>                      seenIds;
        QList<Buffered>                    buffered; // joiner: payload received before the state completes
        QMetaObject::Connection            transportConnection;
        QMetaObject::Connection            transportStateConnection;
        bool                               connectionsAllowed = false; // the selector offers connection transports
        Connection::Ptr                    connection;                 // payload channel of a connection transport
        bool                               channelReady = false;       // payload can be exchanged on the transport

        Transport *messageTransport() const { return qobject_cast<Transport *>(q->transport().data()); }

        int payloadBudget() const { return connection ? MaxConnectionPayload : MaxSxePayload; }

        bool sendSxe(const QDomElement &sxe)
        {
            if (connection) {
                QByteArray  bytes;
                QTextStream stream(&bytes);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
                stream.setCodec("UTF-8");
#endif
                sxe.save(stream, -1);
                stream.flush();
                if (bytes.size() > MaxConnectionMessage) {
                    qWarning("jingle-sxe: dropping an <sxe/> of %d bytes", int(bytes.size()));
                    return false;
                }
                return connection->writeDatagram(QNetworkDatagram(bytes));
            }
            if (auto transport = messageTransport()) {
                transport->sendSxe(sxe);
                return true;
            }
            return false;
        }

        void setSync(SyncState state)
        {
            if (sync == state)
                return;
            sync = state;
            emit q->syncStateChanged(state);
        }

        Client *client() const { return q->pad()->session()->manager()->client(); }

        QDomElement newSxe()
        {
            auto sxe = client()->doc()->createElementNS(NS, QStringLiteral("sxe"));
            sxe.setAttribute(QStringLiteral("session"), q->pad()->session()->sid());
            sxe.setAttribute(QStringLiteral("id"), QString::number(++sxeCounter));
            return sxe;
        }

        void sendElement(const QString &tagName, const QDomElement &child = QDomElement())
        {
            if (!channelReady)
                return;
            auto sxe = newSxe();
            auto el  = client()->doc()->createElementNS(NS, tagName);
            if (!child.isNull())
                el.appendChild(child);
            sxe.appendChild(el);
            sendSxe(sxe);
        }

        // Packs edits into as few <sxe/> elements as the payload budget allows.
        void sendState(const QList<QDomElement> &edits, const QDomElement &begin = QDomElement(),
                       const QDomElement &end = QDomElement())
        {
            if (!channelReady)
                return;
            QPointer<Application> guard(q);
            const int             budget = payloadBudget();
            auto                  doc    = client()->doc();
            auto                  sxe    = newSxe();
            auto                  state  = doc->createElementNS(NS, QStringLiteral("state"));
            sxe.appendChild(state);
            int size = 0;
            if (!begin.isNull()) {
                state.appendChild(begin);
                size += editSize(begin);
            }
            for (const auto &edit : edits) {
                const auto esize = editSize(edit);
                if (size > 0 && size + esize > budget) {
                    sendSxe(sxe);
                    if (!guard || !channelReady)
                        return;
                    sxe   = newSxe();
                    state = doc->createElementNS(NS, QStringLiteral("state"));
                    sxe.appendChild(state);
                    size = 0;
                }
                state.appendChild(edit.ownerDocument() == *doc ? edit : doc->importNode(edit, true).toElement());
                size += esize;
            }
            if (!end.isNull())
                state.appendChild(end);
            if (state.hasChildNodes())
                sendSxe(sxe);
        }

        void applySenders()
        {
            if (document)
                document->setReadOnly(!q->mayEdit(q->pad()->session()->role()));
        }

        void onOutgoingEdits(const QList<QDomElement> &edits)
        {
            // A host includes edits made before the peer joined in the state snapshot,
            // and a joiner can't edit before it has the state.
            const bool canSend = q->isHost() ? peerSynchronized : sync == SyncState::Synchronized;
            if (!canSend || q->state() >= State::Finishing)
                return;
            sendState(edits);
        }

        void sendSnapshot()
        {
            if (!document)
                return;
            auto doc   = client()->doc();
            auto begin = doc->createElementNS(NS, QStringLiteral("document-begin"));
            begin.setAttribute(QStringLiteral("prolog"), encodeProlog(document->prolog()));
            auto end = doc->createElementNS(NS, QStringLiteral("document-end"));
            // Edits are never sent to a joiner before its snapshot, so none must be skipped.
            end.setAttribute(QStringLiteral("last-sender"), QString());
            end.setAttribute(QStringLiteral("last-id"), QString());
            // The snapshot and the end marker are sent without interleaving any other <sxe/>.
            sendState(document->snapshot(*doc), begin, end);
        }

        void onSxe(const QDomElement &sxe, const Jid &from)
        {
            const auto id = sxe.attribute(QStringLiteral("id"));
            if (!id.isEmpty()) {
                const auto key = from.full() + QLatin1Char('\n') + id;
                if (seenIds.contains(key))
                    return;
                seenIds.insert(key);
            }

            QPointer<Application> guard(q);
            for (auto el = sxe.firstChildElement(); !el.isNull() && guard; el = el.nextSiblingElement()) {
                if (el.namespaceURI() != NS)
                    continue;
                const auto tag = el.tagName();
                if (tag == QLatin1String("connect")) {
                    if (q->isHost()) {
                        peerSynchronized = false;
                        sendElement(QStringLiteral("state-offer"), q->makeLocalOffer());
                    }
                } else if (tag == QLatin1String("state-offer")) {
                    if (q->isHost())
                        continue;
                    const auto description = el.firstChildElement(QStringLiteral("description"));
                    if (sync == SyncState::Connecting && description.namespaceURI() == q->pad()->ns()) {
                        buffered.clear();
                        setSync(SyncState::Receiving);
                        sendElement(QStringLiteral("accept-state"));
                    } else {
                        sendElement(QStringLiteral("refuse-state"));
                    }
                } else if (tag == QLatin1String("accept-state")) {
                    if (q->isHost()) {
                        sendSnapshot();
                        peerSynchronized = true;
                    }
                } else if (tag == QLatin1String("refuse-state")) {
                    if (q->isHost())
                        q->remove(Reason::FailedApplication, QStringLiteral("the peer refused the shared state"));
                } else if (tag == QLatin1String("state")) {
                    onState(el, from, id);
                }
            }
        }

        void onState(const QDomElement &state, const Jid &from, const QString &id)
        {
            if (!document)
                return;
            // Edits of a participant outside the senders are ignored; the state transfer is not an edit.
            const bool peerMayEdit = q->mayEdit(q->pad()->session()->peerRole());
            if (q->isHost()) {
                if (peerSynchronized && peerMayEdit)
                    document->applyEdits(state);
                return;
            }

            if (sync == SyncState::Synchronized) {
                if (peerMayEdit)
                    document->applyEdits(state);
                return;
            }
            if (sync != SyncState::Receiving)
                return;

            const auto begin = state.firstChildElement(QStringLiteral("document-begin"));
            if (!document->isImporting()) {
                if (begin.isNull()) {
                    buffered.append({ from, id, state.cloneNode(true).toElement() });
                    return;
                }
                document->beginImport(decodeProlog(begin.attribute(QStringLiteral("prolog"))));
            }

            QPointer<Application> guard(q);
            document->applyEdits(state);
            if (!guard)
                return;
            const auto end = state.lastChildElement(QStringLiteral("document-end"));
            if (end.isNull())
                return;

            document->endImport();
            if (!guard)
                return;
            // Payload already included in the snapshot is identified by last-sender/last-id.
            const Jid  lastSender(end.attribute(QStringLiteral("last-sender")));
            const auto lastId  = end.attribute(QStringLiteral("last-id"));
            auto       pending = std::move(buffered);
            buffered.clear();
            if (!lastId.isEmpty()) {
                for (int i = 0; i < pending.size(); ++i) {
                    if (pending[i].id == lastId && pending[i].from.compare(lastSender)) {
                        pending.erase(pending.begin(), pending.begin() + i + 1);
                        break;
                    }
                }
            }
            setSync(SyncState::Synchronized);
            for (const auto &entry : std::as_const(pending)) {
                if (!guard || !document)
                    return;
                document->applyEdits(entry.state);
            }
        }

        void attachTransport()
        {
            QObject::disconnect(transportConnection);
            QObject::disconnect(transportStateConnection);
            auto transport = messageTransport();
            if (!transport)
                return;
            transportConnection
                = QObject::connect(transport, &Transport::sxeReceived, q,
                                   [this](const QDomElement &sxe, const Jid &from) { onSxe(sxe, from); });
            // A message transport that replaced a connection is ready when it starts.
            transportStateConnection = QObject::connect(transport, &XMPP::Jingle::Transport::stateChanged, q, [this]() {
                auto transport = messageTransport();
                if (transport && transport->state() == State::Active && !channelReady && q->state() >= State::Connecting
                    && q->state() < State::Finishing)
                    onChannelReady();
            });
        }

        void detachConnection()
        {
            channelReady = false;
            if (!connection)
                return;
            connection->disconnect(q);
            connection.reset();
        }

        void setConnection(Connection::Ptr channel)
        {
            detachConnection();
            connection = std::move(channel);
            QObject::connect(connection.data(), &Connection::readyRead, q, [this]() { readDatagrams(); });
            QObject::connect(connection.data(), &ByteStream::connectionClosed, q, [this]() { onConnectionLost(); });
            QObject::connect(connection.data(), &Connection::disconnected, q, [this]() { onConnectionLost(); });
            QObject::connect(connection.data(), &ByteStream::error, q, [this](int) { onConnectionLost(); });
            QPointer<Application> guard(q);
            onChannelReady();
            if (guard && connection && connection->hasPendingDatagrams())
                readDatagrams();
        }

        void readDatagrams()
        {
            QPointer<Application> guard(q);
            const auto            peer = q->pad()->session()->peer();
            const auto            sid  = q->pad()->session()->sid();
            while (guard && connection && connection->hasPendingDatagrams()) {
                const auto   datagram = connection->readDatagram();
                QDomDocument doc;
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
                const bool parsed = doc.setContent(datagram.data(), true);
#else
                const bool parsed
                    = bool(doc.setContent(datagram.data(), QDomDocument::ParseOption::UseNamespaceProcessing));
#endif
                const auto sxe = doc.documentElement();
                if (!parsed || sxe.namespaceURI() != NS || sxe.localName() != QLatin1String("sxe")
                    || sxe.attribute(QStringLiteral("session")) != sid) {
                    qWarning("jingle-sxe: ignoring an invalid message on the connection");
                    continue;
                }
                onSxe(sxe, peer);
            }
        }

        // The connection broke: continue on the next transport, ending with SXE messages.
        void onConnectionLost()
        {
            if (!connection || q->state() >= State::Finishing)
                return;
            qDebug("jingle-sxe: connection lost, replacing the transport");
            detachConnection();
            q->selectNextTransport();
        }

        // The payload channel of the current transport is ready. On the first channel the content becomes
        // active; on a replacement the joiner synchronizes again, since edits in flight may have been lost.
        void onChannelReady()
        {
            QPointer<Application> guard(q);
            channelReady = true;
            if (q->state() < State::Active) {
                q->setState(State::Active);
                if (!guard || q->state() != State::Active)
                    return;
            }
            if (q->isHost()) {
                // Edits made until the joiner has the state again go with its snapshot.
                peerSynchronized = false;
                setSync(SyncState::Synchronized);
            } else {
                buffered.clear();
                setSync(SyncState::Connecting);
                if (guard)
                    sendElement(QStringLiteral("connect"));
            }
        }
    };

    Application::Application(Pad *pad, const QString &contentName, Origin creator, Origin senders) :
        d(std::make_unique<Private>())
    {
        d->q = this;
        setParent(pad);
        _pad         = pad;
        _contentName = contentName;
        _creator     = creator;
        _senders     = senders;
        // Connections are offered only to a peer that advertises SXE payload on them; an offer from the
        // peer is accepted when a local transport can make such connections.
        QStringList transports { NS_TRANSPORT };
        const auto  session = pad->session();
        if (pad->session()->role() != creator || session->peerFeatures().test(NS_CONNECTIONS))
            transports += connectionTransports(session->manager());
        _transportSelector.reset(new NSTransportsList(session, transports));
        d->connectionsAllowed = transports.size() > 1;

        d->document = new Document(this);
        connect(d->document, &Document::outgoingEdits, this,
                [this](const QList<QDomElement> &edits) { d->onOutgoingEdits(edits); });
        if (isHost())
            d->document->reset(pad->ns() == NS_SVG_WHITEBOARD ? emptyWhiteboardDocument() : QDomDocument());
        connect(this, &XMPP::Jingle::Application::sendersChanged, this, [this]() { d->applySenders(); });
        d->applySenders();
    }

    Application::~Application() { setParent(nullptr); }

    Document *Application::document() const { return d->document; }

    void Application::setInitialDocument(const QDomDocument &doc)
    {
        if (d->document && isHost() && !d->peerSynchronized)
            d->document->reset(doc);
    }

    bool Application::isHost() const { return isLocal(); }

    Application::SyncState Application::syncState() const { return d->sync; }

    bool Application::mayEdit(Origin role) const { return _senders == Origin::Both || _senders == role; }

    void Application::setState(State state)
    {
        _state = state;
        if (state >= State::Finishing) {
            if (d->connection) {
                // The application no longer owns the channel; the transport drains and closes it.
                auto closing = d->connection;
                d->detachConnection();
                closing->close();
            }
            d->setSync(SyncState::Offline);
        }
        emit stateChanged(state);
    }

    const std::optional<XMPP::Stanza::Error> &Application::lastError() const { return d->lastError; }

    Reason Application::lastReason() const { return d->lastReason; }

    // A connection transport can be replaced, finally by the SXE transport. That one is not replaced once it
    // runs; until then it is only a proposal, which a simultaneous replacement by the peer may win.
    bool Application::allowsSharedTransport() const { return d->connectionsAllowed; }

    bool Application::isTransportReplaceEnabled() const
    {
        return !_transport || !d->messageTransport() || _transport->state() < State::Active;
    }

    void Application::remove(Reason::Condition cond, const QString &comment)
    {
        if (_state == State::Finished || _terminationReason.isValid())
            return;
        _terminationReason = Reason(cond, comment);
        d->lastReason      = _terminationReason;

        QPointer<Application> guard(this);
        d->detachConnection();
        if (_transport) {
            _transport->disconnect(this);
            _transport->stop();
            if (!guard)
                return;
        }
        if (isLocal() && _state <= State::ApprovedToSend) {
            setState(State::Finished); // never sent to the peer
            return;
        }
        emit updated();
    }

    Application::SetDescError Application::setRemoteOffer(const QDomElement &description)
    {
        if (description.namespaceURI() != _pad->ns())
            return IncompatibleParameters;
        // <sxe/> payload is addressed by the Jingle sid, so a session carries one SXE content.
        for (auto content : _pad->session()->contentList()) {
            auto other = qobject_cast<Application *>(content);
            if (other && other != this && other->state() < State::Finishing)
                return IncompatibleParameters;
        }
        return Ok;
    }

    Application::SetDescError Application::setRemoteAnswer(const QDomElement &description)
    {
        if (description.namespaceURI() != _pad->ns())
            return IncompatibleParameters;
        setState(State::Accepted);
        return Ok;
    }

    QDomElement Application::makeLocalOffer() { return _pad->doc()->createElementNS(_pad->ns(), "description"); }

    QDomElement Application::makeLocalAnswer() { return makeLocalOffer(); }

    void Application::prepare()
    {
        if (!_transport && _transportSelector->hasMoreTransports()) {
            // The most preferred transport the peer supports; the SXE transport is the fallback below.
            auto transport = _transportSelector->getNextTransport();
            if (transport && !setTransport(transport))
                transport->releaseIfUnused();
        }
        if (!_transport)
            setTransport(_pad->session()->newOutgoingTransport(NS_TRANSPORT));
        if (!_transport) {
            remove(Reason::FailedTransport, QStringLiteral("no SXE transport"));
            return;
        }
        setState(State::ApprovedToSend);
        prepareTransport();
    }

    void Application::prepareTransport()
    {
        d->detachConnection();
        d->attachTransport();
        if (d->messageTransport()) {
            _transport->prepare();
            return;
        }
        QPointer<Application> guard(this);
        const auto            expected = _transport;
        expectSingleConnection(ConnectionFeatures,
                               [this](Connection::Ptr connection) { d->setConnection(connection); });
        if (guard && expected && _transport == expected && _state < State::Finishing)
            expected->prepare();
    }

    void Application::start()
    {
        if (!_transport || _state >= State::Finishing)
            return;
        d->attachTransport();
        QPointer<Application> guard(this);
        if (!d->messageTransport()) {
            // Active once the connection is ready.
            setState(State::Connecting);
            if (guard && _transport)
                _transport->start();
            return;
        }
        _transport->start();
        if (guard && !d->channelReady)
            d->onChannelReady();
    }

    void Application::incomingRemove(const Reason &r)
    {
        d->lastReason = r;
        setState(State::Finished);
    }

    //----------------------------------------------------------------------------
    // Manager
    //----------------------------------------------------------------------------
    Manager::Manager(const QString &descriptionNs, QObject *parent) :
        XMPP::Jingle::ApplicationManager(parent), descriptionNs_(descriptionNs)
    {
    }

    Manager::~Manager()
    {
        if (jingleManager_)
            jingleManager_->unregisterApp(descriptionNs_);
    }

    void Manager::setJingleManager(XMPP::Jingle::Manager *jm) { jingleManager_ = jm; }

    Application *Manager::startApplication(ApplicationManagerPad *pad, const QString &contentName, Origin creator,
                                           Origin senders)
    {
        if (contentName.isEmpty() || (creator != Origin::Initiator && creator != Origin::Responder))
            return nullptr;
        return new Application(static_cast<Pad *>(pad), contentName, creator, senders);
    }

    ApplicationManagerPad *Manager::pad(Session *session) { return new Pad(this, session); }

    void Manager::closeAll(const QString &) { }

    QStringList Manager::ns() const { return { descriptionNs_ }; }

    QStringList Manager::discoFeatures() const
    {
        QStringList features { descriptionNs_ };
        if (!connectionTransports(jingleManager_).isEmpty())
            features += NS_CONNECTIONS;
        return features;
    }

    QString Manager::descriptionNamespace() const { return descriptionNs_; }

    std::optional<std::any> Manager::parseProposal(const QDomElement &description) const
    {
        if (description.tagName() != QLatin1String("description") || description.namespaceURI() != descriptionNs_)
            return std::nullopt;
        return std::any(Proposal {});
    }

    QDomElement Manager::serializeProposal(const std::any &data, QDomDocument *document) const
    {
        if (!document || data.type() != typeid(Proposal))
            return {};
        return document->createElementNS(descriptionNs_, QStringLiteral("description"));
    }

}}}
