// SPDX-License-Identifier: LGPL-2.1-or-later
#include "jingle-rtp-description.h"

#include <QSet>
#include <limits>

namespace XMPP::Jingle::RTP {
namespace {
    const QString NS_RTCP_FB(QStringLiteral("urn:xmpp:jingle:apps:rtp:rtcp-fb:0"));
    const QString NS_RTP_HDREXT(QStringLiteral("urn:xmpp:jingle:apps:rtp:rtp-hdrext:0"));
    const QString NS_SSMA(QStringLiteral("urn:xmpp:jingle:apps:rtp:ssma:0"));

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
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
        const bool parsed = owned.setContent(xml, true);
#else
        const auto parsed = owned.setContent(xml, QDomDocument::ParseOption::UseNamespaceProcessing);
#endif
        if (!parsed)
            return {};
        const auto element = owned.documentElement();
        return element.isNull() ? QDomElement() : target.importNode(element, true).toElement();
    }

    std::optional<quint32> number(const QString &text)
    {
        if (text.isEmpty())
            return {};
        for (auto c : text) {
            if (c < QLatin1Char('0') || c > QLatin1Char('9'))
                return {};
        }
        bool       ok    = false;
        const auto value = text.toULongLong(&ok);
        if (!ok || value > std::numeric_limits<quint32>::max())
            return {};
        return quint32(value);
    }

