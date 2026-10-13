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

namespace XMPP { namespace Jingle { namespace SXE {
    QString SxeOmemo::feature() { return QStringLiteral("urn:xmpp:sxe:0#omemo"); }

    SxeOmemo::SxeOmemo(XMPP::Jingle::Manager *manager, OmemoEncryption *omemo, QObject *parent) :
        QObject(parent), manager_(manager), omemo_(omemo)
    {
        if (auto transports = TransportManager::of(manager))
            transports->setPayloadProtector(this);
    }

    SxeOmemo::~SxeOmemo()
    {
        auto transports = TransportManager::of(manager_);
        if (transports && transports->payloadProtector() == this)
            transports->setPayloadProtector(nullptr);
    }

    QString SxeOmemo::method() const { return OmemoEncryption::namespaceUri(); }

    bool SxeOmemo::canProtect(const Jid &peer) const
    {
        return omemo_ && manager_ && omemo_->isReady() && omemo_->protocolsFor(peer).testFlag(OmemoProtocol::Omemo2)
            && manager_->client()->capsManager()->features(peer).test(feature());
    }

    QStringList SxeOmemo::discoFeatures() const
    {
        if (!omemo_ || !omemo_->supportedProtocols().testFlag(OmemoProtocol::Omemo2))
            return {};
        return { feature() };
    }

    void SxeOmemo::protect(const Jid &peer, const QDomElement &sxe, const Done &done)
    {
        if (!omemo_) {
            done({});
            return;
        }
        auto job    = omemo_->encryptElement(peer, OmemoProtocol::Omemo2, sxe);
        auto finish = [job, done]() {
            job->deleteLater();
            done(job->success() ? job->stanza() : QDomElement());
        };
        if (job->isFinished())
            finish();
        else
            connect(job, &EncryptionJob::finished, this, finish);
    }

    void SxeOmemo::unprotect(const Jid &from, const QDomElement &message, const Done &done)
    {
        QDomElement encrypted;
        for (auto child = message.firstChildElement(QStringLiteral("encrypted")); !child.isNull();
             child      = child.nextSiblingElement(QStringLiteral("encrypted")))
            if (child.namespaceURI() == OmemoEncryption::namespaceUri())
                encrypted = child;
        if (!omemo_ || encrypted.isNull()) {
            done({});
            return;
        }
        auto job    = omemo_->decryptElement(from, encrypted);
        auto finish = [job, done]() {
            job->deleteLater();
            done(job->success() ? job->stanza() : QDomElement());
        };
        if (job->isFinished())
            finish();
        else
            connect(job, &EncryptionJob::finished, this, finish);
    }
}}}
