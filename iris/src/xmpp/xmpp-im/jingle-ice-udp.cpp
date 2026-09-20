// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-ice-udp.h"

#include <QSet>
#include <limits>

namespace XMPP::Jingle::ICE {

const QString NS_ICE_UDP = QStringLiteral("urn:xmpp:jingle:transports:ice-udp:1");

namespace {

    QByteArray serializeOpaqueExtension(const QDomElement &element)
    {
        QDomDocument owned;
        const auto   imported = owned.importNode(element, true);
        if (imported.isNull())
            return {};
        owned.appendChild(imported);
        return owned.toByteArray(-1);
    }

    QDomElement deserializeOpaqueExtension(QDomDocument &target, const QByteArray &xml)
    {
        QDomDocument owned;
        if (!owned.setContent(xml, true))
            return {};
        const auto element = owned.documentElement();
        return element.isNull() ? QDomElement() : target.importNode(element, true).toElement();
    }

    QString elementName(const QDomElement &element)
    {
        return element.localName().isEmpty() ? element.tagName() : element.localName();
    }

    bool fail(QString *error, const QString &message)
    {
        if (error)
            *error = message;
        return false;
    }

    bool parseInteger(const QDomElement &element, const QString &name, qlonglong minimum, qlonglong maximum,
                      int *result, QString *error, bool optional = false)
    {
        if (!element.hasAttribute(name)) {
            if (optional) {
                *result = -1;
                return true;
            }
            return fail(error, QStringLiteral("Missing %1 attribute").arg(name));
        }

        bool            ok    = false;
        const qlonglong value = element.attribute(name).toLongLong(&ok);
        if (!ok || value < minimum || value > maximum)
            return fail(error, QStringLiteral("Invalid %1 attribute").arg(name));

        *result = int(value);
        return true;
    }

    bool parseAddress(const QDomElement &element, const QString &name, QHostAddress *result, QString *error,
                      bool optional = false)
    {
        if (!element.hasAttribute(name)) {
            if (optional) {
                *result = QHostAddress();
                return true;
            }
            return fail(error, QStringLiteral("Missing %1 attribute").arg(name));
        }

        *result = QHostAddress(element.attribute(name));
        if (result->isNull())
            return fail(error, QStringLiteral("Invalid %1 attribute").arg(name));
        return true;
    }

    bool validateCandidate(const UdpCandidate &candidate, QString *error)
    {
        if (candidate.component < 1 || candidate.component > 255)
            return fail(error, QStringLiteral("Invalid component attribute"));
        if (candidate.foundation.isEmpty())
            return fail(error, QStringLiteral("Missing foundation attribute"));
        if (candidate.generation < 0 || candidate.generation > 255)
            return fail(error, QStringLiteral("Invalid generation attribute"));
        if (candidate.id.isEmpty())
            return fail(error, QStringLiteral("Missing id attribute"));
        if (candidate.ip.isNull())
            return fail(error, QStringLiteral("Invalid ip attribute"));
        if (candidate.network < -1 || candidate.network > 255)
            return fail(error, QStringLiteral("Invalid network attribute"));
        if (candidate.port < 1 || candidate.port > 65535)
            return fail(error, QStringLiteral("Invalid port attribute"));
        if (candidate.priority < 1)
            return fail(error, QStringLiteral("Invalid priority attribute"));
        if (candidate.protocol != QStringLiteral("udp"))
            return fail(error, QStringLiteral("Unsupported ICE-UDP protocol"));
        if (candidate.relPort < -1 || candidate.relPort == 0 || candidate.relPort > 65535)
            return fail(error, QStringLiteral("Invalid rel-port attribute"));

        static const QSet<QString> candidateTypes { QStringLiteral("host"), QStringLiteral("prflx"),
                                                    QStringLiteral("relay"), QStringLiteral("srflx") };
        if (!candidateTypes.contains(candidate.type))
            return fail(error, QStringLiteral("Unsupported ICE-UDP candidate type"));

        return true;
    }

    bool validateRemoteCandidate(const UdpRemoteCandidate &candidate, QString *error)
    {
        if (candidate.component < 1 || candidate.component > 255)
            return fail(error, QStringLiteral("Invalid remote-candidate component"));
        if (candidate.ip.isNull())
            return fail(error, QStringLiteral("Invalid remote-candidate ip"));
        if (candidate.port < 1 || candidate.port > 65535)
            return fail(error, QStringLiteral("Invalid remote-candidate port"));
        return true;
    }

