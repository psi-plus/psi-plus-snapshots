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

#include <QPointer>
#include <QSet>
#include <QUrl>

namespace XMPP { namespace Jingle { namespace SXE {
    const QString NS_TRANSPORT(QStringLiteral("urn:xmpp:jingle:transports:sxe"));
    const QString NS_SVG_WHITEBOARD(QStringLiteral("urn:xmpp:jingle:apps:svg-whiteboard:0"));

    namespace {
        const QString NS_SVG(QStringLiteral("http://www.w3.org/2000/svg"));
        const QString NS_HINTS(QStringLiteral("urn:xmpp:hints"));

        // Character budget of edits packed into one <sxe/>. Servers commonly limit stanza size.
        constexpr int MaxSxePayload = 16384;
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
        QList<Jid> hosts;
        bool       pendingUpdate = false;
    };

    namespace {
        class PreparedSxeUpdate : public XMPP::Jingle::Transport::PreparedUpdate {
        public:
            QList<Jid> hosts;
        };
    }

    Transport::Transport(TransportManagerPad *pad, Origin creator) :
        XMPP::Jingle::Transport(pad, creator), d(std::make_unique<Private>())
    {
    }

    Transport::~Transport() { setParent(nullptr); }

    void Transport::prepare()
    {
        if (isLocal())
            d->hosts = { _pad->session()->manager()->client()->jid() };
        else if (d->hosts.isEmpty()) {
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
        auto prepared   = std::make_unique<PreparedSxeUpdate>();
        prepared->hosts = std::move(hosts);
        return { PrepareUpdateStatus::Ready, std::move(prepared), {} };
    }

    bool Transport::commitPreparedUpdate(PreparedUpdatePtr update)
    {
        auto prepared = dynamic_cast<PreparedSxeUpdate *>(update.get());
        if (!prepared || _state == State::Finished)
            return false;
        d->hosts = prepared->hosts;
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

    void Transport::sendSxe(const QDomElement &sxe)
    {
        if (_state == State::Finished)
            return;
        auto session = _pad->session();
        auto client  = session->manager()->client();
        auto doc     = client->doc();
        auto message = doc->createElement(QStringLiteral("message"));
        message.setAttribute(QStringLiteral("to"), session->peer().full());
        message.setAttribute(QStringLiteral("id"), client->genUniqueId());
        message.appendChild(sxe.ownerDocument() == *doc ? sxe : doc->importNode(sxe, true).toElement());
        // The state of a session is useless outside of it: keep it out of archives and offline storage.
        message.appendChild(doc->createElementNS(NS_HINTS, QStringLiteral("no-store")));
        message.appendChild(doc->createElementNS(NS_HINTS, QStringLiteral("no-copy")));
        client->send(message);
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
                        emit t->sxeReceived(sxe, from);
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

    QStringList TransportManager::discoFeatures() const { return { NS, NS_TRANSPORT }; }

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
        for (int i = 2; _session->content(name, _session->role()); ++i)
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
            auto transport = qobject_cast<Transport *>(q->transport().data());
            if (!transport)
                return;
            auto sxe = newSxe();
            auto el  = client()->doc()->createElementNS(NS, tagName);
            if (!child.isNull())
                el.appendChild(child);
            sxe.appendChild(el);
            transport->sendSxe(sxe);
        }

        // Packs edits into as few <sxe/> elements as the payload budget allows.
        void sendState(const QList<QDomElement> &edits, const QDomElement &begin = QDomElement(),
                       const QDomElement &end = QDomElement())
        {
            QPointer<Transport> transport = qobject_cast<Transport *>(q->transport().data());
            if (!transport)
                return;
            auto doc   = client()->doc();
            auto sxe   = newSxe();
            auto state = doc->createElementNS(NS, QStringLiteral("state"));
            sxe.appendChild(state);
            int size = 0;
            if (!begin.isNull()) {
                state.appendChild(begin);
                size += editSize(begin);
            }
            for (const auto &edit : edits) {
                const auto esize = editSize(edit);
                if (size > 0 && size + esize > MaxSxePayload) {
                    transport->sendSxe(sxe);
                    if (!transport)
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
                transport->sendSxe(sxe);
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
            if (q->isHost()) {
                if (peerSynchronized)
                    document->applyEdits(state);
                return;
            }

            if (sync == SyncState::Synchronized) {
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
            auto transport = qobject_cast<Transport *>(q->transport().data());
            if (transport)
                transportConnection
                    = QObject::connect(transport, &Transport::sxeReceived, q,
                                       [this](const QDomElement &sxe, const Jid &from) { onSxe(sxe, from); });
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
        _transportSelector.reset(new NSTransportsList(pad->session(), { NS_TRANSPORT }));

        d->document = new Document(this);
        connect(d->document, &Document::outgoingEdits, this,
                [this](const QList<QDomElement> &edits) { d->onOutgoingEdits(edits); });
        if (isHost())
            d->document->reset(pad->ns() == NS_SVG_WHITEBOARD ? emptyWhiteboardDocument() : QDomDocument());
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

    void Application::setState(State state)
    {
        _state = state;
        if (state >= State::Finishing)
            d->setSync(SyncState::Offline);
        emit stateChanged(state);
    }

    const std::optional<XMPP::Stanza::Error> &Application::lastError() const { return d->lastError; }

    Reason Application::lastReason() const { return d->lastReason; }

    // The SXE transport is the only one: it can be installed, but never replaced.
    bool Application::isTransportReplaceEnabled() const { return !_transport; }

    void Application::remove(Reason::Condition cond, const QString &comment)
    {
        if (_state == State::Finished || _terminationReason.isValid())
            return;
        _terminationReason = Reason(cond, comment);
        d->lastReason      = _terminationReason;

        QPointer<Application> guard(this);
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
        return description.namespaceURI() == _pad->ns() ? Ok : IncompatibleParameters;
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
        d->attachTransport();
        _transport->prepare();
    }

    void Application::start()
    {
        if (!_transport || _state >= State::Finishing)
            return;
        d->attachTransport();
        QPointer<Application> guard(this);
        _transport->start();
        if (!guard)
            return;
        setState(State::Active);
        if (!guard || _state != State::Active)
            return;
        if (isHost()) {
            d->setSync(SyncState::Synchronized);
        } else {
            d->setSync(SyncState::Connecting);
            d->sendElement(QStringLiteral("connect"));
        }
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

    QStringList Manager::discoFeatures() const { return { descriptionNs_ }; }

    QString Manager::descriptionNamespace() const { return descriptionNs_; }

}}}
