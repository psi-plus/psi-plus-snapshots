/*
 * Copyright (C) 2026 Psi contributors
 * SPDX-License-Identifier: MIT
 */
#include "qjdnsmulticast.h"
#include "qjdnsshared_p.h"
#include <QRegularExpression>

class QJDnsMulticastPrivate
{
public:
    QJDnsShared *backend = nullptr;
    QNetworkInterface interface;
    QList<QHostAddress> addresses;
    bool closing = false;
};

namespace {
bool linkLocal6(const QHostAddress &address)
{
    if(address.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    const auto bytes = address.toIPv6Address();
    return bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80;
}

bool usable(const QHostAddress &address)
{
    return !address.isNull() && !address.isMulticast()
        && address != QHostAddress::Any && address != QHostAddress::AnyIPv4
        && address != QHostAddress::AnyIPv6;
}

QList<QHostAddress> interfaceAddresses(const QNetworkInterface &interface)
{
    QList<QHostAddress> addresses;
    for(const auto &entry : interface.addressEntries())
    {
        const auto address = entry.ip();
        if(!usable(address))
            continue;
        // Link-local IPv6 works even without a globally routable IPv6 address.
        if(linkLocal6(address))
            addresses.prepend(address);
        else
            addresses.append(address);
    }
    return addresses;
}
}

QJDnsMulticast::QJDnsMulticast(QObject *parent) : QObject(parent), d(new QJDnsMulticastPrivate)
{
    d->backend = new QJDnsShared(QJDnsShared::Multicast, this);
    connect(d->backend, &QJDnsShared::shutdownFinished, this, &QJDnsMulticast::shutdownFinished);
}

QJDnsMulticast::~QJDnsMulticast()
{
    delete d->backend;
    delete d;
}

QJDnsMulticast *QJDnsMulticast::createOn(const QNetworkInterface &interface,
                                       const QList<QHostAddress> &addresses, QObject *parent)
{
    if(!interface.isValid() || !(interface.flags() & QNetworkInterface::IsUp))
        return nullptr;
    auto *session = new QJDnsMulticast(parent);
    session->d->interface = interface;
    bool have4 = false, have6 = false;
    for(const auto &address : addresses)
    {
        const bool ipv6 = address.protocol() == QAbstractSocket::IPv6Protocol;
        if((ipv6 ? have6 : have4) || !usable(address))
            continue;
        if(session->d->backend->d->addInterface(address, interface))
        {
            session->d->addresses.append(address);
            (ipv6 ? have6 : have4) = true;
        }
    }
    if(session->d->addresses.isEmpty())
    {
        delete session;
        return nullptr;
    }
    return session;
}

QJDnsMulticast *QJDnsMulticast::create(const QNetworkInterface &interface, QObject *parent)
{
    return createOn(interface, interfaceAddresses(interface), parent);
}

QJDnsMulticast *QJDnsMulticast::create(const QHostAddress &address, QObject *parent)
{
    if(!usable(address))
        return nullptr;
    QNetworkInterface selected;
    QHostAddress selectedAddress;
    for(const auto &interface : QNetworkInterface::allInterfaces())
    {
        for(const auto &candidate : interfaceAddresses(interface))
        {
            // Scope is significant for IPv6 link-local addresses. An unscoped
            // address is accepted only when it identifies exactly one link.
            auto unscoped = candidate;
            unscoped.setScopeId(QString());
            auto asked = address;
            asked.setScopeId(QString());
            const auto scope = address.scopeId();
            if(unscoped != asked || (!scope.isEmpty() && scope != interface.name()
                                    && scope != QString::number(interface.index())))
                continue;
            if(selected.isValid() && selected.index() != interface.index())
                return nullptr; // ambiguous link-local address requires a scope
            selected = interface;
            selectedAddress = candidate;
        }
    }
    return selected.isValid() ? createOn(selected, {selectedAddress}, parent) : nullptr;
}

QNetworkInterface QJDnsMulticast::networkInterface() const { return d->interface; }
QList<QHostAddress> QJDnsMulticast::addresses() const { return d->addresses; }

namespace {
QByteArray serviceSuffix(QByteArray type)
{
    type = type.toLower();
    if(type.endsWith(".local."))
        type.chop(7);
    static const QRegularExpression syntax(QStringLiteral("^_[a-z0-9](?:[a-z0-9-]{0,13}[a-z0-9])?\\._(tcp|udp)$"));
    if(!syntax.match(QString::fromLatin1(type)).hasMatch())
        return {};
    const auto label = type.left(type.indexOf('.'));
    if(!QRegularExpression(QStringLiteral("[a-z]")).match(QString::fromLatin1(label)).hasMatch())
        return {};
    return "." + type + ".local.";
}
}

QByteArray QJDnsMulticast::serviceName(const QString &instance, const QByteArray &serviceType)
{
    const auto suffix = serviceSuffix(serviceType);
    const auto normalized = instance.normalized(QString::NormalizationForm_C);
    const auto label = normalized.toUtf8();
    if(suffix.isEmpty() || label.isEmpty() || label.size() > 63 || QString::fromUtf8(label) != normalized)
        return {};
    QByteArray escaped;
    for(char c : label)
    {
        if(uchar(c) < 32 || c == 127)
            return {};
        if(c == '.' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    return escaped + suffix;
}

QString QJDnsMulticast::instanceName(const QByteArray &name, const QByteArray &serviceType)
{
    const auto suffix = serviceSuffix(serviceType);
    if(suffix.isEmpty())
        return {};
    QByteArray label;
    int at = 0;
    while(at < name.size())
    {
        char c = name.at(at++);
        if(c == '.')
        {
            if(name.mid(at - 1).toLower() != suffix)
                return {};
            const auto instance = QString::fromUtf8(label);
            return !serviceName(instance, serviceType).isEmpty() && instance.toUtf8() == label ? instance : QString();
        }
        if(c == '\\')
        {
            if(at >= name.size())
                return {};
            c = name.at(at++);
            if(c >= '0' && c <= '9')
            {
                if(at + 1 >= name.size() || name.at(at) < '0' || name.at(at) > '9'
                   || name.at(at + 1) < '0' || name.at(at + 1) > '9')
                    return {};
                const int value = (c - '0') * 100 + (name.at(at) - '0') * 10 + name.at(at + 1) - '0';
                if(value > 255)
                    return {};
                c = char(value);
                at += 2;
            }
        }
        if(uchar(c) < 32 || c == 127 || label.size() >= 63)
            return {};
        label += c;
    }
    return {};
}

QJDnsSharedRequest *QJDnsMulticast::subscribe(const QByteArray &name, int type, QObject *parent)
{
    auto *request = new QJDnsSharedRequest(d->backend, parent ? parent : this);
    request->d->deferred = true;
    request->query(name, type);
    if(d->closing)
        request->d->failShared(QJDnsSharedRequest::ErrorNoNet);
    return request;
}

QJDnsSharedRequest *QJDnsMulticast::publish(const QJDns::Record &record, QJDns::PublishMode mode,
                                         QObject *parent)
{
    auto *request = new QJDnsSharedRequest(d->backend, parent ? parent : this);
    request->d->deferred = true;
    request->publish(mode, record);
    if(d->closing)
        request->d->failShared(QJDnsSharedRequest::ErrorNoNet);
    return request;
}

bool QJDnsMulticast::unpublish(QJDnsSharedRequest *publication)
{
    if(!publication || publication->d->jsp != d->backend->d
       || publication->type() != QJDnsSharedRequest::Publish)
        return false;
    publication->cancel();
    return true;
}

void QJDnsMulticast::shutdown()
{
    if(d->closing)
        return;
    d->closing = true;
    d->backend->shutdown();
}
