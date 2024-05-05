/*
 * jignle-ft.h - Jingle file transfer
 * Copyright (C) 2019-2024  Sergey Ilinykh
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

#include "jingle-ft.h"
#include "jingle-nstransportslist.h"
#include "jingle-session.h"

#include "xmpp_client.h"
#include "xmpp_hash.h"
#include "xmpp_thumbs.h"

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QRandomGenerator>
#endif
#include <QDeadlineTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <chrono>
#include <functional>

using namespace std::chrono_literals;

namespace XMPP { namespace Jingle { namespace FileTransfer {

    const QString  NS               = QStringLiteral("urn:xmpp:jingle:apps:file-transfer:5");
    constexpr auto FINALIZE_TIMEOUT = 30s;

    // tags
    static const QString CHECKSUM_TAG = QStringLiteral("checksum");
    static const QString RECEIVED_TAG = QStringLiteral("received");

    class Checksum : public ContentBase {
    public:
        inline Checksum() { }
        Checksum(const QDomElement &file);
        bool        isValid() const;
        QDomElement toXml(QDomDocument *doc) const;

        File file;
    };

    class Received : public ContentBase {
    public:
        using ContentBase::ContentBase;
        QDomElement toXml(QDomDocument *doc) const;
    };

    //----------------------------------------------------------------------------
    // Checksum
    //----------------------------------------------------------------------------
    Checksum::Checksum(const QDomElement &cs) : ContentBase(cs)
    {
        file = File(cs.firstChildElement(QLatin1String("file")));
    }

    bool Checksum::isValid() const { return ContentBase::isValid() && file.isValid(); }

    QDomElement Checksum::toXml(QDomDocument *doc) const
    {
        auto el = ContentBase::toXml(doc, "checksum", NS);
        if (!el.isNull()) {
            el.appendChild(file.toXml(doc));
        }
        return el;
    }

    //----------------------------------------------------------------------------
    // Received
    //----------------------------------------------------------------------------
    QDomElement Received::toXml(QDomDocument *doc) const { return ContentBase::toXml(doc, RECEIVED_TAG, NS); }

    //----------------------------------------------------------------------------
    // ApplicationManager
    //----------------------------------------------------------------------------
    Manager::Manager(QObject *parent) : XMPP::Jingle::ApplicationManager(parent) { }

    Manager::~Manager()
    {
        if (jingleManager)
            jingleManager->unregisterApp(NS);
    }

    void Manager::setJingleManager(XMPP::Jingle::Manager *jm) { jingleManager = jm; }

    Application *Manager::startApplication(const ApplicationManagerPad::Ptr &pad, const QString &contentName,
                                           Origin creator, Origin senders)
    {
        if (!(contentName.size() > 0 && (senders == Origin::Initiator || senders == Origin::Responder))) {
            qDebug("Invalid Jignle FT App start parameters");
            return nullptr;
        }
        return new Application(pad.staticCast<Pad>(), contentName, creator, senders); // ContentOrigin::Remote
    }

    ApplicationManagerPad *Manager::pad(Session *session) { return new Pad(this, session); }

    void Manager::closeAll(const QString &) { }

    QStringList Manager::discoFeatures() const { return { NS }; }

    Client *Manager::client()
    {
        if (jingleManager) {
            return jingleManager->client();
        }
        return nullptr;
    }

    QStringList Manager::availableTransports() const
    {
        return jingleManager->availableTransports(TransportFeature::Reliable | TransportFeature::Ordered
                                                  | TransportFeature::DataOriented);
    }

    //----------------------------------------------------------------------------
    // Application
    //----------------------------------------------------------------------------
    class Application::Private {
    public:
        struct TransportDesc {
            Origin                    creator = Origin::None;
            State                     state   = State::Created;
            QSharedPointer<Transport> transport;
        };

        Application *q = nullptr;

        Reason updateReason;
        // Action              updateToSend            = Action::NoAction;
        bool closeDeviceOnFinish = true;
        bool streamingMode       = false;
        // bool                endlessRange        = false; // where range in accepted file doesn't have end
        bool                   outgoingReceived    = false;
        bool                   writeLoggingStarted = false;
        bool                   readLoggingStarted  = false;
        File                   file;
        File                   acceptFile; // as it comes with "accept" response
        XMPP::Stanza::Error    lastError;
        Reason                 lastReason;
        Connection::Ptr        connection;
        QIODevice             *device = nullptr;
        std::optional<quint64> bytesLeft;
        QList<Hash>            outgoingChecksum;
        QList<Hash>            incomingChecksum;
        QTimer                *finalizeTimer = nullptr;
        FileHasher            *hasher        = nullptr;

        void setState(State s)
        {
            q->_state = s;
            if (s == State::Finished) {
                if (device && closeDeviceOnFinish) {
                    device->close();
                }
                if (connection) {
                    connection->close();
                }
                if (q->transport())
                    q->disconnect(q->transport().data(), &Transport::updated, q, nullptr);
            }
            if (s >= State::Finishing && q->transport()) {
                q->disconnect(q->transport().data(), &Transport::failed, q, nullptr);
                // we can still try to send transport updates
            }
            emit q->stateChanged(s);
        }

        void onReceived()
        {
            lastReason = Reason(Reason::Condition::Success);
            setState(State::Finished);
        }

        void onIncomingChecksum(const QList<Hash> &hashes)
        {
            if (!hasher || q->_senders != q->_pad->session()->peerRole()) {
                qDebug("jignle-ft: unexpected incoming checksum. was it negotiated? %s",
                       qUtf8Printable(q->pad()->session()->peer().full()));
                return;
            }
            incomingChecksum = hashes;
            tryFinalizeIncoming();
        }

        void handleStreamFail(const QString &errorMsg = {})
        {
            lastReason = Reason(Reason::Condition::FailedApplication,
                                errorMsg.isEmpty() ? QString::fromLatin1("stream failed") : errorMsg);
            setState(State::Finished);
        }

        void expectReceived()
        {
            qDebug("jingle-ft: waiting for <received> for %s", qUtf8Printable(q->pad()->session()->peer().full()));
            expectFinalize([this]() {
                qDebug("jingle-ft: Waiting for <received> timed out. But likely succeeded anyway. %s",
                       qUtf8Printable(q->pad()->session()->peer().full()));
                onReceived();
            });
        }

        void expectFinalize(std::function<void()> &&timeoutCallback)
        {
            if (finalizeTimer || q->state() == State::Finished)
                return;
            finalizeTimer = new QTimer(q);
            finalizeTimer->setSingleShot(true);
            finalizeTimer->setInterval(FINALIZE_TIMEOUT);
            q->connect(finalizeTimer, &QTimer::timeout, q, timeoutCallback);
        }

        void setDevice(QIODevice *dev, bool closeOnFinish)
        {
            device              = dev;
            closeDeviceOnFinish = closeOnFinish;
            if (file.hash().isValid() && file.hash().data().isEmpty() && file.range().hashes.isEmpty()) {
                // no precomputated hashes
                hasher = new FileHasher(file.hash().type());
            }
            if (q->senders() == q->pad()->session()->role()) {
                writeNextBlockToTransport();
            } else {
                readNextBlockFromTransport();
            }
        }

        inline std::size_t getBlockSize()
        {
            auto sz = connection->blockSize();
            return sz ? sz : 8192;
        }

        void writeNextBlockToTransport()
        {
            if (bytesLeft && *bytesLeft == 0) {
                if (hasher) {
                    auto hash = hasher->result();
                    if (hash.isValid()) {
                        outgoingChecksum << hash;
                        emit q->updated();
                        return;
                    }
                }
                expectReceived();
                return; // everything is written
            }
            quint64 sz = getBlockSize();
            if (bytesLeft && sz > *bytesLeft) {
                sz = *bytesLeft;
            }
            QByteArray data;
            if (device->isSequential()) {
                sz = qMin(sz, quint64(device->bytesAvailable()));
                if (!sz)
                    return; // we will come back on readyRead
            }
            data.resize(sz);
            sz = device->read(data.data(), sz);
            if (sz == -1) {
                handleStreamFail(QString::fromLatin1("source device failed"));
                return;
            }
            data.resize(sz);
            if (sz == 0) {
                if (!bytesLeft) {
                    lastReason = Reason(Reason::Condition::Success);
                    if (hasher) {
                        auto hash = hasher->result();
                        if (hash.isValid()) {
                            outgoingChecksum << hash;
                            emit q->updated();
                            return;
                        }
                    }
                    setState(State::Finished);
                } else {
                    handleStreamFail();
                }
                return;
            } else if (hasher) {
                hasher->addData(data);
            }

            if (connection->features() & TransportFeature::MessageOriented) {
                if (!connection->writeDatagram(data)) {
                    handleStreamFail();
                    return;
                }
            } else {
                if (connection->write(data) == -1) {
                    handleStreamFail();
                    return;
                }
            }
            emit q->progress(device->pos());
            if (bytesLeft) {
                *bytesLeft -= data.size();
            }
        }

        void readNextBlockFromTransport()
        {
            quint64 bytesAvail;
            while ((!bytesLeft || *bytesLeft > 0)
                   && ((bytesAvail = connection->bytesAvailable()) || (connection->hasPendingDatagrams()))) {
                QByteArray data;
                if (connection->features() & TransportFeature::MessageOriented) {
                    data = connection->readDatagram().data();
                } else {
                    quint64 sz = 65536; // shall we respect transport->blockSize() ?
                    if (bytesLeft && sz > *bytesLeft) {
                        sz = *bytesLeft;
                    }
                    if (sz > bytesAvail) {
                        sz = bytesAvail;
                    }
                    data = connection->read(sz);
                }
                // qDebug("JINGLE-FT read %d bytes from connection", data.size());
                if (data.isEmpty()) {
                    handleStreamFail();
                    return;
                }
                if (hasher) {
                    hasher->addData(data);
                }
                if (device->write(data) == -1) {
                    handleStreamFail();
                    return;
                }
                emit q->progress(device->pos());
                if (bytesLeft) {
                    *bytesLeft -= data.size();
                }
            }
            if (bytesLeft && *bytesLeft == 0) {
                tryFinalizeIncoming();
            }
        }

        bool amISender() const { return q->senders() == q->pad()->session()->role(); }
        bool amIReceiver() const { return q->senders() != q->pad()->session()->role(); }

        void onConnectionConnected(Connection::Ptr newConnection)
        {
            qDebug("jingle-ft: connected. ready to transfer user data with %s",
                   qUtf8Printable(q->pad()->session()->peer().full()));
            connection = newConnection;

            lastReason = Reason();
            lastError.reset();

            if (acceptFile.range().isValid()) {
                if (acceptFile.range().length) {
                    bytesLeft = acceptFile.range().length;
                }
            } else {
                bytesLeft = acceptFile.size();
            }

            if (streamingMode) {
                qDebug("jingle-ft: streaming mode is active for %s",
                       qUtf8Printable(q->pad()->session()->peer().full()));
                if (amIReceiver()) {
                    connection->setReadHook([this](char *buf, qint64 size) {
                        // in streaming mode we need this to compute hash sum and detect stream end is size was defined
                        if (hasher) {
                            hasher->addData(QByteArray::fromRawData(buf, size));
                        }
                        if (bytesLeft) {
                            *bytesLeft -= quint64(size);
                        }
                        if (bytesLeft && *bytesLeft == 0) {
                            tryFinalizeIncoming();
                        }
                    });
                }
                setState(State::Active);
                emit q->connectionReady();
                return;
            }

            connect(connection.data(), &Connection::readyRead, q, [this]() {
                if (!readLoggingStarted) {
                    qDebug("jingle-ft: got first readRead for %s", qUtf8Printable(q->pad()->session()->peer().full()));
                    readLoggingStarted = true;
                }
                if (!device) {
                    return;
                }
                if (q->pad()->session()->role() != q->senders()) {
                    readNextBlockFromTransport();
                }
            });
            connect(
                connection.data(), &Connection::bytesWritten, q,
                [this](qint64 bytes) {
                    if (!writeLoggingStarted) {
                        qDebug("jingle-ft: wrote first %lld bytes for %s.", bytes,
                               qUtf8Printable(q->pad()->session()->peer().full()));
                        writeLoggingStarted = true;
                    }
                    auto bs = getBlockSize();
                    if (q->pad()->session()->role() == q->senders() && connection->bytesToWrite() < bs) {
                        writeNextBlockToTransport();
                    }
                },
                Qt::QueuedConnection);

            if (amIReceiver()) {
                connect(connection.data(), &Connection::disconnected, q, [this]() { tryFinalizeIncoming(); });
            }

            setState(State::Active);
            if (acceptFile.range().isValid()) {
                emit q->deviceRequested(acceptFile.range().offset, bytesLeft);
            } else {
                emit q->deviceRequested(0, bytesLeft);
            }
        }

        void tryFinalizeIncoming()
        {
            auto moreBytesExpected = bytesLeft && *bytesLeft > 0;
            if (q->_state == State::Finished || outgoingReceived || (connection->isOpen() && moreBytesExpected))
                return;

            // data read finished. check other stuff
            if (hasher && incomingChecksum.isEmpty()) {
                qDebug("jignle-ft: waiting for <checksum> with %s", qUtf8Printable(q->pad()->session()->peer().full()));
                expectFinalize([this]() {
                    qDebug("jingle-ft: Waiting for <checksum> timed out. But likely succeeded anyway. %s",
                           qUtf8Printable(q->pad()->session()->peer().full()));
                    lastReason = Reason(Reason::Condition::Success);
                    setState(State::Finished);
                });
                return;
            }
            if (hasher) {
                auto expectedHash = hasher->result();
                bool found        = false;
                for (auto const &h : std::as_const(incomingChecksum)) {
                    if (h.type() != expectedHash.type())
                        continue;
                    if (h == expectedHash) {
                        qDebug("hurray! checksum matched!");
                        lastReason = Reason(Reason::Condition::Success);
                    } else {
                        qDebug("failure! checksum mismatch! expected %s != %s", qPrintable(expectedHash.toString()),
                               qPrintable(h.toString()));
                        q->remove(Reason::Condition::MediaError, "checksum mismatch");
                        return;
                    }
                    found = true;
                    break;
                }
                if (!found)
                    qDebug("jignle-ft: haven't found %s checksum within received checksums with %s",
                           qPrintable(expectedHash.stringType()), qUtf8Printable(q->pad()->session()->peer().full()));
            }
            outgoingReceived = true;
            emit q->updated();
        }

        void prepareThumbnail(File &file)
        {
            if (file.thumbnail().data.size()) {
                auto    client = q->pad()->session()->manager()->client();
                auto    thumb  = file.thumbnail();
                auto    bm     = client->bobManager();
                BoBData data   = bm->append(thumb.data, thumb.mimeType);
                thumb.uri      = QLatin1String("cid:") + data.cid();
                file.setThumbnail(thumb);
            }
        }
    };

    Application::Application(const QSharedPointer<Pad> &pad, const QString &contentName, Origin creator,
                             Origin senders) : d(new Private)
    {
        d->q         = this;
        _pad         = pad;
        _contentName = contentName;
        _creator     = creator;
        _senders     = senders;
        _transportSelector.reset(
            new NSTransportsList(pad->session(), static_cast<Manager *>(pad->manager())->availableTransports()));
    }

    Application::~Application()
    {
        delete d->hasher;
        qDebug("jingle-ft: destroyed for %s", qUtf8Printable(pad()->session()->peer().full()));
    }

    void Application::setState(State state) { d->setState(state); }

    Stanza::Error Application::lastError() const { return d->lastError; }

    Reason Application::lastReason() const { return d->lastReason; }

    static Application::SetDescError parseDescription(const QDomElement &description, File &file)
    {
        auto el = description.firstChildElement("file");
        if (el.isNull())
            return Application::Unparsed;

        auto f = File(el);
        if (!f.isValid())
            return Application::IncompatibleParameters;

        file = f;
        return Application::Ok;
    }

    Application::SetDescError Application::setRemoteOffer(const QDomElement &description)
    {
        File f;
        auto ret = parseDescription(description, f);
        if (ret == Application::Ok)
            d->file = f;
        return ret;
    }

    Application::SetDescError Application::setRemoteAnswer(const QDomElement &description)
    {
        File f;
        auto ret = parseDescription(description, f);
        if (ret == Application::Ok) {
            d->acceptFile = f;
            setState(State::Accepted);
        }
        return ret;
    }

    QDomElement Application::makeLocalOffer()
    {
        if (!d->file.isValid()) {
            return QDomElement();
        }
        auto doc = _pad->doc();
        auto el  = doc->createElementNS(NS, "description");

        d->prepareThumbnail(d->file);
        el.appendChild(d->file.toXml(doc));
        return el;
    }

    QDomElement Application::makeLocalAnswer()
    {
        if (!d->file.isValid()) {
            return QDomElement();
        }
        if (!d->acceptFile.isValid()) {
            d->acceptFile = d->file;
        }
        auto doc = _pad->doc();
        auto el  = doc->createElementNS(NS, "description");
        el.appendChild(d->acceptFile.toXml(doc));
        return el;
    }

    void Application::setFile(const File &file) { d->file = file; }

    void Application::setFile(const QFileInfo &fi, const QString &description, const Thumbnail &thumb)
    {
        QMimeDatabase mimeDb;

        auto hash = Hash::fastestHash(pad()->session()->peerFeatures());
        if (hash.isValid() && fi.size() < 10e6) { // compute hash dynamically (in a thread) for large files
            QFile f(fi.absoluteFilePath());
            f.open(QIODevice::ReadOnly);
            hash.compute(&f);
            f.close();
        }

        File file;
        file.setDate(fi.lastModified());
        file.setDescription(description);
        file.addHash(hash);
        file.setMediaType(mimeDb.mimeTypeForFile(fi).name());
        file.setName(fi.fileName());
        file.setRange(); // indicate range support
        file.setSize(quint64(fi.size()));
        file.setThumbnail(thumb);
        d->file = file;
    }

    File Application::file() const { return d->file; }

    File Application::acceptFile() const { return d->acceptFile; }

    void Application::setAcceptFile(const File &file) const { d->acceptFile = file; }

    bool Application::isTransportReplaceEnabled() const { return _state < State::Active; }

    void Application::prepareTransport()
    {
        expectSingleConnection(TransportFeature::Reliable | TransportFeature::DataOriented | TransportFeature::Ordered,
                               [this](Connection::Ptr connection) { d->onConnectionConnected(connection); });
        _transport->prepare();
    }

    void Application::setStreamingMode(bool mode)
    {
        Q_ASSERT(_senders != _pad->session()->role());
        if (_senders == _pad->session()->role()) {
            qCritical("streaming mode is implemented only for receiving, not sending");
            remove(Reason::GeneralError, "unsupported file sender streaming mode");
            return;
        }
        if (_state <= State::Connecting) {
            d->streamingMode = mode;
        }
    }

    XMPP::Jingle::Application::Update Application::evaluateOutgoingUpdate()
    {
        if (!isValid()) {
            _update = { Action::NoAction, Reason() };
            return _update;
        }

        if (_state == State::Active && (d->outgoingChecksum.size() > 0 || d->outgoingReceived))
            _update = { Action::SessionInfo, Reason() };
        else
            return XMPP::Jingle::Application::evaluateOutgoingUpdate();
        return _update;
    }

    OutgoingUpdate Application::takeOutgoingUpdate()
    {
        qDebug("jingle-ft: take outgoing update for %s", qUtf8Printable(pad()->session()->peer().full()));
        if (_update.action == Action::NoAction) {
            return OutgoingUpdate();
        }

        auto client = _pad->session()->manager()->client();
        auto doc    = client->doc();

        if (_update.action == Action::SessionInfo && (d->outgoingChecksum.size() > 0 || d->outgoingReceived)) {
            if (d->outgoingReceived) {
                d->outgoingReceived = false;
                Received received(creator(), _contentName);
                return OutgoingUpdate { QList<QDomElement>() << received.toXml(doc), [this](bool) {
                                           d->lastReason = Reason(Reason::Condition::Success);
                                           d->setState(State::Finished);
                                       } };
            }
            if (!d->outgoingChecksum.isEmpty()) {
                ContentBase cb(_pad->session()->role(), _contentName);
                File        f;
                if (d->file.range().isValid()) {
                    Range r  = d->file.range();
                    r.hashes = d->outgoingChecksum;
                    f.setRange(r);
                } else {
                    f.setHashes(d->outgoingChecksum);
                }
                auto el = cb.toXml(doc, "checksum", NS);
                el.appendChild(f.toXml(doc));
                d->outgoingChecksum.clear();
                return OutgoingUpdate { QList<QDomElement>() << el, [this](bool) { d->expectReceived(); } };
            }
        }
        if (_update.action == Action::ContentAdd && _creator == _pad->session()->role()) {
            // we are doing outgoing file transfer request. so need thumbnail
        }

        return XMPP::Jingle::Application::takeOutgoingUpdate();
    }

    void Application::prepare()
    {
        if (!_transport) {
            selectNextTransport();
        }
        if (_transport) {
            d->setState(State::ApprovedToSend);
            prepareTransport();
        }
    }

    void Application::start()
    {
        if (_transport) {
            d->setState(State::Connecting);
            _transport->start();
        }
        // TODO we need QIODevice somewhere here
    }

    void Application::remove(Reason::Condition cond, const QString &comment)
    {
        if (_state >= State::Finishing)
            return;

        _terminationReason = Reason(cond, comment);
        if (_transport) {
            _transport->disconnect(this);
            _transport->stop();
        }

        if (_creator == _pad->session()->role() && _state <= State::ApprovedToSend) {
            // local content, not yet sent to remote
            d->lastReason = _terminationReason;
            setState(State::Finished);
            return;
        }

        emit updated();
    }

    void Application::incomingRemove(const Reason &r)
    {
        d->lastReason = r;
        d->setState(State::Finished);
    }

    bool Application::isValid() const
    {
        return d->file.isValid() && _contentName.size() > 0
            && (_senders == Origin::Initiator || _senders == Origin::Responder);
    }

    void Application::setDevice(QIODevice *dev, bool closeOnFinish)
    {
        if (!dev) { // failed to provide proper device
            _terminationReason
                = Reason(Reason::Condition::FailedApplication, QString::fromLatin1("No destination device"));
            emit updated();
            return;
        }
        d->setDevice(dev, closeOnFinish);
    }

    Connection::Ptr Application::connection() const { return d->connection.staticCast<XMPP::Jingle::Connection>(); }

    Pad::Pad(Manager *manager, Session *session) : _manager(manager), _session(session) { }

    QDomElement Pad::takeOutgoingSessionInfoUpdate()
    {
        return QDomElement(); // TODO
    }

    QString Pad::ns() const { return NS; }

    Session *Pad::session() const { return _session; }

    ApplicationManager *Pad::manager() const { return _manager; }

    QString Pad::generateContentName(Origin senders)
    {
        QString prefix = senders == _session->role() ? "fileoffer" : "filereq";
        QString name;
        do {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
            name = prefix + QString("_%1").arg(QRandomGenerator::global()->generate() & 0xffff, 4, 16, QChar('0'));
#else
            name = prefix + QString("_%1").arg(qrand() & 0xffff, 4, 16, QChar('0'));
#endif
        } while (_session->content(name, _session->role()));
        return name;
    }

    bool Pad::incomingSessionInfo(const QDomElement &jingle)
    {
        for (auto el = jingle.firstChildElement(); !el.isNull(); el = el.nextSiblingElement()) {
            if (el.tagName() == CHECKSUM_TAG) {
                Checksum checksum(el);
                auto     app = session()->content(checksum.name, checksum.creator);
                if (app) {
                    qDebug("jignle-ft: got checksum: %s for %s", qPrintable(checksum.file.hashes().value(0).toString()),
                           qUtf8Printable(session()->peer().full()));
                    static_cast<Application *>(app)->d->onIncomingChecksum(checksum.file.hashes());
                }
                return true;
            } else if (el.tagName() == RECEIVED_TAG) {
                Received received(el);
                auto     app = session()->content(received.name, received.creator);
                if (app) {
                    qDebug("jingle-ft: got received for %s", qUtf8Printable(session()->peer().full()));
                    static_cast<Application *>(app)->d->onReceived();
                }
                return true;
            } else {
                // TODO report actual error
                qDebug("jingle-ft: unknown session-info: %s", qPrintable(el.tagName()));
            }
        }
        return false;
    }

    void Pad::addOutgoingOffer(const File &file)
    {
        auto selfp = _session->applicationPad(NS);
        auto app   = _manager->startApplication(selfp, "ft", _session->role(), _session->role());
        app->setFile(file);
    }

} // namespace FileTransfer
} // namespace Jingle
} // namespace XMPP
