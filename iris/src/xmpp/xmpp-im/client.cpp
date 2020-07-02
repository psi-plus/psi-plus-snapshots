/*
 * client.cpp - IM Client
 * Copyright (C) 2003  Justin Karneges
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

//! \class XMPP::Client client.h
//! \brief Communicates with the XMPP network.  Start here.
//!
//!  Client controls an active XMPP connection.  It allows you to connect,
//!  authenticate, manipulate the roster, and send / receive messages and
//!  presence.  It is the centerpiece of this library, and all Tasks must pass
//!  through it.
//!
//!  For convenience, many Tasks are handled internally to Client (such as
//!  JT_Auth).  However, for accessing features beyond the basics provided by
//!  Client, you will need to manually invoke Tasks.  Fortunately, the
//!  process is very simple.
//!
//!  The entire Task system is heavily founded on Qt.  All Tasks have a parent,
//!  except for the root Task, and are considered QObjects.  By using Qt's RTTI
//!  facilities (QObject::sender(), QObject::isA(), etc), you can use a
//!  "fire and forget" approach with Tasks.
//!
//!  \code
//!  #include "client.h"
//!  using namespace XMPP;
//!
//!  ...
//!
//!  Client *client;
//!
//!  Session::Session()
//!  {
//!    client = new Client;
//!    connect(client, SIGNAL(handshaken()), SLOT(clientHandshaken()));
//!    connect(client, SIGNAL(authFinished(bool,int,QString)), SLOT(authFinished(bool,int,QString)));
//!    client->connectToHost("jabber.org");
//!  }
//!
//!  void Session::clientHandshaken()
//!  {
//!    client->authDigest("jabtest", "12345", "Psi");
//!  }
//!
//!  void Session::authFinished(bool success, int, const QString &err)
//!  {
//!    if(success)
//!      printf("Login success!");
//!    else
//!      printf("Login failed.  Here's why: %s\n", err.toLatin1());
//!  }
//!  \endcode

#include "filetransfer.h"
#include "httpfileupload.h"
#include "im.h"
#include "jingle-ft.h"
#include "jingle-ibb.h"
#include "jingle-ice.h"
#include "jingle-s5b.h"
#include "jingle.h"
#include "protocol.h"
#include "s5b.h"
#include "tcpportreserver.h"
#include "xmpp_bitsofbinary.h"
#include "xmpp_caps.h"
#include "xmpp_hash.h"
#include "xmpp_ibb.h"
#include "xmpp_serverinfomanager.h"
#include "xmpp_tasks.h"
#include "xmpp_xmlcommon.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QTimer>

#ifdef Q_OS_WIN
#define vsnprintf _vsnprintf
#endif

#define GROUPS_DELIMITER_TIMEOUT 10

namespace XMPP {
//----------------------------------------------------------------------------
// Client
//----------------------------------------------------------------------------
class Client::GroupChat {
public:
    enum { Connecting, Connected, Closing };
    GroupChat() = default;

    Jid     j;
    int     status = 0;
    QString password;
};

class Client::ClientPrivate {
public:
    ClientPrivate() { }

    QPointer<ClientStream>  stream;
    QDomDocument            doc;
    int                     id_seed = 0xaaaa;
    Task *                  root    = nullptr;
    QNetworkAccessManager * qnam    = nullptr;
    QString                 host, user, pass, resource;
    QString                 osName, osVersion, tzname, clientName, clientVersion;
    CapsSpec                caps, serverCaps;
    DiscoItem::Identity     identity;
    Features                features;
    QMap<QString, Features> extension_features;
    int                     tzoffset         = 0;
    bool                    useTzoffset      = false; // manual tzoffset is old way of doing utc<->local translations
    bool                    active           = false;
    bool                    capsOptimization = false; // don't send caps every time

    LiveRoster             roster;
    ResourceList           resourceList;
    CapsManager *          capsman               = nullptr;
    TcpPortReserver *      tcpPortReserver       = nullptr;
    S5BManager *           s5bman                = nullptr;
    Jingle::S5B::Manager * jingleS5BManager      = nullptr;
    Jingle::IBB::Manager * jingleIBBManager      = nullptr;
    Jingle::ICE::Manager * jingleICEManager      = nullptr;
    IBBManager *           ibbman                = nullptr;
    BoBManager *           bobman                = nullptr;
    FileTransferManager *  ftman                 = nullptr;
    ServerInfoManager *    serverInfoManager     = nullptr;
    HttpFileUploadManager *httpFileUploadManager = nullptr;
    Jingle::Manager *      jingleManager         = nullptr;
    QList<GroupChat>       groupChatList;
    EncryptionHandler *    encryptionHandler = nullptr;
};

Client::Client(QObject *par) : QObject(par)
{
    d                = new ClientPrivate;
    d->active        = false;
    d->osName        = "N/A";
    d->clientName    = "N/A";
    d->clientVersion = "0.0";

    d->root = new Task(this, true);

    d->s5bman = new S5BManager(this);
    connect(d->s5bman, SIGNAL(incomingReady()), SLOT(s5b_incomingReady()));

    d->ibbman = new IBBManager(this);
    connect(d->ibbman, SIGNAL(incomingReady()), SLOT(ibb_incomingReady()));

    d->bobman = new BoBManager(this);

    d->ftman = nullptr;

    d->capsman = new CapsManager(this);

    d->serverInfoManager     = new ServerInfoManager(this);
    d->httpFileUploadManager = new HttpFileUploadManager(this);

    d->jingleManager = new Jingle::Manager(this);
    auto ft          = new Jingle::FileTransfer::Manager(this);
    d->jingleManager->registerApp(Jingle::FileTransfer::NS, ft);
    d->jingleS5BManager = new Jingle::S5B::Manager(d->jingleManager);
    d->jingleIBBManager = new Jingle::IBB::Manager(d->jingleManager);
    d->jingleICEManager = new Jingle::ICE::Manager(d->jingleManager);
    d->jingleManager->registerTransport(Jingle::S5B::NS, d->jingleS5BManager);
    d->jingleManager->registerTransport(Jingle::IBB::NS, d->jingleIBBManager);
    d->jingleManager->registerTransport(Jingle::ICE::NS, d->jingleICEManager);
}

Client::~Client()
{
    // fprintf(stderr, "\tClient::~Client\n");
    // fflush(stderr);

    close(true);

    delete d->ftman;
    delete d->ibbman;
    delete d->s5bman;
    delete d->jingleManager;
    delete d->root;
    delete d;
    // fprintf(stderr, "\tClient::~Client\n");
}

void Client::connectToServer(ClientStream *s, const Jid &j, bool auth)
{
    d->stream = s;
    // connect(d->stream, SIGNAL(connected()), SLOT(streamConnected()));
    // connect(d->stream, SIGNAL(handshaken()), SLOT(streamHandshaken()));
    connect(d->stream, SIGNAL(error(int)), SLOT(streamError(int)));
    // connect(d->stream, SIGNAL(sslCertificateReady(QSSLCert)), SLOT(streamSSLCertificateReady(QSSLCert)));
    connect(d->stream, SIGNAL(readyRead()), SLOT(streamReadyRead()));
    // connect(d->stream, SIGNAL(closeFinished()), SLOT(streamCloseFinished()));
    connect(d->stream, SIGNAL(incomingXml(QString)), SLOT(streamIncomingXml(QString)));
    connect(d->stream, SIGNAL(outgoingXml(QString)), SLOT(streamOutgoingXml(QString)));
    connect(d->stream, SIGNAL(haveUnhandledFeatures()), SLOT(parseUnhandledStreamFeatures()));

    d->stream->connectToServer(j, auth);
}

void Client::start(const QString &host, const QString &user, const QString &pass, const QString &_resource)
{
    // TODO
    d->host     = host;
    d->user     = user;
    d->pass     = pass;
    d->resource = _resource;

    Status stat;
    stat.setIsAvailable(false);
    d->resourceList += Resource(resource(), stat);

    JT_PushPresence *pp = new JT_PushPresence(rootTask());
    connect(pp, SIGNAL(subscription(Jid, QString, QString)), SLOT(ppSubscription(Jid, QString, QString)));
    connect(pp, SIGNAL(presence(Jid, Status)), SLOT(ppPresence(Jid, Status)));

    JT_PushMessage *pm = new JT_PushMessage(rootTask(), d->encryptionHandler);
    connect(pm, SIGNAL(message(Message)), SLOT(pmMessage(Message)));

    JT_PushRoster *pr = new JT_PushRoster(rootTask());
    connect(pr, SIGNAL(roster(Roster)), SLOT(prRoster(Roster)));

    new JT_ServInfo(rootTask());
    new JT_PongServer(rootTask());

    d->active = true;
}

void Client::setTcpPortReserver(TcpPortReserver *portReserver) { d->tcpPortReserver = portReserver; }

TcpPortReserver *Client::tcpPortReserver() const { return d->tcpPortReserver; }

void Client::setFileTransferEnabled(bool b)
{
    if (b) {
        if (!d->ftman)
            d->ftman = new FileTransferManager(this);
    } else {
        if (d->ftman) {
            delete d->ftman;
            d->ftman = nullptr;
        }
    }
}

FileTransferManager *Client::fileTransferManager() const { return d->ftman; }

S5BManager *Client::s5bManager() const { return d->s5bman; }

Jingle::S5B::Manager *Client::jingleS5BManager() const { return d->jingleS5BManager; }

Jingle::IBB::Manager *Client::jingleIBBManager() const { return d->jingleIBBManager; }

Jingle::ICE::Manager *Client::jingleICEManager() const { return d->jingleICEManager; }

IBBManager *Client::ibbManager() const { return d->ibbman; }

BoBManager *Client::bobManager() const { return d->bobman; }

CapsManager *Client::capsManager() const { return d->capsman; }

void Client::setCapsOptimizationAllowed(bool allowed) { d->capsOptimization = allowed; }

bool Client::capsOptimizationAllowed() const
{
    if (d->capsOptimization && d->active && d->serverInfoManager->features().hasCapsOptimize()) {
        auto it = d->resourceList.find(d->resource);
        return it != d->resourceList.end() && it->status().isAvailable();
    }
    return false;
}

ServerInfoManager *Client::serverInfoManager() const { return d->serverInfoManager; }

HttpFileUploadManager *Client::httpFileUploadManager() const { return d->httpFileUploadManager; }

Jingle::Manager *Client::jingleManager() const { return d->jingleManager; }

bool Client::isActive() const { return d->active; }

QString Client::groupChatPassword(const QString &host, const QString &room) const
{
    Jid jid(room + "@" + host);
    for (const GroupChat &i : d->groupChatList) {
        if (i.j.compare(jid, false)) {
            return i.password;
        }
    }
    return QString();
}

void Client::groupChatChangeNick(const QString &host, const QString &room, const QString &nick, const Status &_s)
{
    Jid jid(room + "@" + host + "/" + nick);
    for (QList<GroupChat>::Iterator it = d->groupChatList.begin(); it != d->groupChatList.end(); it++) {
        GroupChat &i = *it;
        if (i.j.compare(jid, false)) {
            i.j = jid;

            Status s = _s;
            s.setIsAvailable(true);

            JT_Presence *j = new JT_Presence(rootTask());
            j->pres(jid, s);
            j->go(true);

            break;
        }
    }
}

bool Client::groupChatJoin(const QString &host, const QString &room, const QString &nick, const QString &password,
                           int maxchars, int maxstanzas, int seconds, const QDateTime &since, const Status &_s)
{
    Jid jid(room + "@" + host + "/" + nick);
    for (QList<GroupChat>::Iterator it = d->groupChatList.begin(); it != d->groupChatList.end();) {
        GroupChat &i = *it;
        if (i.j.compare(jid, false)) {
            // if this room is shutting down, then free it up
            if (i.status == GroupChat::Closing)
                it = d->groupChatList.erase(it);
            else
                return false;
        } else
            ++it;
    }

    debug(QString("Client: Joined: [%1]\n").arg(jid.full()));
    GroupChat i;
    i.j        = jid;
    i.status   = GroupChat::Connecting;
    i.password = password;
    d->groupChatList += i;

    JT_Presence *j = new JT_Presence(rootTask());
    Status       s = _s;
    s.setMUC();
    s.setMUCHistory(maxchars, maxstanzas, seconds, since);
    if (!password.isEmpty()) {
        s.setMUCPassword(password);
    }
    j->pres(jid, s);
    j->go(true);

    return true;
}

void Client::groupChatSetStatus(const QString &host, const QString &room, const Status &_s)
{
    Jid  jid(room + "@" + host);
    bool found = false;
    for (const GroupChat &i : d->groupChatList) {
        if (i.j.compare(jid, false)) {
            found = true;
            jid   = i.j;
            break;
        }
    }
    if (!found)
        return;

    Status s = _s;
    s.setIsAvailable(true);

    JT_Presence *j = new JT_Presence(rootTask());
    j->pres(jid, s);
    j->go(true);
}

void Client::groupChatLeave(const QString &host, const QString &room, const QString &statusStr)
{
    Jid jid(room + "@" + host);
    for (QList<GroupChat>::Iterator it = d->groupChatList.begin(); it != d->groupChatList.end(); it++) {
        GroupChat &i = *it;

        if (!i.j.compare(jid, false))
            continue;

        i.status = GroupChat::Closing;
        debug(QString("Client: Leaving: [%1]\n").arg(i.j.full()));

        JT_Presence *j = new JT_Presence(rootTask());
        Status       s;
        s.setIsAvailable(false);
        s.setStatus(statusStr);
        j->pres(i.j, s);
        j->go(true);
    }
}

void Client::groupChatLeaveAll(const QString &statusStr)
{
    if (d->stream && d->active) {
        for (QList<GroupChat>::Iterator it = d->groupChatList.begin(); it != d->groupChatList.end(); it++) {
            GroupChat &i = *it;
            i.status     = GroupChat::Closing;

            JT_Presence *j = new JT_Presence(rootTask());
            Status       s;
            s.setIsAvailable(false);
            s.setStatus(statusStr);
            j->pres(i.j, s);
            j->go(true);
        }
    }
}

QString Client::groupChatNick(const QString &host, const QString &room) const
{
    Jid jid(room + "@" + host);
    for (const GroupChat &gc : d->groupChatList) {
        if (gc.j.compare(jid, false)) {
            return gc.j.resource();
        }
    }
    return QString();
}

/*void Client::start()
{
    if(d->stream->old()) {
        // old has no activation step
        d->active = true;
        activated();
    }
    else {
        // TODO: IM session
    }
}*/

