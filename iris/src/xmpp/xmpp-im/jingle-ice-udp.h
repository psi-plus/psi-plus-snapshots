// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_ICE_UDP_H
#define JINGLE_ICE_UDP_H

#include <QByteArray>
#include <QDomElement>
#include <QHostAddress>
#include <QList>
#include <QString>
#include <optional>

namespace XMPP::Jingle::ICE {

extern const QString NS_ICE_UDP;

struct UdpCandidate {
    int          component = -1;
    QString      foundation;
    int          generation = -1;
    QString      id;
    QHostAddress ip;
    int          network  = -1;
    int          port     = -1;
    int          priority = -1;
    QString      protocol;
    QHostAddress relAddr;
    int          relPort = -1;
    QString      type;
};

struct UdpRemoteCandidate {
    int          component = -1;
    QHostAddress ip;
    int          port = -1;
};

// Wire model for XEP-0176 only. Foreign namespaced children (for example
// XEP-0320 fingerprints) are preserved but not interpreted here.
struct UdpTransportDescription {
    QString                           pwd;
    QString                           ufrag;
    QList<UdpCandidate>               candidates;
    std::optional<UdpRemoteCandidate> remoteCandidate;
    QList<QByteArray>                  extensions; // serialized foreign XML; no parser-document lifetime

    bool isValid(QString *error = nullptr) const;
};

class UdpTransportCodec {
public:
    static std::optional<UdpTransportDescription> fromXml(const QDomElement &transport, QString *error = nullptr);
    static QDomElement toXml(QDomDocument &doc, const UdpTransportDescription &transport, QString *error = nullptr);
};

// The production ICE implementation predates XEP-0176 and internally uses the
// XEP-0371-shaped ice:0 DOM. These helpers are a wire adapter only: ICE state,
// candidate gathering, DTLS and packet ownership remain shared.
QDomElement iceUdpToInternal(QDomDocument &doc, const QDomElement &transport, const QString &internalNamespace,
                             QString *error = nullptr);
QDomElement internalToIceUdp(QDomDocument &doc, const QDomElement &transport, QString *error = nullptr);

} // namespace XMPP::Jingle::ICE

#endif // JINGLE_ICE_UDP_H