    bool parseCandidate(const QDomElement &element, UdpCandidate *candidate, QString *error)
    {
        if (!element.firstChildElement().isNull() || !element.text().trimmed().isEmpty())
            return fail(error, QStringLiteral("ICE-UDP candidate must be empty"));

        if (!parseInteger(element, QStringLiteral("component"), 1, 255, &candidate->component, error)
            || !parseInteger(element, QStringLiteral("generation"), 0, 255, &candidate->generation, error)
            || !parseAddress(element, QStringLiteral("ip"), &candidate->ip, error)
            || !parseInteger(element, QStringLiteral("port"), 1, 65535, &candidate->port, error)
            || !parseInteger(element, QStringLiteral("priority"), 1, std::numeric_limits<int>::max(),
                             &candidate->priority, error)
            || !parseInteger(element, QStringLiteral("network"), 0, 255, &candidate->network, error, true)
            || !parseAddress(element, QStringLiteral("rel-addr"), &candidate->relAddr, error, true)
            || !parseInteger(element, QStringLiteral("rel-port"), 1, 65535, &candidate->relPort, error, true)) {
            return false;
        }

        candidate->foundation = element.attribute(QStringLiteral("foundation"));
        candidate->id         = element.attribute(QStringLiteral("id"));
        candidate->protocol   = element.attribute(QStringLiteral("protocol"));
        candidate->type       = element.attribute(QStringLiteral("type"));
        return validateCandidate(*candidate, error);
    }

    bool parseRemoteCandidate(const QDomElement &element, UdpRemoteCandidate *candidate, QString *error)
    {
        if (!element.firstChildElement().isNull() || !element.text().trimmed().isEmpty())
            return fail(error, QStringLiteral("ICE-UDP remote-candidate must be empty"));

        if (!parseInteger(element, QStringLiteral("component"), 1, 255, &candidate->component, error)
            || !parseAddress(element, QStringLiteral("ip"), &candidate->ip, error)
            || !parseInteger(element, QStringLiteral("port"), 1, 65535, &candidate->port, error)) {
            return false;
        }
        return validateRemoteCandidate(*candidate, error);
    }

    QDomElement candidateToXml(QDomDocument &doc, const UdpCandidate &candidate)
    {
        auto element = doc.createElementNS(NS_ICE_UDP, QStringLiteral("candidate"));
        element.setAttribute(QStringLiteral("component"), candidate.component);
        element.setAttribute(QStringLiteral("foundation"), candidate.foundation);
        element.setAttribute(QStringLiteral("generation"), candidate.generation);
        element.setAttribute(QStringLiteral("id"), candidate.id);
        element.setAttribute(QStringLiteral("ip"), candidate.ip.toString());
        if (candidate.network >= 0)
            element.setAttribute(QStringLiteral("network"), candidate.network);
        element.setAttribute(QStringLiteral("port"), candidate.port);
        element.setAttribute(QStringLiteral("priority"), candidate.priority);
        element.setAttribute(QStringLiteral("protocol"), candidate.protocol);
        if (!candidate.relAddr.isNull())
            element.setAttribute(QStringLiteral("rel-addr"), candidate.relAddr.toString());
        if (candidate.relPort >= 0)
            element.setAttribute(QStringLiteral("rel-port"), candidate.relPort);
        element.setAttribute(QStringLiteral("type"), candidate.type);
        return element;
    }

    QDomElement remoteCandidateToXml(QDomDocument &doc, const UdpRemoteCandidate &candidate)
    {
        auto element = doc.createElementNS(NS_ICE_UDP, QStringLiteral("remote-candidate"));
        element.setAttribute(QStringLiteral("component"), candidate.component);
        element.setAttribute(QStringLiteral("ip"), candidate.ip.toString());
        element.setAttribute(QStringLiteral("port"), candidate.port);
        return element;
    }