// TODO: fast close
void Client::close(bool)
{
    // fprintf(stderr, "\tClient::close\n");
    // fflush(stderr);

    if (d->stream) {
        d->stream->disconnect(this);
        d->stream->close();
        d->stream = nullptr;
    }
    disconnected();
    cleanup(); // TODO wait till stream writes all data to the socket
}

void Client::cleanup()
{
    d->active = false;
    // d->authed = false;
    d->groupChatList.clear();
}

/*void Client::continueAfterCert()
{
    d->stream->continueAfterCert();
}

void Client::streamConnected()
{
    connected();
}

void Client::streamHandshaken()
{
    handshaken();
}*/

void Client::streamError(int)
{
    // StreamError e = err;
    // error(e);

    // if(!e.isWarning()) {
    disconnected();
    cleanup();
    //}
} // namespace XMPP

/*void Client::streamSSLCertificateReady(const QSSLCert &cert)
{
    sslCertReady(cert);
}

void Client::streamCloseFinished()
{
    closeFinished();
}*/

void Client::streamReadyRead()
{
    // fprintf(stderr, "\tClientStream::streamReadyRead\n");
    // fflush(stderr);

    while (d->stream && d->stream->stanzaAvailable()) {
        Stanza s = d->stream->read();

        QString out = s.toString();
        debug(QString("Client: incoming: [\n%1]\n").arg(out));
        emit xmlIncoming(out);

        QDomElement x = s.element(); // oldStyleNS(s.element());
        distribute(x);
    }
}

