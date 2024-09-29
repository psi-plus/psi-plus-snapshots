/*
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

#ifndef XMPP_CLIENT_H
#define XMPP_CLIENT_H

#include "xmpp/jid/jid.h"
#include "xmpp_discoitem.h"
#include "xmpp_status.h"

#include <QCryptographicHash>
#include <QObject>
#include <QStringList>

class ByteStream;
class QDomDocument;
class QDomElement;
class QNetworkAccessManager;

namespace XMPP {
class BSConnection;
class CapsManager;
class CarbonsManager;
class ClientStream;
class EncryptionHandler;
class Features;
class FileTransferManager;
class HttpFileUploadManager;
class IBBManager;
class JT_PushMessage;
class JidLinkManager;
class LiveRoster;
class LiveRosterItem;
class Message;
class Resource;
class ResourceList;
class Roster;
class RosterItem;
class S5BManager;
class ServerInfoManager;
class Stream;
class Task;
class TcpPortReserver;
class ExternalServiceDiscovery;
class StunDiscoManager;

namespace Jingle {
    class Manager;
    namespace S5B {
        class Manager;
    }
    namespace IBB {
        class Manager;
    }
    namespace ICE {
        class Manager;
    }
}

class Client : public QObject {
    Q_OBJECT

public:
    Client(QObject *parent = nullptr);
    ~Client();

    bool isActive() const;
    void connectToServer(ClientStream *s, const Jid &j, bool auth = true);
    void start(const QString &host, const QString &user, const QString &pass, const QString &resource);
    void close(bool fast = false);

    bool                hasStream() const;
    Stream             &stream();
    QString             streamBaseNS() const;
    const LiveRoster   &roster() const;
    const ResourceList &resourceList() const;
    bool                isSessionRequired() const;

    void send(const QDomElement &);
    void send(const QString &);
    void clearSendQueue();

    QString host() const;
    QString user() const;
    QString pass() const;
    QString resource() const;
    Jid     jid() const;

    void                   setNetworkAccessManager(QNetworkAccessManager *qnam);
    QNetworkAccessManager *networkAccessManager() const;

    void rosterRequest(bool withGroupsDelimiter = true);
    void sendMessage(Message &);
    void sendSubscription(const Jid &, const QString &, const QString &nick = QString());
    void setPresence(const Status &);

    void          debug(const QString &);
    QString       genUniqueId();
    Task         *rootTask();
    QDomDocument *doc() const;

    QString  OSName() const;
    QString  OSVersion() const;
    QString  timeZone() const;
    int      timeZoneOffset() const;
    bool     manualTimeZoneOffset() const;
    QString  clientName() const;
    QString  clientVersion() const;
    CapsSpec caps() const;
    CapsSpec serverCaps() const;

    void               setOSName(const QString &);
    void               setOSVersion(const QString &);
    void               setTimeZone(const QString &, int);
    void               setClientName(const QString &);
    void               setClientVersion(const QString &);
    void               setCaps(const CapsSpec &);
    void               setEncryptionHandler(EncryptionHandler *);
    EncryptionHandler *encryptionHandler() const;

    void                setIdentity(const DiscoItem::Identity &);
    DiscoItem::Identity identity() const;

    void            setFeatures(const Features &f);
    const Features &features() const;
    DiscoItem       makeDiscoResult(const QString &node = QString()) const;
    void            setCapsOptimizationAllowed(bool allowed);
    bool            capsOptimizationAllowed() const;

    void                      setTcpPortReserver(TcpPortReserver *portReserver);
    TcpPortReserver          *tcpPortReserver() const;
    S5BManager               *s5bManager() const;
    IBBManager               *ibbManager() const;
    BoBManager               *bobManager() const;
    JidLinkManager           *jidLinkManager() const;
    CapsManager              *capsManager() const;
    CarbonsManager           *carbonsManager() const;
    JT_PushMessage           *pushMessage() const;
    ServerInfoManager        *serverInfoManager() const;
    ExternalServiceDiscovery *externalServiceDiscovery() const;
    StunDiscoManager         *stunDiscoManager() const;
    HttpFileUploadManager    *httpFileUploadManager() const;
    Jingle::Manager          *jingleManager() const;
    Jingle::S5B::Manager     *jingleS5BManager() const;
    Jingle::IBB::Manager     *jingleIBBManager() const;
    Jingle::ICE::Manager     *jingleICEManager() const;

    void                 setFileTransferEnabled(bool b);
    FileTransferManager *fileTransferManager() const;

    QString groupChatPassword(const QString &host, const QString &room) const;
    bool    groupChatJoin(const QString &host, const QString &room, const QString &nick,
                          const QString &password = QString(), int maxchars = -1, int maxstanzas = -1, int seconds = -1,
                          const QDateTime &since = QDateTime(), const Status    & = Status());
    void    groupChatSetStatus(const QString &host, const QString &room, const Status &);
    void    groupChatChangeNick(const QString &host, const QString &room, const QString &nick, const Status &);
    void    groupChatLeave(const QString &host, const QString &room, const QString &statusStr = QString());
    void    groupChatLeaveAll(const QString &statusStr = QString());
    QString groupChatNick(const QString &host, const QString &room) const;

signals:
    void activated();
    void disconnected();
    // void authFinished(bool, int, const QString &);
    void rosterGroupsDelimiterRequestFinished(const QString &);
    void rosterRequestFinished(bool, int, const QString &);
    void rosterItemAdded(const RosterItem &);
    void rosterItemUpdated(const RosterItem &);
    void rosterItemRemoved(const RosterItem &);
    void resourceAvailable(const Jid &, const Resource &);
    void resourceUnavailable(const Jid &, const Resource &);
    void presenceError(const Jid &, int, const QString &);
    void subscription(const Jid &, const QString &, const QString &);
    void messageReceived(const Message &);
    void debugText(const QString &);
    void xmlIncoming(const QString &);
    void xmlOutgoing(const QString &);
    void stanzaElementOutgoing(QDomElement &);
    void groupChatJoined(const Jid &);
    void groupChatLeft(const Jid &);
    void groupChatPresence(const Jid &, const Status &);
    void groupChatError(const Jid &, int, const QString &);

    void incomingJidLink();

    void beginImportRoster();
    void endImportRoster();

private slots:
    // void streamConnected();
    // void streamHandshaken();
    // void streamError(const StreamError &);
    // void streamSSLCertificateReady(const QSSLCert &);
    // void streamCloseFinished();
    void streamError(int);
    void streamReadyRead();
    void streamIncomingXml(const QString &);
    void streamOutgoingXml(const QString &);

    void slotRosterRequestFinished();

    // basic daemons
    void ppSubscription(const Jid &, const QString &, const QString &);
    void ppPresence(const Jid &, const Status &);
    void pmMessage(const Message &);
    void prRoster(const Roster &);

    void s5b_incomingReady();
    void ibb_incomingReady();

    void handleSMAckResponse(int);
    void parseUnhandledStreamFeatures();

public:
    class GroupChat;

private:
    void cleanup();
    void distribute(const QDomElement &);
    void importRoster(const Roster &);
    void importRosterItem(const RosterItem &);
    void updateSelfPresence(const Jid &, const Status &);
    void updatePresence(LiveRosterItem *, const Jid &, const Status &);
    void handleIncoming(BSConnection *);

    void sendAckRequest();

    class ClientPrivate;
    ClientPrivate *d;
};
} // namespace XMPP

#endif // XMPP_CLIENT_H