    std::optional<QList<ExtensionParameter>> parseExtensionParameters(const QDomElement &parent, const QString &ns)
    {
        QList<ExtensionParameter> result;
        for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            if (child.namespaceURI() != ns || child.localName() != QLatin1String("parameter"))
                return {};
            ExtensionParameter parameter;
            parameter.name = child.attribute(QStringLiteral("name"));
            if (parameter.name.isEmpty())
                return {};
            if (child.hasAttribute(QStringLiteral("value")))
                parameter.value = child.attribute(QStringLiteral("value"));
            result.append(std::move(parameter));
        }
        return result;
    }

    void appendExtensionParameters(QDomDocument &doc, QDomElement &parent, const QString &ns,
                                   const QList<ExtensionParameter> &parameters)
    {
        for (const auto &parameter : parameters) {
            auto child = doc.createElementNS(ns, QStringLiteral("parameter"));
            child.setAttribute(QStringLiteral("name"), parameter.name);
            if (parameter.value)
                child.setAttribute(QStringLiteral("value"), *parameter.value);
            parent.appendChild(child);
        }
    }

    std::optional<Feedback> parseFeedback(const QDomElement &element)
    {
        Feedback result;
        result.type = element.attribute(QStringLiteral("type"));
        if (result.type.isEmpty())
            return {};
        if (element.hasAttribute(QStringLiteral("subtype")))
            result.subtype = element.attribute(QStringLiteral("subtype"));
        auto parameters = parseExtensionParameters(element, NS_RTCP_FB);
        if (!parameters)
            return {};
        result.parameters = std::move(*parameters);
        return result;
    }

    std::optional<quint32> parseFeedbackTrrInt(const QDomElement &element)
    {
        if (!element.firstChildElement().isNull() || !element.hasAttribute(QStringLiteral("value")))
            return {};
        return number(element.attribute(QStringLiteral("value")));
    }

    QDomElement feedbackToXml(QDomDocument &doc, const Feedback &feedback)
    {
        auto element = doc.createElementNS(NS_RTCP_FB, QStringLiteral("rtcp-fb"));
        element.setAttribute(QStringLiteral("type"), feedback.type);
        if (!feedback.subtype.isEmpty())
            element.setAttribute(QStringLiteral("subtype"), feedback.subtype);
        appendExtensionParameters(doc, element, NS_RTCP_FB, feedback.parameters);
        return element;
    }

    QDomElement feedbackTrrIntToXml(QDomDocument &doc, quint32 value)
    {
        auto element = doc.createElementNS(NS_RTCP_FB, QStringLiteral("rtcp-fb-trr-int"));
        element.setAttribute(QStringLiteral("value"), QString::number(value));
        return element;
    }

    bool validHeaderExtensionId(quint32 id) { return (id >= 1 && id <= 256) || (id >= 4096 && id <= 4351); }

    std::optional<Origin> parseSenders(const QDomElement &element)
    {
        if (!element.hasAttribute(QStringLiteral("senders")))
            return Origin::Both;
        const auto value = element.attribute(QStringLiteral("senders"));
        if (value == QLatin1String("both"))
            return Origin::Both;
        if (value == QLatin1String("initiator"))
            return Origin::Initiator;
        if (value == QLatin1String("responder"))
            return Origin::Responder;
        if (value == QLatin1String("none"))
            return Origin::None;
        return {};
    }

    QString sendersToString(Origin senders)
    {
        switch (senders) {
        case Origin::Both:
            return QStringLiteral("both");
        case Origin::Initiator:
            return QStringLiteral("initiator");
        case Origin::Responder:
            return QStringLiteral("responder");
        case Origin::None:
            return QStringLiteral("none");
        }
        return {};
    }

    std::optional<HeaderExtension> parseHeaderExtension(const QDomElement &element)
    {
        const auto id = number(element.attribute(QStringLiteral("id")));
        if (!id || !validHeaderExtensionId(*id))
            return {};
        HeaderExtension result;
        result.id  = quint16(*id);
        result.uri = element.attribute(QStringLiteral("uri"));
        if (result.uri.isEmpty())
            return {};
        auto senders = parseSenders(element);
        if (!senders)
            return {};
        result.senders  = *senders;
        auto parameters = parseExtensionParameters(element, NS_RTP_HDREXT);
        if (!parameters)
            return {};
        result.parameters = std::move(*parameters);
        return result;
    }

    QDomElement headerExtensionToXml(QDomDocument &doc, const HeaderExtension &extension)
    {
        auto element = doc.createElementNS(NS_RTP_HDREXT, QStringLiteral("rtp-hdrext"));
        element.setAttribute(QStringLiteral("id"), QString::number(extension.id));
        element.setAttribute(QStringLiteral("uri"), extension.uri);
        if (extension.senders != Origin::Both)
            element.setAttribute(QStringLiteral("senders"), sendersToString(extension.senders));
        appendExtensionParameters(doc, element, NS_RTP_HDREXT, extension.parameters);
        return element;
    }

    std::optional<Source> parseSource(const QDomElement &element)
    {
        const auto ssrc = number(element.attribute(QStringLiteral("ssrc")));
        if (!ssrc)
            return {};
        Source result;
        result.ssrc     = *ssrc;
        auto parameters = parseExtensionParameters(element, NS_SSMA);
        if (!parameters)
            return {};
        result.parameters = std::move(*parameters);
        return result;
    }

    QDomElement sourceToXml(QDomDocument &doc, const Source &source)
    {
        auto element = doc.createElementNS(NS_SSMA, QStringLiteral("source"));
        element.setAttribute(QStringLiteral("ssrc"), QString::number(source.ssrc));
        appendExtensionParameters(doc, element, NS_SSMA, source.parameters);
        return element;
    }

    std::optional<SourceGroup> parseSourceGroup(const QDomElement &element)
    {
        SourceGroup result;
        result.semantics = element.attribute(QStringLiteral("semantics"));
        if (result.semantics.isEmpty())
            return {};
        QSet<quint32> seen;
        for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            if (child.namespaceURI() != NS_SSMA || child.localName() != QLatin1String("source")
                || !child.firstChildElement().isNull())
                return {};
            const auto ssrc = number(child.attribute(QStringLiteral("ssrc")));
            if (!ssrc || seen.contains(*ssrc))
                return {};
            seen.insert(*ssrc);
            result.sources.append(*ssrc);
        }
        return result;
    }

    QDomElement sourceGroupToXml(QDomDocument &doc, const SourceGroup &group)
    {
        auto element = doc.createElementNS(NS_SSMA, QStringLiteral("ssrc-group"));
        element.setAttribute(QStringLiteral("semantics"), group.semantics);
        for (auto ssrc : group.sources) {
            auto source = doc.createElementNS(NS_SSMA, QStringLiteral("source"));
            source.setAttribute(QStringLiteral("ssrc"), QString::number(ssrc));
            element.appendChild(source);
        }
        return element;
    }
}

