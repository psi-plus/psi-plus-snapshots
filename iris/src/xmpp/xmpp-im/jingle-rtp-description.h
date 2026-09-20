// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef JINGLE_RTP_DESCRIPTION_H
#define JINGLE_RTP_DESCRIPTION_H

#include "jingle.h"

#include <QDomElement>
#include <QList>
#include <QMap>
#include <iris/iris_export.h>
#include <optional>

namespace XMPP::Jingle::RTP {

struct ExtensionParameter {
    QString                name;
    std::optional<QString> value;
};

// XEP-0293. Presence at description scope applies to the whole content;
// presence inside PayloadType applies only to that payload.
struct Feedback {
    QString                   type;
    QString                   subtype;
    QList<ExtensionParameter> parameters;
};

// XEP-0294. senders defaults to Both when omitted on the wire.
struct HeaderExtension {
    quint16                   id = 0;
    QString                   uri;
    Origin                    senders = Origin::Both;
    QList<ExtensionParameter> parameters;
};

// XEP-0339 source-specific media attributes.
struct Source {
    quint32                   ssrc = 0;
    QList<ExtensionParameter> parameters;
};

struct SourceGroup {
    QString        semantics;
    QList<quint32> sources;
};

struct PayloadType {
    quint8                 id = 0;
    QString                name;
    std::optional<quint32> clockrate;
    std::optional<quint8>  channels; // absent means one in a full offer/answer
    std::optional<quint32> ptime;
    std::optional<quint32> maxptime;
    QMap<QString, QString> parameters;
    QList<Feedback>        feedback;
    // XEP-0293 allows zero even though an older XML schema revision used positiveInteger.
    std::optional<quint32> feedbackTrrInt;
    // Unknown payload extensions remain opaque and round-trip unchanged.
    // Serialized XML keeps this value object independent of a QDomDocument lifetime.
    QList<QByteArray> extensions;
};

// Wire description only. Codec/extension selection belongs to the media-provider
// contract; parsing a typed extension does not advertise or accept that capability.
struct IRIS_EXPORT Description {
    QString                media;
    std::optional<quint32> ssrc;
    QList<PayloadType>     payloads;
    bool                   rtcpMux = false;

    QList<Feedback>        feedback;
    std::optional<quint32> feedbackTrrInt;
    QList<HeaderExtension> headerExtensions;
    bool                   extmapAllowMixed = false;
    QList<Source>          sources;
    QList<SourceGroup>     sourceGroups;

    // Unknown description extensions remain opaque and round-trip unchanged.
    // Never retain QDomElement handles from a parser-owned document here.
    QList<QByteArray> extensions;

    static QString ns();
    // Advisory description-info can omit payloads or carry incomplete payloads.
    // It must not be treated as a replacement offer/answer.
    static std::optional<Description> fromXml(const QDomElement &, bool advisory = false);
    QDomElement                       toXml(QDomDocument &) const;
};

}
#endif