void Client::streamIncomingXml(const QString &s)
{
    QString str = s;
    if (str.at(str.length() - 1) != '\n')
        str += '\n';
    emit xmlIncoming(str);
}

void Client::streamOutgoingXml(const QString &s)
{
    QString str = s;
    if (str.at(str.length() - 1) != '\n')
        str += '\n';
    emit xmlOutgoing(str);
}

void Client::parseUnhandledStreamFeatures()
{
    QList<QDomElement> nl = d->stream->unhandledFeatures();
    for (const QDomElement &e : nl) {
        if (e.localName() == "c" && e.namespaceURI() == NS_CAPS) {
            d->serverCaps = CapsSpec::fromXml(e);
            if (d->capsman->isEnabled()) {
                d->capsman->updateCaps(Jid(d->stream->jid().domain()), d->serverCaps);
            }
        }
    }
}

void Client::debug(const QString &str) { emit debugText(str); }

QString Client::genUniqueId()
{
    QString s = QString::asprintf("a%x", d->id_seed);
    d->id_seed += 0x10;
    return s;
}

Task *Client::rootTask() { return d->root; }

QDomDocument *Client::doc() const { return &d->doc; }

void Client::distribute(const QDomElement &x)
{
    static QString fromAttr(QStringLiteral("from"));
    if (x.hasAttribute(fromAttr)) {
        Jid j(x.attribute(fromAttr));
        if (!j.isValid()) {
            debug("Client: bad 'from' JID\n");
            return;
        }
    }

    if (!rootTask()->take(x) && (x.attribute("type") == "get" || x.attribute("type") == "set")) {
        debug("Client: Unrecognized IQ.\n");

        // Create reply element
        QDomElement reply = createIQ(doc(), "error", x.attribute("from"), x.attribute("id"));

        // Copy children
        for (QDomNode n = x.firstChild(); !n.isNull(); n = n.nextSibling()) {
            reply.appendChild(n.cloneNode());
        }

        // Add error
        QDomElement error = doc()->createElement("error");
        error.setAttribute("type", "cancel");
        reply.appendChild(error);

        QDomElement error_type = doc()->createElementNS(QLatin1String("urn:ietf:params:xml:ns:xmpp-stanzas"),
                                                        QLatin1String("feature-not-implemented"));
        error.appendChild(error_type);

        send(reply);
    }
}