QString Description::ns() { return QStringLiteral("urn:xmpp:jingle:apps:rtp:1"); }

std::optional<Description> Description::fromXml(const QDomElement &element, bool advisory)
{
    if (element.namespaceURI() != ns() || element.localName() != QLatin1String("description"))
        return {};
    Description result;
    result.media = element.attribute(QStringLiteral("media"));
    if (result.media.isEmpty())
        return {};
    if (element.hasAttribute(QStringLiteral("ssrc"))) {
        result.ssrc = number(element.attribute(QStringLiteral("ssrc")));
        if (!result.ssrc)
            return {};
    }

    QSet<int>     payloadIds;
    QSet<quint32> sourceIds;
    for (auto child = element.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.namespaceURI() == ns() && child.localName() == QLatin1String("rtcp-mux")) {
            if (result.rtcpMux)
                return {};
            result.rtcpMux = true;
        } else if (child.namespaceURI() == ns() && child.localName() == QLatin1String("payload-type")) {
            PayloadType payload;
            const auto  id = number(child.attribute(QStringLiteral("id")));
            if (!id || *id > 127 || payloadIds.contains(int(*id)))
                return {};
            payloadIds.insert(int(*id));
            payload.id   = quint8(*id);
            payload.name = child.attribute(QStringLiteral("name"));
            if (!advisory && payload.id >= 96 && payload.name.isEmpty())
                return {};
            if (child.hasAttribute(QStringLiteral("channels"))) {
                const auto channels = number(child.attribute(QStringLiteral("channels")));
                if (!channels || *channels == 0 || *channels > 255)
                    return {};
                payload.channels = quint8(*channels);
            }
            for (auto entry : { qMakePair(QStringLiteral("clockrate"), &payload.clockrate),
                                qMakePair(QStringLiteral("ptime"), &payload.ptime),
                                qMakePair(QStringLiteral("maxptime"), &payload.maxptime) }) {
                if (child.hasAttribute(entry.first)) {
                    *entry.second = number(child.attribute(entry.first));
                    if (!*entry.second)
                        return {};
                }
            }
            for (auto param = child.firstChildElement(); !param.isNull(); param = param.nextSiblingElement()) {
                if (param.namespaceURI() == ns() && param.localName() == QLatin1String("parameter")) {
                    const auto name = param.attribute(QStringLiteral("name"));
                    if (name.isEmpty() || payload.parameters.contains(name)
                        || !param.hasAttribute(QStringLiteral("value")))
                        return {};
                    payload.parameters.insert(name, param.attribute(QStringLiteral("value")));
                } else if (param.namespaceURI() == NS_RTCP_FB && param.localName() == QLatin1String("rtcp-fb")) {
                    auto feedback = parseFeedback(param);
                    if (!feedback)
                        return {};
                    payload.feedback.append(std::move(*feedback));
                } else if (param.namespaceURI() == NS_RTCP_FB
                           && param.localName() == QLatin1String("rtcp-fb-trr-int")) {
                    if (payload.feedbackTrrInt)
                        return {};
                    payload.feedbackTrrInt = parseFeedbackTrrInt(param);
                    if (!payload.feedbackTrrInt)
                        return {};
                } else {
                    payload.extensions.append(serializeOpaqueExtension(param));
                }
            }
            result.payloads.append(std::move(payload));
        } else if (child.namespaceURI() == NS_RTCP_FB && child.localName() == QLatin1String("rtcp-fb")) {
            auto feedback = parseFeedback(child);
            if (!feedback)
                return {};
            result.feedback.append(std::move(*feedback));
        } else if (child.namespaceURI() == NS_RTCP_FB && child.localName() == QLatin1String("rtcp-fb-trr-int")) {
            if (result.feedbackTrrInt)
                return {};
            result.feedbackTrrInt = parseFeedbackTrrInt(child);
            if (!result.feedbackTrrInt)
                return {};
        } else if (child.namespaceURI() == NS_RTP_HDREXT && child.localName() == QLatin1String("rtp-hdrext")) {
            auto extension = parseHeaderExtension(child);
            if (!extension)
                return {};
            result.headerExtensions.append(std::move(*extension));
        } else if (child.namespaceURI() == NS_RTP_HDREXT && child.localName() == QLatin1String("extmap-allow-mixed")) {
            if (result.extmapAllowMixed || !child.firstChildElement().isNull())
                return {};
            result.extmapAllowMixed = true;
        } else if (child.namespaceURI() == NS_SSMA && child.localName() == QLatin1String("source")) {
            auto source = parseSource(child);
            if (!source || sourceIds.contains(source->ssrc))
                return {};
            sourceIds.insert(source->ssrc);
            result.sources.append(std::move(*source));
        } else if (child.namespaceURI() == NS_SSMA && child.localName() == QLatin1String("ssrc-group")) {
            auto group = parseSourceGroup(child);
            if (!group)
                return {};
            result.sourceGroups.append(std::move(*group));
        } else {
            result.extensions.append(serializeOpaqueExtension(child));
        }
    }
    if (!advisory && result.payloads.isEmpty())
        return {};
    return result;
}

