// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0391 core is usable without any OMEMO implementation.
#include <iris/jingle-jet.h>
#include <iris/xmpp_client.h>

#include <QCoreApplication>
#include <utility>

using namespace XMPP;
namespace J = XMPP::Jingle;

namespace {
void check(bool value, const char *message)
{
    if (!value)
        qFatal("%s", message);
}

class TestKeyTransport : public J::JET::KeyTransport {
public:
    explicit TestKeyTransport(QString methodId, QString feature) :
        id_(std::move(methodId)), feature_(std::move(feature))
    {
    }

    QString     methodId() const override { return id_; }
    QStringList discoFeatures() const override { return { feature_ }; }
    bool        canEncrypt(const Jid &) const override { return false; }
    bool        accepts(const QString &profile) const override { return profile == feature_; }
    void        encryptSecret(const Jid &, const Done &done) override
    {
        done({}, QStringLiteral("No key exchange in this registry test"));
    }
    void decryptSecret(const Jid &, const QDomElement &, const Done &done) override
    {
        done({}, QStringLiteral("No key exchange in this registry test"));
    }

private:
    QString id_;
    QString feature_;
};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Client           client;
    auto            *manager = client.jingleManager();
    check(manager != nullptr, "Iris did not create a Jingle manager");
    auto *jet = manager->jet();
    check(jet != nullptr, "The Jingle manager did not create its JET registry");

    {
        check(jet->discoFeatures().isEmpty(), "JET advertised encryption without a registered key transport");
        check(!manager->discoFeatures().contains(J::JET::Manager::ns()),
              "The Jingle manager advertised JET with no provider");

        TestKeyTransport first(QStringLiteral("test-first"), QStringLiteral("urn:test:jet:first"));
        TestKeyTransport second(QStringLiteral("test-second"), QStringLiteral("urn:test:jet:second"));
        TestKeyTransport duplicate(QStringLiteral("test-first"), QStringLiteral("urn:test:jet:duplicate"));

        check(jet->registerKeyTransport(&first) && jet->registerKeyTransport(&first),
              "JET rejected a method's idempotent registration");
        check(!jet->registerKeyTransport(&duplicate), "JET replaced a registered encryption method");
        check(jet->registerKeyTransport(&second), "JET could not register a second encryption method");

        const auto advertised = jet->discoFeatures();
        check(advertised.contains(J::JET::Manager::ns()) && advertised.contains(QStringLiteral("urn:test:jet:first"))
                  && advertised.contains(QStringLiteral("urn:test:jet:second"))
                  && !advertised.contains(QStringLiteral("urn:test:jet:duplicate")),
              "JET advertised the wrong generic/protocol-specific features");

        check(manager->discoFeatures().contains(J::JET::Manager::ns())
                  && manager->discoFeatures().contains(QStringLiteral("urn:test:jet:first")),
              "The Jingle manager did not advertise registered JET providers");
        jet->unregisterKeyTransport(&first);
        check(jet->discoFeatures().contains(QStringLiteral("urn:test:jet:second"))
                  && !jet->discoFeatures().contains(QStringLiteral("urn:test:jet:first")),
              "JET could not unregister a single encryption method");
        jet->unregisterKeyTransport(&second);
        check(jet->discoFeatures().isEmpty() && !manager->discoFeatures().contains(J::JET::Manager::ns()),
              "JET advertised itself without a key transport");
    }

    check(manager->jet() == jet, "JET registry unexpectedly disappeared after provider removal");
    return 0;
}