void Client::send(const QDomElement &x)
{
    if (!d->stream)
        return;

    // QString out;
    // QTextStream ts(&out, IO_WriteOnly);
    // x.save(ts, 0);

    // QString out = Stream::xmlToString(x);
    // debug(QString("Client: outgoing: [\n%1]\n").arg(out));
    // xmlOutgoing(out);

    QDomElement e = addCorrectNS(x);
    Stanza      s = d->stream->createStanza(e);
    if (s.isNull()) { // e's namespace is not "jabber:client" or e.tagName is not in (message,presence,iq)
        // printf("bad stanza??\n");
        return;
    }
    emit stanzaElementOutgoing(e); // signal handler may change the node (TODO weird design?)
    if (e.isNull()) {              // so it was changed by signal above
        return;
    }
    QString out = s.toString();
    // qWarning() << "Out: " << out;
    debug(QString("Client: outgoing: [\n%1]\n").arg(out));
    emit xmlOutgoing(out);

    // printf("x[%s] x2[%s] s[%s]\n", Stream::xmlToString(x).toLatin1(), Stream::xmlToString(e).toLatin1(),
    // s.toString().toLatin1());
    d->stream->write(s);
}

void Client::send(const QString &str)
{
    if (!d->stream)
        return;

    debug(QString("Client: outgoing: [\n%1]\n").arg(str));
    emit xmlOutgoing(str);
    static_cast<ClientStream *>(d->stream)->writeDirect(str);
}

/* drops any pending outgoing xml elements */
void Client::clearSendQueue()
{
    if (d->stream)
        d->stream->clearSendQueue();
}

bool Client::hasStream() const { return !!d->stream; }

Stream &Client::stream() { return *(d->stream.data()); }

QString Client::streamBaseNS() const { return d->stream->baseNS(); }