QDomElement Description::toXml(QDomDocument &doc) const
{
    auto root = doc.createElementNS(ns(), QStringLiteral("description"));
    root.setAttribute(QStringLiteral("media"), media);
    if (ssrc)
        root.setAttribute(QStringLiteral("ssrc"), QString::number(*ssrc));

    for (const auto &entry : feedback)
        root.appendChild(feedbackToXml(doc, entry));
    if (feedbackTrrInt)
        root.appendChild(feedbackTrrIntToXml(doc, *feedbackTrrInt));

    for (const auto &payload : payloads) {
        auto child = doc.createElementNS(ns(), QStringLiteral("payload-type"));
        child.setAttribute(QStringLiteral("id"), int(payload.id));
        if (!payload.name.isEmpty())
            child.setAttribute(QStringLiteral("name"), payload.name);
        if (payload.channels)
            child.setAttribute(QStringLiteral("channels"), int(*payload.channels));
        for (const auto &entry : { qMakePair(QStringLiteral("clockrate"), payload.clockrate),
                                   qMakePair(QStringLiteral("ptime"), payload.ptime),
                                   qMakePair(QStringLiteral("maxptime"), payload.maxptime) }) {
            if (entry.second)
                child.setAttribute(entry.first, QString::number(*entry.second));
        }
        for (auto it = payload.parameters.cbegin(); it != payload.parameters.cend(); ++it) {
            auto param = doc.createElementNS(ns(), QStringLiteral("parameter"));
            param.setAttribute(QStringLiteral("name"), it.key());
            param.setAttribute(QStringLiteral("value"), it.value());
            child.appendChild(param);
        }
        for (const auto &entry : payload.feedback)
            child.appendChild(feedbackToXml(doc, entry));
        if (payload.feedbackTrrInt)
            child.appendChild(feedbackTrrIntToXml(doc, *payload.feedbackTrrInt));
        for (const auto &extension : payload.extensions) {
            const auto element = deserializeOpaqueExtension(doc, extension);
            if (!element.isNull())
                child.appendChild(element);
        }
        root.appendChild(child);
    }
    if (rtcpMux)
        root.appendChild(doc.createElementNS(ns(), QStringLiteral("rtcp-mux")));
    for (const auto &group : sourceGroups)
        root.appendChild(sourceGroupToXml(doc, group));
    for (const auto &source : sources)
        root.appendChild(sourceToXml(doc, source));
    for (const auto &extension : headerExtensions)
        root.appendChild(headerExtensionToXml(doc, extension));
    if (extmapAllowMixed)
        root.appendChild(doc.createElementNS(NS_RTP_HDREXT, QStringLiteral("extmap-allow-mixed")));
    for (const auto &extension : extensions) {
        const auto element = deserializeOpaqueExtension(doc, extension);
        if (!element.isNull())
            root.appendChild(element);
    }
    return root;
}
}
