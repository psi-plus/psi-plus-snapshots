/*
 * jingle-sxe-omemo.cpp - SXE payload protected with OMEMO 2
 * Copyright (C) 2026  Sergey Ilinykh
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

#include "jingle-sxe-omemo.h"

#include "xmpp_caps.h"
#include "xmpp_client.h"
#include "xmpp_encryption.h"
#include "xmpp_omemo.h"

#include <QPointer>

namespace XMPP { namespace Jingle { namespace SXE {
    namespace {
        OmemoEncryption *method(XMPP::Jingle::Manager *manager)
        {
            return manager ? qobject_cast<OmemoEncryption *>(
                                 manager->client()->encryptionManager()->method(OmemoEncryption::methodId()))
                           : nullptr;
        }

        // Use the already selected Jingle encryption session. The synthetic message
        // is never sent: only its encrypted child travels next to the routing marker.
        void complete(EncryptionJob *job, EncryptedSession *session, QObject *receiver, const SxeOmemo::Done &done,
                      const QString &name, const QString &ns)
        {
            auto finish = [job, guard = QPointer<EncryptedSession>(session), receiver = QPointer<QObject>(receiver),
                           done, name, ns]() {
                QDomElement result;
                if (guard && !guard->isClosing() && job->success() && !job->metadata().protocolOnly) {
                    for (auto child = job->stanza().firstChildElement(name); !child.isNull();
                         child = child.nextSiblingElement(name)) {
                        if (child.namespaceURI() != ns)
                            continue;
                        if (!result.isNull()) {
                            result = {};
                            break;
                        }
                        result = child;
                    }
                }
                job->deleteLater();
                if (receiver)
                    done(result);
            };
            if (job->isFinished())
                finish();
            else
                QObject::connect(job, &EncryptionJob::finished, job, finish);
        }
    }

    QString SxeOmemo::feature() { return QStringLiteral("urn:xmpp:sxe:0#omemo"); }
    QString SxeOmemo::methodId() { return OmemoEncryption::methodId(); }
    QString SxeOmemo::wireProfile() { return OmemoEncryption::namespaceUri(); }

    bool SxeOmemo::isSupported(XMPP::Jingle::Manager *manager)
    {
        auto *omemo = method(manager);
        return omemo && omemo->supportedProtocols().testFlag(OmemoProtocol::Omemo2);
    }

    bool SxeOmemo::canProtect(Session *session)
    {
        auto *omemo = method(session->manager());
        return omemo && omemo->isReady() && omemo->protocolsFor(session->peer()).testFlag(OmemoProtocol::Omemo2)
            && session->checkPeerCaps(feature());
    }

    void SxeOmemo::protect(Session *session, const QDomElement &sxe, QObject *receiver, const Done &done)
    {
        auto *encryption = session->encryptionSession();
        if (!encryption || encryption->isClosing() || encryption->methodId() != methodId()) {
            done({});
            return;
        }
        QDomDocument document;
        auto wrapper = document.createElement(QStringLiteral("message"));
        wrapper.setAttribute(QStringLiteral("type"), QStringLiteral("chat"));
        wrapper.setAttribute(QStringLiteral("to"), session->peer().full());
        document.appendChild(wrapper);
        wrapper.appendChild(document.importNode(sxe, true));
        EncryptionContext operation;
        // Explicit OMEMO 2 disables legacy fallback; never encrypt transient edits to our other devices.
        operation.options.insert(QStringLiteral("omemoProtocol"), QStringLiteral("omemo2"));
        operation.options.insert(QStringLiteral("omemoOwnDevices"), false);
        complete(encryption->encrypt(wrapper, operation), encryption, receiver, done,
                 QStringLiteral("encrypted"), wireProfile());
    }

    void SxeOmemo::unprotect(Session *session, const Jid &from, const QDomElement &message, QObject *receiver,
                            const Done &done)
    {
        auto *encryption = session->encryptionSession();
        QDomElement encrypted;
        for (auto child = message.firstChildElement(QStringLiteral("encrypted")); !child.isNull();
             child = child.nextSiblingElement(QStringLiteral("encrypted"))) {
            if (child.namespaceURI() != wireProfile())
                continue;
            if (!encrypted.isNull()) {
                done({});
                return;
            }
            encrypted = child;
        }
        if (!encryption || encryption->isClosing() || encryption->methodId() != methodId() || encrypted.isNull()) {
            done({});
            return;
        }
        QDomDocument document;
        auto wrapper = document.createElement(QStringLiteral("message"));
        wrapper.setAttribute(QStringLiteral("type"), QStringLiteral("chat"));
        wrapper.setAttribute(QStringLiteral("from"), from.full());
        wrapper.setAttribute(QStringLiteral("to"), session->manager()->client()->jid().full());
        document.appendChild(wrapper);
        wrapper.appendChild(document.importNode(encrypted, true));
        complete(encryption->decrypt(wrapper), encryption, receiver, done,
                 QStringLiteral("sxe"), QStringLiteral("urn:xmpp:sxe:0"));
    }
}}}