const LiveRoster &Client::roster() const { return d->roster; }

const ResourceList &Client::resourceList() const { return d->resourceList; }

bool Client::isSessionRequired() const
{
    return d->stream && !d->stream->old() && d->stream->streamFeatures().session_required;
}

QString Client::host() const { return d->host; }

QString Client::user() const { return d->user; }

QString Client::pass() const { return d->pass; }

QString Client::resource() const { return d->resource; }

Jid Client::jid() const
{
    QString s;
    if (!d->user.isEmpty())
        s += d->user + '@';
    s += d->host;
    if (!d->resource.isEmpty()) {
        s += '/';
        s += d->resource;
    }

    return Jid(s);
}

void Client::setNetworkAccessManager(QNetworkAccessManager *qnam) { d->qnam = qnam; }

QNetworkAccessManager *Client::networkAccessManager() const { return d->qnam; }

void Client::ppSubscription(const Jid &j, const QString &s, const QString &n) { emit subscription(j, s, n); }

void Client::ppPresence(const Jid &j, const Status &s)
{
    if (s.isAvailable())
        debug(QString("Client: %1 is available.\n").arg(j.full()));
    else
        debug(QString("Client: %1 is unavailable.\n").arg(j.full()));

    for (QList<GroupChat>::Iterator it = d->groupChatList.begin(); it != d->groupChatList.end(); it++) {
        GroupChat &i = *it;

        if (i.j.compare(j, false)) {
            bool us = (i.j.resource() == j.resource() || j.resource().isEmpty()) ? true : false;

            debug(QString("for groupchat i=[%1] pres=[%2], [us=%3].\n").arg(i.j.full()).arg(j.full()).arg(us));
            switch (i.status) {
            case GroupChat::Connecting:
                if (us && s.hasError()) {
                    Jid j = i.j;
                    d->groupChatList.erase(it);
                    emit groupChatError(j, s.errorCode(), s.errorString());
                } else {
                    // don't signal success unless it is a non-error presence
                    if (!s.hasError()) {
                        i.status = GroupChat::Connected;
                        emit groupChatJoined(i.j);
                    }
                    emit groupChatPresence(j, s);
                }
                break;
            case GroupChat::Connected:
                emit groupChatPresence(j, s);
                break;
            case GroupChat::Closing:
                if (us && !s.isAvailable()) {
                    Jid j = i.j;
                    d->groupChatList.erase(it);
                    emit groupChatLeft(j);
                }
                break;
            default:
                break;
            }

            return;
        }
    }

    if (s.hasError()) {
        emit presenceError(j, s.errorCode(), s.errorString());
        return;
    }

    // is it me?
    if (j.compare(jid(), false)) {
        updateSelfPresence(j, s);
    } else {
        // update all relavent roster entries
        for (LiveRoster::Iterator it = d->roster.begin(); it != d->roster.end(); ++it) {
            LiveRosterItem &i = *it;

            if (!i.jid().compare(j, false))
                continue;

            // roster item has its own resource?
            if (!i.jid().resource().isEmpty()) {
                if (i.jid().resource() != j.resource())
                    continue;
            }

            updatePresence(&i, j, s);
        }
    }
}

void Client::updateSelfPresence(const Jid &j, const Status &s)
{
    ResourceList::Iterator rit   = d->resourceList.find(j.resource());
    bool                   found = (rit == d->resourceList.end()) ? false : true;

    // unavailable?  remove the resource
    if (!s.isAvailable()) {
        if (found) {
            debug(QString("Client: Removing self resource: name=[%1]\n").arg(j.resource()));
            (*rit).setStatus(s);
            emit resourceUnavailable(j, *rit);
            d->resourceList.erase(rit);
        }
    }
    // available?  add/update the resource
    else {
        Resource r;
        if (!found) {
            r = Resource(j.resource(), s);
            d->resourceList += r;
            debug(QString("Client: Adding self resource: name=[%1]\n").arg(j.resource()));
        } else {
            (*rit).setStatus(s);
            r = *rit;
            debug(QString("Client: Updating self resource: name=[%1]\n").arg(j.resource()));
        }

        emit resourceAvailable(j, r);
    }
}

void Client::updatePresence(LiveRosterItem *i, const Jid &j, const Status &s)
{
    ResourceList::Iterator rit   = i->resourceList().find(j.resource());
    bool                   found = (rit == i->resourceList().end()) ? false : true;

    // unavailable?  remove the resource
    if (!s.isAvailable()) {
        if (found) {
            (*rit).setStatus(s);
            debug(QString("Client: Removing resource from [%1]: name=[%2]\n").arg(i->jid().full()).arg(j.resource()));
            emit resourceUnavailable(j, *rit);
            i->resourceList().erase(rit);
            i->setLastUnavailableStatus(s);
        } else {
            // create the resource just for the purpose of emit
            Resource r = Resource(j.resource(), s);
            i->resourceList() += r;
            rit = i->resourceList().find(j.resource());
            emit resourceUnavailable(j, *rit);
            i->resourceList().erase(rit);
            i->setLastUnavailableStatus(s);
        }
    }
    // available?  add/update the resource
    else {
        Resource r;
        if (!found) {
            r = Resource(j.resource(), s);
            i->resourceList() += r;
            debug(QString("Client: Adding resource to [%1]: name=[%2]\n").arg(i->jid().full()).arg(j.resource()));
        } else {
            (*rit).setStatus(s);
            r = *rit;
            debug(QString("Client: Updating resource to [%1]: name=[%2]\n").arg(i->jid().full()).arg(j.resource()));
        }

        emit resourceAvailable(j, r);
    }
}

