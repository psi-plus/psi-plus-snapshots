/*
 * Copyright (C) 2021  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#ifndef XMPP_DTLS_H
#define XMPP_DTLS_H

#include <QAbstractSocket>

#include "xmpp_hash.h"

namespace QCA {
class Certificate;
class PrivateKey;
}

namespace XMPP {

/*
Calls order:
juliet: startOutgoing() -> localFingerprint() -> network
        network (remote fingerpring) -> setRemoteFingerprint() -> negotiate(start server) -> network (iq result)
romeo:  setRemoteFingerprint() -> acceptIncoming() -> localFingerprint() -> network
        network (iq result) -> negotiate(start client)
*/

class Dtls : public QObject {
    Q_OBJECT
public:
    enum Setup { NotSet, Active, Passive, ActPass, HoldConn };

    struct FingerPrint {
        Hash  hash;
        Setup setup = Setup(-1);

        FingerPrint() : setup(NotSet) { }
        inline FingerPrint(const QDomElement &el) { parse(el); }
        FingerPrint(const Hash &hash, Setup setup) : hash(hash), setup(setup) { }

        static QString ns();

        inline bool operator==(const FingerPrint &other) const { return setup == other.setup || hash == other.hash; }
        inline bool operator!=(const FingerPrint &other) const { return !(*this == other); }

        bool        parse(const QDomElement &el);
        inline bool isValid() const
        {
            return hash.isValid() && !hash.data().isEmpty() && setup >= Active && setup <= HoldConn;
        }
        QDomElement toXml(QDomDocument *doc) const;
    };

    explicit Dtls(QObject *parent = nullptr, const QString &localJid = QString(), const QString &remoteJid = QString());

    // state machine stuff
    void initOutgoing();   // when it's our side first send dtls info
    void acceptIncoming(); // when we need to respond to the remote dtls info
    void onRemoteAcceptedFingerprint();

    void             setLocalCertificate(const QCA::Certificate &cert, const QCA::PrivateKey &pkey);
    QCA::Certificate localCertificate() const;
    QCA::Certificate remoteCertificate() const;

    const FingerPrint &localFingerprint() const;
    const FingerPrint &remoteFingerprint() const;
    void               setRemoteFingerprint(const FingerPrint &fingerprint);

    QAbstractSocket::SocketError error() const;

    QByteArray readDatagram();
    QByteArray readOutgoingDatagram();
    void       writeDatagram(const QByteArray &data);
    void       writeIncomingDatagram(const QByteArray &data);

    bool isStarted() const;

    static bool isSupported();
signals:
    void needRestart();
    void readyRead();
    void readyReadOutgoing();
    void connected();
    void errorOccurred(QAbstractSocket::SocketError);
    void closed();

private:
    void negotiate(); // yep. it's possible to make it public but not really necessary atm

    class Private;
    Private *d;
};

} // namespace XMPP

#endif // XMPP_DTLS_H
