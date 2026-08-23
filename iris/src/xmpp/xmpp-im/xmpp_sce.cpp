/*
 * xmpp_sce.cpp - XEP-0420 Stanza Content Encryption helper
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "xmpp_sce.h"

#include "xmpp/jid/jid.h"
#include "xmpp_xmlcommon.h"

#include <QtCrypto>

#include <QDateTime>
#include <QDomNamedNodeMap>
#include <QSet>

#include <algorithm>

namespace XMPP {
namespace {
    constexpr auto SceNs     = "urn:xmpp:sce:1";
    constexpr auto ClientNs  = "jabber:client";
    constexpr auto HintsNs   = "urn:xmpp:hints";
    constexpr auto SidNs     = "urn:xmpp:sid:0";
    constexpr auto AddressNs = "http://jabber.org/protocol/address";
    constexpr auto EmeNs     = "urn:xmpp:eme:0";

    QString localName(const QDomElement &element)
    {
        return element.localName().isEmpty() ? element.tagName().section(QLatin1Char(':'), -1) : element.localName();
    }

    void setError(QString *error, const QString &text)
    {
        if (error)
            *error = text;
    }

    bool validStanzaRoot(const QDomElement &stanza)
    {
        const auto name = localName(stanza);
        return name == QLatin1String("message") || name == QLatin1String("iq") || name == QLatin1String("presence");
    }

    void copyAttributes(const QDomElement &source, QDomElement &target)
    {
        const auto attrs = source.attributes();
        for (int i = 0; i < attrs.count(); ++i) {
            const auto attr = attrs.item(i).toAttr();
            if (attr.isNull() || attr.name() == QLatin1String("xmlns"))
                continue;
            if (!attr.namespaceURI().isEmpty())
                target.setAttributeNS(attr.namespaceURI(), attr.name(), attr.value());
            else
                target.setAttribute(attr.name(), attr.value());
        }
    }

    QDomElement createStanzaRoot(QDomDocument &document, const QDomElement &source)
    {
        const QString ns   = source.namespaceURI().isEmpty() ? QLatin1String(ClientNs) : source.namespaceURI();
        auto          root = document.createElementNS(ns, source.tagName());
        copyAttributes(source, root);
        document.appendChild(root);
        return root;
    }

    QDomElement normalizedElement(const QDomElement &source)
    {
        if (!source.namespaceURI().isEmpty())
            return source;
        return addCorrectNS(source);
    }

    QString bareJidAttribute(const QDomElement &stanza, const QString &name)
    {
        const auto value = stanza.attribute(name);
        if (value.isEmpty())
            return {};
        const Jid jid(value);
        return jid.isValid() ? jid.bare() : QString();
    }

    QDomElement directChildNS(const QDomElement &parent, const QString &name, const QString &ns)
    {
        for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            if (localName(child) == name && child.namespaceURI() == ns)
                return child;
        }
        return {};
    }

    int directChildCountNS(const QDomElement &parent, const QString &name, const QString &ns)
    {
        int count = 0;
        for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
            if (localName(child) == name && child.namespaceURI() == ns)
                ++count;
        }
        return count;
    }

    QString randomPadding(int length)
    {
        if (length <= 0)
            return {};

        // XEP-0420 no longer restricts the alphabet. Hex keeps the serialized
        // representation single-byte/ASCII while all entropy still comes from QCA.
        const int bytes = (length + 1) / 2;
        return QString::fromLatin1(QCA::Random::randomArray(bytes).toByteArray().toHex().left(length));
    }

    int randomBoundedInclusive(int maximum)
    {
        if (maximum <= 0)
            return 0;
        const auto bytes = QCA::Random::randomArray(4).toByteArray();
        if (bytes.size() != 4)
            return 0;
        quint32 value = 0;
        for (const auto byte : bytes)
            value = (value << 8) | static_cast<quint8>(byte);
        return static_cast<int>(value % static_cast<quint32>(maximum + 1));
    }

    bool verifyAddressAffix(const QDomElement &outer, const QDomElement &envelope, const QString &affixName,
                            const QString &attributeName, bool required, QString *error)
    {
        const auto count = directChildCountNS(envelope, affixName, QLatin1String(SceNs));
        if (count > 1) {
            setError(error, QStringLiteral("SCE envelope contains more than one <%1/> affix").arg(affixName));
            return false;
        }
        if (count == 0) {
            if (required) {
                setError(error, QStringLiteral("SCE envelope is missing required <%1/> affix").arg(affixName));
                return false;
            }
            return true;
        }

        const auto affix  = directChildNS(envelope, affixName, QLatin1String(SceNs));
        const auto actual = bareJidAttribute(outer, attributeName);
        const Jid  expectedJid(affix.attribute(QStringLiteral("jid")));
        if (!expectedJid.isValid() || actual.isEmpty() || expectedJid.bare() != actual) {
            setError(error, QStringLiteral("SCE <%1/> affix does not match outer stanza").arg(affixName));
            return false;
        }
        return true;
    }

    bool verifyTimeAffix(const QDomElement &envelope, const StanzaContentEncryption::Profile &profile, QString *error)
    {
        const auto count = directChildCountNS(envelope, QStringLiteral("time"), QLatin1String(SceNs));
        if (count > 1) {
            setError(error, QStringLiteral("SCE envelope contains more than one <time/> affix"));
            return false;
        }
        if (!profile.affixes.testFlag(StanzaContentEncryption::Time))
            return true;
        if (count != 1) {
            setError(error, QStringLiteral("SCE envelope is missing required <time/> affix"));
            return false;
        }

        const auto stamp
            = directChildNS(envelope, QStringLiteral("time"), QLatin1String(SceNs)).attribute(QStringLiteral("stamp"));
        const auto dateTime = QDateTime::fromString(stamp, Qt::ISODate);
        if (!dateTime.isValid()) {
            setError(error, QStringLiteral("SCE <time/> affix contains an invalid XEP-0082 timestamp"));
            return false;
        }
        if (profile.maximumClockSkewSeconds > 0) {
            const auto skew = qAbs(dateTime.toUTC().secsTo(QDateTime::currentDateTimeUtc()));
            if (skew > profile.maximumClockSkewSeconds) {
                setError(error, QStringLiteral("SCE <time/> affix exceeds the configured clock-skew policy"));
                return false;
            }
        }
        return true;
    }

} // namespace

QString StanzaContentEncryption::namespaceUri() { return QString::fromLatin1(SceNs); }

bool StanzaContentEncryption::isServerProcessedElement(const QDomElement &element)
{
    if (element.isNull())
        return false;

    const auto ns   = element.namespaceURI();
    const auto name = localName(element);
    if (ns == QLatin1String(HintsNs))
        return true;
    if (ns == QLatin1String(EmeNs))
        return true;
    if (ns == QLatin1String(SidNs) && name == QLatin1String("stanza-id"))
        return true; // origin-id is intentionally encryptable.
    if (ns == QLatin1String(AddressNs) && (name == QLatin1String("addresses") || name == QLatin1String("address")))
        return true;
    return false;
}

std::optional<StanzaContentEncryption::Prepared>
StanzaContentEncryption::prepare(const QDomElement &stanza, const Profile &profile, QString *error)
{
    if (stanza.isNull() || !validStanzaRoot(stanza)) {
        setError(error, QStringLiteral("SCE can only prepare an XMPP message, IQ or presence stanza"));
        return std::nullopt;
    }

    Prepared prepared;
    auto     outer = createStanzaRoot(prepared.outerDocument, stanza);

    auto envelope = prepared.envelopeDocument.createElementNS(QLatin1String(SceNs), QStringLiteral("envelope"));
    prepared.envelopeDocument.appendChild(envelope);
    auto content = prepared.envelopeDocument.createElementNS(QLatin1String(SceNs), QStringLiteral("content"));
    envelope.appendChild(content);

    for (auto child = stanza.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        const auto normalized = normalizedElement(child);
        if (isServerProcessedElement(normalized))
            outer.appendChild(prepared.outerDocument.importNode(normalized, true));
        else
            content.appendChild(prepared.envelopeDocument.importNode(normalized, true));
    }

    if (profile.affixes.testFlag(RandomPadding)) {
        auto rpad = prepared.envelopeDocument.createElementNS(QLatin1String(SceNs), QStringLiteral("rpad"));
        envelope.appendChild(rpad);

        // First meet the profile's fixed target, then add the XEP-defined
        // independent random extra length. Recompute after adding the rpad tag
        // itself so the target refers to serialized envelope size.
        const int currentSize = prepared.envelopeDocument.toByteArray(-1).size();
        const int fixed       = qMax(0, profile.minimumEnvelopeSize - currentSize);
        const int randomExtra = randomBoundedInclusive(qMax(0, profile.maximumRandomPadding));
        rpad.appendChild(prepared.envelopeDocument.createTextNode(randomPadding(fixed + randomExtra)));
    }

    if (profile.affixes.testFlag(Time)) {
        auto time = prepared.envelopeDocument.createElementNS(QLatin1String(SceNs), QStringLiteral("time"));
        time.setAttribute(QStringLiteral("stamp"),
                          QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'")));
        envelope.appendChild(time);
    }

    if (profile.affixes.testFlag(To)) {
        const auto jid = bareJidAttribute(stanza, QStringLiteral("to"));
        if (jid.isEmpty()) {
            setError(error, QStringLiteral("SCE profile requires <to/> but stanza has no valid to JID"));
            return std::nullopt;
        }
        auto to = prepared.envelopeDocument.createElementNS(QLatin1String(SceNs), QStringLiteral("to"));
        to.setAttribute(QStringLiteral("jid"), jid);
        envelope.appendChild(to);
    }

    if (profile.affixes.testFlag(From)) {
        const auto jid = bareJidAttribute(stanza, QStringLiteral("from"));
        if (jid.isEmpty()) {
            setError(error, QStringLiteral("SCE profile requires <from/> but stanza has no valid from JID"));
            return std::nullopt;
        }
        auto from = prepared.envelopeDocument.createElementNS(QLatin1String(SceNs), QStringLiteral("from"));
        from.setAttribute(QStringLiteral("jid"), jid);
        envelope.appendChild(from);
    }

    return prepared;
}

std::optional<QDomDocument> StanzaContentEncryption::restore(const QDomElement &encryptedOuter,
                                                             const QDomElement &envelope, const Profile &profile,
                                                             QString *error)
{
    if (encryptedOuter.isNull() || !validStanzaRoot(encryptedOuter)) {
        setError(error, QStringLiteral("SCE outer element is not a valid stanza"));
        return std::nullopt;
    }
    if (envelope.isNull() || localName(envelope) != QLatin1String("envelope")
        || envelope.namespaceURI() != QLatin1String(SceNs)) {
        setError(error, QStringLiteral("Decrypted SCE payload is not an urn:xmpp:sce:1 envelope"));
        return std::nullopt;
    }
    if (directChildCountNS(envelope, QStringLiteral("content"), QLatin1String(SceNs)) != 1) {
        setError(error, QStringLiteral("SCE envelope must contain exactly one <content/> element"));
        return std::nullopt;
    }
    if (profile.affixes.testFlag(RandomPadding)
        && directChildCountNS(envelope, QStringLiteral("rpad"), QLatin1String(SceNs)) != 1) {
        setError(error, QStringLiteral("SCE profile requires exactly one <rpad/> affix"));
        return std::nullopt;
    }
    if (!verifyAddressAffix(encryptedOuter, envelope, QStringLiteral("to"), QStringLiteral("to"),
                            profile.affixes.testFlag(To), error)
        || !verifyAddressAffix(encryptedOuter, envelope, QStringLiteral("from"), QStringLiteral("from"),
                               profile.affixes.testFlag(From), error)
        || !verifyTimeAffix(envelope, profile, error)) {
        return std::nullopt;
    }

    QDomDocument result;
    auto         root = createStanzaRoot(result, encryptedOuter);

    // Only elements that must remain server-visible survive from the outer
    // stanza. This deliberately drops attacker-injected sensitive plaintext.
    for (auto child = encryptedOuter.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        const auto normalized = normalizedElement(child);
        if (isServerProcessedElement(normalized))
            root.appendChild(result.importNode(normalized, true));
    }

    const auto content = directChildNS(envelope, QStringLiteral("content"), QLatin1String(SceNs));
    for (auto child = content.firstChildElement(); !child.isNull(); child = child.nextSiblingElement()) {
        if (child.namespaceURI().isEmpty())
            continue; // XEP-0420 requires name+namespace identification.
        if (isServerProcessedElement(child))
            continue; // Never trust server-only elements from encrypted content.
        root.appendChild(result.importNode(child, true));
    }

    return result;
}

} // namespace XMPP