void Client::pmMessage(const Message &m)
{
    debug(QString("Client: Message from %1\n").arg(m.from().full()));

    // bits of binary. we can't do this in Message, since it knows nothing about Client
    for (const BoBData &b : m.bobDataList()) {
        d->bobman->append(b);
    }

    if (!m.ibbData().data.isEmpty()) {
        d->ibbman->takeIncomingData(m.from(), m.id(), m.ibbData(), Stanza::Message);
    }

    if (m.type() == "groupchat") {
        for (QList<GroupChat>::Iterator it = d->groupChatList.begin(); it != d->groupChatList.end(); it++) {
            const GroupChat &i = *it;

            if (!i.j.compare(m.from(), false))
                continue;

            if (i.status == GroupChat::Connected)
                messageReceived(m);
        }
    } else
        messageReceived(m);
}

void Client::prRoster(const Roster &r) { importRoster(r); }

void Client::rosterRequest(bool withGroupsDelimiter)
{
    if (!d->active)
        return;

    JT_Roster *r = new JT_Roster(rootTask());
    if (withGroupsDelimiter) {
        connect(r, &JT_Roster::finished, this, [this, r]() mutable {
            if (r->success()) {
                d->roster.setGroupsDelimiter(r->groupsDelimiter());
                emit rosterGroupsDelimiterRequestFinished(r->groupsDelimiter());
            }

            r = new JT_Roster(rootTask());
            connect(r, SIGNAL(finished()), SLOT(slotRosterRequestFinished()));
            r->get();
            d->roster.flagAllForDelete(); // mod_groups patch
            r->go(true);
        });
        r->getGroupsDelimiter();
        // WORKAROUND: Some bad servers (Facebook for example) don't respond
        // on groups delimiter request. Wait timeout and go ahead.
        r->setTimeout(GROUPS_DELIMITER_TIMEOUT);
    } else {
        connect(r, SIGNAL(finished()), SLOT(slotRosterRequestFinished()));
        r->get();
        d->roster.flagAllForDelete(); // mod_groups patch
    }

    r->go(true);
}

void Client::slotRosterRequestFinished()
{
    JT_Roster *r = static_cast<JT_Roster *>(sender());
    // on success, let's take it
    if (r->success()) {
        // d->roster.flagAllForDelete(); // mod_groups patch

        importRoster(r->roster());

        for (LiveRoster::Iterator it = d->roster.begin(); it != d->roster.end();) {
            LiveRosterItem &i = *it;
            if (i.flagForDelete()) {
                emit rosterItemRemoved(i);
                it = d->roster.erase(it);
            } else
                ++it;
        }
    } else {
        // don't report a disconnect.  Client::error() will do that.
        if (r->statusCode() == Task::ErrDisc)
            return;
    }

    // report success / fail
    emit rosterRequestFinished(r->success(), r->statusCode(), r->statusString());
}

void Client::importRoster(const Roster &r)
{
    emit beginImportRoster();
    for (Roster::ConstIterator it = r.begin(); it != r.end(); ++it) {
        importRosterItem(*it);
    }
    emit endImportRoster();
}

void Client::importRosterItem(const RosterItem &item)
{
    QString substr;
    switch (item.subscription().type()) {
    case Subscription::Both:
        substr = "<-->";
        break;
    case Subscription::From:
        substr = "  ->";
        break;
    case Subscription::To:
        substr = "<-  ";
        break;
    case Subscription::Remove:
        substr = "xxxx";
        break;
    case Subscription::None:
    default:
        substr = "----";
        break;
    }

    QString dstr, str = QString::asprintf("  %s %-32s", qPrintable(substr), qPrintable(item.jid().full()));
    if (!item.name().isEmpty())
        str += QString(" [") + item.name() + "]";
    str += '\n';

    // Remove
    if (item.subscription().type() == Subscription::Remove) {
        LiveRoster::Iterator it = d->roster.find(item.jid());
        if (it != d->roster.end()) {
            emit rosterItemRemoved(*it);
            d->roster.erase(it);
        }
        dstr = "Client: (Removed) ";
    }
    // Add/Update
    else {
        LiveRoster::Iterator it = d->roster.find(item.jid());
        if (it != d->roster.end()) {
            LiveRosterItem &i = *it;
            i.setFlagForDelete(false);
            i.setRosterItem(item);
            emit rosterItemUpdated(i);
            dstr = "Client: (Updated) ";
        } else {
            LiveRosterItem i(item);
            d->roster += i;

            // signal it
            emit rosterItemAdded(i);
            dstr = "Client: (Added)   ";
        }
    }

    debug(dstr + str);
}