    QDomElement internalCandidateToUdp(QDomDocument &doc, const QDomElement &candidate, QString *error)
    {
        auto                     converted = doc.createElementNS(NS_ICE_UDP, QStringLiteral("candidate"));
        static const QStringList attributes { QStringLiteral("component"),  QStringLiteral("foundation"),
                                              QStringLiteral("generation"), QStringLiteral("id"),
                                              QStringLiteral("ip"),         QStringLiteral("network"),
                                              QStringLiteral("port"),       QStringLiteral("priority"),
                                              QStringLiteral("protocol"),   QStringLiteral("rel-addr"),
                                              QStringLiteral("rel-port"),   QStringLiteral("type") };
        for (const auto &attribute : attributes) {
            if (candidate.hasAttribute(attribute))
                converted.setAttribute(attribute, candidate.attribute(attribute));
        }
        UdpCandidate parsed;
        if (!parseCandidate(converted, &parsed, error))
            return {};
        return converted;
    }

} // namespace

bool UdpTransportDescription::isValid(QString *error) const
{
    if (!candidates.isEmpty() && remoteCandidate)
        return fail(error, QStringLiteral("ICE-UDP transport cannot mix candidate and remote-candidate"));
    if (!candidates.isEmpty() && (pwd.isEmpty() || ufrag.isEmpty()))
        return fail(error, QStringLiteral("ICE-UDP candidates require pwd and ufrag"));

    QSet<QString> candidateIds;
    for (const auto &candidate : candidates) {
        if (!validateCandidate(candidate, error))
            return false;
        if (candidateIds.contains(candidate.id))
            return fail(error, QStringLiteral("Duplicate ICE-UDP candidate id"));
        candidateIds.insert(candidate.id);
    }

    if (remoteCandidate && !validateRemoteCandidate(*remoteCandidate, error))
        return false;

    for (const auto &extension : extensions) {
        QDomDocument extensionDoc;
        if (!extensionDoc.setContent(extension, true))
            return fail(error, QStringLiteral("Malformed ICE-UDP extension"));
        const auto element = extensionDoc.documentElement();
        if (element.isNull() || element.namespaceURI().isEmpty() || element.namespaceURI() == NS_ICE_UDP)
            return fail(error, QStringLiteral("Invalid ICE-UDP extension namespace"));
    }

    return true;
}

std::optional<UdpTransportDescription> UdpTransportCodec::fromXml(const QDomElement &transport, QString *error)
{
    if (transport.namespaceURI() != NS_ICE_UDP || elementName(transport) != QStringLiteral("transport")) {
        fail(error, QStringLiteral("Not an XEP-0176 transport element"));
        return std::nullopt;
    }

    UdpTransportDescription result;
    result.pwd   = transport.attribute(QStringLiteral("pwd"));
    result.ufrag = transport.attribute(QStringLiteral("ufrag"));

    for (auto element = transport.firstChildElement(); !element.isNull(); element = element.nextSiblingElement()) {
        if (element.namespaceURI() != NS_ICE_UDP) {
            result.extensions.append(serializeOpaqueExtension(element));
            continue;
        }

        const auto name = elementName(element);
        if (name == QStringLiteral("candidate")) {
            UdpCandidate candidate;
            if (!parseCandidate(element, &candidate, error))
                return std::nullopt;
            result.candidates.append(candidate);
        } else if (name == QStringLiteral("remote-candidate")) {
            if (result.remoteCandidate) {
                fail(error, QStringLiteral("Multiple ICE-UDP remote-candidate elements"));
                return std::nullopt;
            }
            UdpRemoteCandidate candidate;
            if (!parseRemoteCandidate(element, &candidate, error))
                return std::nullopt;
            result.remoteCandidate = candidate;
        } else {
            fail(error, QStringLiteral("Unknown XEP-0176 transport child"));
            return std::nullopt;
        }
    }

    if (!result.isValid(error))
        return std::nullopt;
    return result;
}

QDomElement UdpTransportCodec::toXml(QDomDocument &doc, const UdpTransportDescription &transport, QString *error)
{
    if (!transport.isValid(error))
        return {};

    auto element = doc.createElementNS(NS_ICE_UDP, QStringLiteral("transport"));
    if (!transport.pwd.isEmpty())
        element.setAttribute(QStringLiteral("pwd"), transport.pwd);
    if (!transport.ufrag.isEmpty())
        element.setAttribute(QStringLiteral("ufrag"), transport.ufrag);

    for (const auto &candidate : transport.candidates)
        element.appendChild(candidateToXml(doc, candidate));
    if (transport.remoteCandidate)
        element.appendChild(remoteCandidateToXml(doc, *transport.remoteCandidate));
    for (const auto &extension : transport.extensions) {
        const auto child = deserializeOpaqueExtension(doc, extension);
        if (!child.isNull())
            element.appendChild(child);
    }

    return element;
}

QDomElement iceUdpToInternal(QDomDocument &doc, const QDomElement &transport, const QString &internalNamespace,
                             QString *error)
{
    const auto parsed = UdpTransportCodec::fromXml(transport, error);
    if (!parsed || internalNamespace.isEmpty())
        return {};

    auto internal = doc.createElementNS(internalNamespace, QStringLiteral("transport"));
    if (!parsed->pwd.isEmpty())
        internal.setAttribute(QStringLiteral("pwd"), parsed->pwd);
    if (!parsed->ufrag.isEmpty())
        internal.setAttribute(QStringLiteral("ufrag"), parsed->ufrag);

    for (const auto &candidate : parsed->candidates) {
        auto element = doc.createElement(QStringLiteral("candidate"));
        element.setAttribute(QStringLiteral("component"), candidate.component);
        element.setAttribute(QStringLiteral("foundation"), candidate.foundation);
        element.setAttribute(QStringLiteral("generation"), candidate.generation);
        element.setAttribute(QStringLiteral("id"), candidate.id);
        element.setAttribute(QStringLiteral("ip"), candidate.ip.toString());
        if (candidate.network >= 0)
            element.setAttribute(QStringLiteral("network"), candidate.network);
        element.setAttribute(QStringLiteral("port"), candidate.port);
        element.setAttribute(QStringLiteral("priority"), candidate.priority);
        element.setAttribute(QStringLiteral("protocol"), candidate.protocol);
        if (!candidate.relAddr.isNull())
            element.setAttribute(QStringLiteral("rel-addr"), candidate.relAddr.toString());
        if (candidate.relPort >= 0)
            element.setAttribute(QStringLiteral("rel-port"), candidate.relPort);
        element.setAttribute(QStringLiteral("type"), candidate.type);
        internal.appendChild(element);
    }
    if (parsed->remoteCandidate) {
        auto element = doc.createElement(QStringLiteral("remote-candidate"));
        element.setAttribute(QStringLiteral("component"), parsed->remoteCandidate->component);
        element.setAttribute(QStringLiteral("ip"), parsed->remoteCandidate->ip.toString());
        element.setAttribute(QStringLiteral("port"), parsed->remoteCandidate->port);
        internal.appendChild(element);
    }
    for (const auto &extension : parsed->extensions) {
        const auto child = deserializeOpaqueExtension(doc, extension);
        if (!child.isNull())
            internal.appendChild(child);
    }
    return internal;
}

QDomElement internalToIceUdp(QDomDocument &doc, const QDomElement &transport, QString *error)
{
    if (transport.isNull() || elementName(transport) != QStringLiteral("transport")) {
        fail(error, QStringLiteral("Invalid internal ICE transport element"));
        return {};
    }

    UdpTransportDescription converted;
    converted.pwd   = transport.attribute(QStringLiteral("pwd"));
    converted.ufrag = transport.attribute(QStringLiteral("ufrag"));

    for (auto child = transport.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        const auto name = elementName(child);
        if (name == QStringLiteral("candidate") && child.namespaceURI().isEmpty()) {
            auto udpCandidate = internalCandidateToUdp(doc, child, error);
            if (udpCandidate.isNull())
                return {};
            UdpCandidate parsed;
            if (!parseCandidate(udpCandidate, &parsed, error))
                return {};
            converted.candidates.append(parsed);
        } else if (name == QStringLiteral("remote-candidate") && child.namespaceURI().isEmpty()) {
            if (converted.remoteCandidate) {
                fail(error, QStringLiteral("Multiple internal remote-candidate elements"));
                return {};
            }
            UdpRemoteCandidate remote;
            if (!parseInteger(child, QStringLiteral("component"), 1, 255, &remote.component, error)
                || !parseAddress(child, QStringLiteral("ip"), &remote.ip, error)
                || !parseInteger(child, QStringLiteral("port"), 1, 65535, &remote.port, error))
                return {};
            converted.remoteCandidate = remote;
        } else if (name == QStringLiteral("gathering-complete") && child.namespaceURI().isEmpty()) {
            continue; // XEP-0371-only signal; XEP-0176 has no equivalent.
        } else if (!child.namespaceURI().isEmpty()) {
            converted.extensions.append(serializeOpaqueExtension(child));
        } else {
            fail(error, QStringLiteral("Unsupported internal ICE child for XEP-0176"));
            return {};
        }
    }

    return UdpTransportCodec::toXml(doc, converted, error);
}

} // namespace XMPP::Jingle::ICE
