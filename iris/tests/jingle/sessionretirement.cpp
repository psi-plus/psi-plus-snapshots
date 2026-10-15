// SPDX-License-Identifier: LGPL-2.1-or-later
// A finished FT object is retained while another content runs, but cannot
// keep the session alive once its last live content finishes or is removed.
#define main iris_sxesession_main
#include "sxesession.cpp"
#undef main
#include "../../src/xmpp/xmpp-im/jingle-ibb.h"
#include <QBuffer>
#include <iris/jingle-ft.h>
#include <iris/xmpp_caps.h>

namespace FT = J::FileTransfer;

static void retireSession(J::Origin sender, bool cancel)
{
    Endpoint alice(QStringLiteral("alice")), bob(QStringLiteral("bob"));
    for (auto pair : { std::pair<Endpoint *, Endpoint *> { &alice, &bob }, { &bob, &alice } }) {
        DiscoItem disco;
        disco.setJid(pair.second->jid);
        disco.setNode(QStringLiteral("urn:iris:test:session-retirement"));
        disco.setFeatures(Features(QStringList { FT::NS, J::IBB::NS }));
        const CapsSpec caps(disco);
        CapsRegistry::instance()->registerCaps(caps, disco);
        pair.first->client.capsManager()->updateCaps(pair.second->jid, caps);
    }
    QPointer<J::Session>      local = alice.client.jingleManager()->newSession(bob.jid), remote;
    QPointer<FT::Application> first, tail, remoteFirst, remoteTail;
    FT::File                  file;
    file.setName(QStringLiteral("small.txt"));
    file.setSize(4);
    file.addHash(Hash::from(Hash::Sha256, QByteArray("data")));
    auto configure = [&](FT::Application *a, bool held) {
        QObject::connect(a, &FT::Application::deviceRequested, a, [a, held](quint64, std::optional<quint64>) {
            const bool sending = a->senders() == a->pad()->session()->role();
            if (held && sending)
                return;
            auto *device = new QBuffer(a);
            if (sending)
                device->setData("data");
            device->open(sending ? QIODevice::ReadOnly : QIODevice::WriteOnly);
            a->setDevice(device);
        });
    };
    QObject::connect(bob.client.jingleManager(), &J::Manager::incomingSession, &bob.client, [&](J::Session *session) {
        remote = session;
        for (auto *app : session->contentList()) {
            auto *a = qobject_cast<FT::Application *>(app);
            check(a, "unexpected content");
            const bool held                   = a->contentName() == tail->contentName();
            (held ? remoteTail : remoteFirst) = a;
            a->setAcceptFile(a->file());
            configure(a, held);
        }
        session->accept();
    });
    for (int i = 0; i < 2; ++i) {
        auto *a = qobject_cast<FT::Application *>(local->newContent(FT::NS, sender));
        check(a && a->setTransport(local->newOutgoingTransport(J::IBB::NS)), "IBB file transfer unavailable");
        (i ? tail : first) = a;
        a->setFile(file);
        configure(a, bool(i));
        local->addContent(a);
    }
    local->initiate();
    pump(alice, bob);
    check(local && remote && first && remoteFirst && tail && remoteTail, "initial session disappeared");
    check(first->state() == J::State::Finished && remoteFirst->state() == J::State::Finished,
          "first content did not finish");
    check(local->state() == J::State::Active && remote->state() == J::State::Active,
          "finished content terminated another active content");
    check(local->contentList().size() == 2 && remote->contentList().size() == 2,
          "fixture did not retain the finished content");

    if (cancel) {
        auto *receiver = sender == J::Origin::Initiator ? remoteTail.data() : tail.data();
        receiver->remove(J::Reason::Cancel, QStringLiteral("Media request cancelled"));
    } else {
        auto *source = sender == J::Origin::Initiator ? tail.data() : remoteTail.data();
        auto *device = new QBuffer(source);
        device->setData("data");
        device->open(QIODevice::ReadOnly);
        source->setDevice(device);
    }
    pump(alice, bob);
    check((!local || local->state() == J::State::Finished) && (!remote || remote->state() == J::State::Finished),
          "last content left the session active");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(!local && !remote && !first && !tail && !remoteFirst && !remoteTail,
          "terminal session or contents were not destroyed");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    for (auto sender : { J::Origin::Initiator, J::Origin::Responder })
        for (bool cancel : { false, true })
            retireSession(sender, cancel);
    qInfo("Jingle session retirement tests passed");
}