void Client::sendMessage(Message &m)
{
    JT_Message *j = new JT_Message(rootTask(), m);
    j->go(true);
}

void Client::sendSubscription(const Jid &jid, const QString &type, const QString &nick)
{
    JT_Presence *j = new JT_Presence(rootTask());
    j->sub(jid, type, nick);
    j->go(true);
}

void Client::setPresence(const Status &s)
{
    if (d->capsman->isEnabled()) {
        if (d->caps.version().isEmpty() && !d->caps.node().isEmpty()) {
            d->caps = CapsSpec(makeDiscoResult(d->caps.node())); /* recompute caps hash */
        }
    }

    JT_Presence *j = new JT_Presence(rootTask());
    j->pres(s);
    j->go(true);

    // update our resourceList
    ppPresence(jid(), s);
    // ResourceList::Iterator rit = d->resourceList.find(resource());
    // Resource &r = *rit;
    // r.setStatus(s);
}

QString Client::OSName() const { return d->osName; }

QString Client::OSVersion() const { return d->osVersion; }

QString Client::timeZone() const { return d->tzname; }

int Client::timeZoneOffset() const { return d->tzoffset; }

/**
  \brief Returns true if Client is using old, manual time zone conversions.

  By default, conversions between UTC and local time are done automatically by Qt.
  In this mode, manualTimeZoneOffset() returns true,
  and timeZoneOffset() always retuns 0 (so you shouldn't use that function).

  However, if you call setTimeZone(), Client instance switches to old mode
  and uses given time zone offset for all calculations.
  */
bool Client::manualTimeZoneOffset() const { return d->useTzoffset; }

QString Client::clientName() const { return d->clientName; }

QString Client::clientVersion() const { return d->clientVersion; }

CapsSpec Client::caps() const { return d->caps; }

CapsSpec Client::serverCaps() const { return d->serverCaps; }

void Client::setOSName(const QString &name)
{
    if (d->osName != name)
        d->caps.resetVersion();
    d->osName = name;
}

void Client::setOSVersion(const QString &version)
{
    if (d->osVersion != version)
        d->caps.resetVersion();
    d->osVersion = version;
}

void Client::setTimeZone(const QString &name, int offset)
{
    d->tzname      = name;
    d->tzoffset    = offset;
    d->useTzoffset = true;
}

void Client::setClientName(const QString &s)
{
    if (d->clientName != s)
        d->caps.resetVersion();
    d->clientName = s;
}

void Client::setClientVersion(const QString &s)
{
    if (d->clientVersion != s)
        d->caps.resetVersion();
    d->clientVersion = s;
}

void Client::setCaps(const CapsSpec &s) { d->caps = s; }

void Client::setEncryptionHandler(EncryptionHandler *encryptionHandler) { d->encryptionHandler = encryptionHandler; }

EncryptionHandler *Client::encryptionHandler() const { return d->encryptionHandler; }

DiscoItem::Identity Client::identity() const { return d->identity; }

void Client::setIdentity(const DiscoItem::Identity &identity)
{
    if (!(d->identity == identity)) {
        d->caps.resetVersion();
    }
    d->identity = identity;
}

void Client::setFeatures(const Features &f)
{
    if (!(d->features == f)) {
        d->caps.resetVersion();
    }
    d->features = f;
}

const Features &Client::features() const { return d->features; }

DiscoItem Client::makeDiscoResult(const QString &node) const
{
    DiscoItem item;
    item.setNode(node);
    DiscoItem::Identity id = identity();
    if (id.category.isEmpty() || id.type.isEmpty()) {
        id.category = "client";
        id.type     = "pc";
    }
    item.setIdentities(id);

    Features features;

    if (d->ftman) {
        features.addFeature("http://jabber.org/protocol/bytestreams");
        features.addFeature("http://jabber.org/protocol/ibb");
        features.addFeature("http://jabber.org/protocol/si");
        features.addFeature("http://jabber.org/protocol/si/profile/file-transfer");
    }
    features.addFeature("http://jabber.org/protocol/disco#info");
    features.addFeature("jabber:x:data");
    features.addFeature("urn:xmpp:bob");
    features.addFeature("urn:xmpp:ping");
    features.addFeature("urn:xmpp:time");
    features.addFeature("urn:xmpp:message-correct:0");
    features.addFeature("urn:xmpp:jingle:1");
    // TODO rather do foreach for all registered jingle apps and transports
    features.addFeature("urn:xmpp:jingle:transports:s5b:1");
    features.addFeature("urn:xmpp:jingle:transports:ibb:1");
    // TODO: since it depends on UI it needs a way to be disabled
    features.addFeature("urn:xmpp:jingle:apps:file-transfer:5");
    Hash::populateFeatures(features);
    features.addFeature(NS_CAPS);

    // Client-specific features
    for (const QString &i : d->features.list()) {
        features.addFeature(i);
    }

    item.setFeatures(features);

    // xep-0232 Software Information
    XData            si;
    XData::FieldList si_fields;

    XData::Field si_type_field;
    si_type_field.setType(XData::Field::Field_Hidden);
    si_type_field.setVar("FORM_TYPE");
    si_type_field.setValue(QStringList(QLatin1String("urn:xmpp:dataforms:softwareinfo")));
    si_fields.append(si_type_field);

    XData::Field software_field;
    software_field.setType(XData::Field::Field_TextSingle);
    software_field.setVar("software");
    software_field.setValue(QStringList(d->clientName));
    si_fields.append(software_field);

    XData::Field software_v_field;
    software_v_field.setType(XData::Field::Field_TextSingle);
    software_v_field.setVar("software_version");
    software_v_field.setValue(QStringList(d->clientVersion));
    si_fields.append(software_v_field);

    XData::Field os_field;
    os_field.setType(XData::Field::Field_TextSingle);
    os_field.setVar("os");
    os_field.setValue(QStringList(d->osName));
    si_fields.append(os_field);

    XData::Field os_v_field;
    os_v_field.setType(XData::Field::Field_TextSingle);
    os_v_field.setVar("os_version");
    os_v_field.setValue(QStringList(d->osVersion));
    si_fields.append(os_v_field);

    si.setType(XData::Data_Result);
    si.setFields(si_fields);

    item.setExtensions(QList<XData>() << si);

    return item;
}

