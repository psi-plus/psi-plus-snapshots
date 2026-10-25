/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef XMPP_LINKLOCALSERVICE_H
#define XMPP_LINKLOCALSERVICE_H

#include "xmpp_status.h"
#include "xmpp_stream.h"
#include <QHostAddress>

namespace XMPP {
/** One XEP-0174 account on one local network link.
 *
 * Discovery, DNS-SD presence and TCP connections share the existing JDNS and
 * LinkLocalStream implementations. Attach this Stream to Client::connectToStream
 * to use the normal message/IQ tasks. No server login, XML presence or server
 * roster is involved. TCP is currently unauthenticated and unencrypted; callers
 * must explicitly enable this mode rather than treating DNS names as identities.
 * All objects must live on the same thread. The service owns its sockets.
 */
class IRIS_EXPORT LinkLocalService : public Stream {
    Q_OBJECT
public:
    explicit LinkLocalService(QObject *parent = nullptr);
    ~LinkLocalService() override;
    static bool isSupported();
    // address must be assigned locally; IPv6 link-local addresses need a scope.
    // A port of zero asks the operating system to choose an available port.
    // Online/error signals are deferred; connect them before calling start().
    bool start(const Jid &identity, const QHostAddress &address, const QString &displayName, quint16 port = 0);
    bool isOnline() const;
    Jid  localJid() const;
    void setStatus(const Status &status);
    void setDisplayName(const QString &name);
    void clearSendQueue();

    QDomDocument           &doc() const override;
    QString                 baseNS() const override;
    bool                    old() const override;
    void                    close() override;
    bool                    stanzaAvailable() const override;
    Stanza                  read() override;
    void                    write(const Stanza &stanza) override;
    int                     errorCondition() const override;
    QString                 errorText() const override;
    QHash<QString, QString> errorLangText() const override;
    QDomElement             errorAppSpec() const override;

signals:
    void online();
    // A conflict may change the local identity before it is announced.
    void identityChanged(const XMPP::Jid &identity);
    void peerChanged(const XMPP::Jid &peer, const QString &displayName, const XMPP::Status &status);
    // A peer failure does not disconnect the whole account.
    void peerError(const XMPP::Jid &peer, const QString &description);

private:
    class Private;
    Private *d;
};
}
#endif