void Client::s5b_incomingReady() { handleIncoming(d->s5bman->takeIncoming()); }

void Client::ibb_incomingReady()
{
    auto c = d->ibbman->takeIncoming();
    if (!c)
        return;

    if (d->jingleIBBManager && d->jingleIBBManager->handleIncoming(c))
        return;
    handleIncoming(c);
}

void Client::handleIncoming(BSConnection *c)
{
    if (!c)
        return;
    if (!d->ftman) {
        c->close();
        c->deleteLater();
        return;
    }
    d->ftman->stream_incomingReady(c);
}

void Client::handleSMAckResponse(int h) { qDebug() << "handleSMAckResponse: h = " << h; }

//---------------------------------------------------------------------------
// LiveRosterItem
//---------------------------------------------------------------------------
LiveRosterItem::LiveRosterItem(const Jid &jid) : RosterItem(jid) { setFlagForDelete(false); }

LiveRosterItem::LiveRosterItem(const RosterItem &i)
{
    setRosterItem(i);
    setFlagForDelete(false);
}

LiveRosterItem::~LiveRosterItem() { }

void LiveRosterItem::setRosterItem(const RosterItem &i)
{
    setJid(i.jid());
    setName(i.name());
    setGroups(i.groups());
    setSubscription(i.subscription());
    setAsk(i.ask());
    setIsPush(i.isPush());
}

ResourceList &LiveRosterItem::resourceList() { return v_resourceList; }

ResourceList::Iterator LiveRosterItem::priority() { return v_resourceList.priority(); }

const ResourceList &LiveRosterItem::resourceList() const { return v_resourceList; }

ResourceList::ConstIterator LiveRosterItem::priority() const { return v_resourceList.priority(); }

bool LiveRosterItem::isAvailable() const
{
    if (v_resourceList.count() > 0)
        return true;
    return false;
}

const Status &LiveRosterItem::lastUnavailableStatus() const { return v_lastUnavailableStatus; }

bool LiveRosterItem::flagForDelete() const { return v_flagForDelete; }

void LiveRosterItem::setLastUnavailableStatus(const Status &s) { v_lastUnavailableStatus = s; }

void LiveRosterItem::setFlagForDelete(bool b) { v_flagForDelete = b; }

//---------------------------------------------------------------------------
// LiveRoster
//---------------------------------------------------------------------------
class LiveRoster::Private {
public:
    QString groupsDelimiter;
};

LiveRoster::LiveRoster() : QList<LiveRosterItem>(), d(new LiveRoster::Private) { }
LiveRoster::LiveRoster(const LiveRoster &other) : QList<LiveRosterItem>(other), d(new LiveRoster::Private)
{
    d->groupsDelimiter = other.d->groupsDelimiter;
}

LiveRoster::~LiveRoster() { delete d; }

LiveRoster &LiveRoster::operator=(const LiveRoster &other)
{
    QList<LiveRosterItem>::operator=(other);
    d->groupsDelimiter             = other.d->groupsDelimiter;
    return *this;
}
void LiveRoster::flagAllForDelete()
{
    for (Iterator it = begin(); it != end(); ++it)
        (*it).setFlagForDelete(true);
}

LiveRoster::Iterator LiveRoster::find(const Jid &j, bool compareRes)
{
    Iterator it;
    for (it = begin(); it != end(); ++it) {
        if ((*it).jid().compare(j, compareRes))
            break;
    }
    return it;
}

LiveRoster::ConstIterator LiveRoster::find(const Jid &j, bool compareRes) const
{
    ConstIterator it;
    for (it = begin(); it != end(); ++it) {
        if ((*it).jid().compare(j, compareRes))
            break;
    }
    return it;
}

void LiveRoster::setGroupsDelimiter(const QString &groupsDelimiter) { d->groupsDelimiter = groupsDelimiter; }

QString LiveRoster::groupsDelimiter() const { return d->groupsDelimiter; }

}
