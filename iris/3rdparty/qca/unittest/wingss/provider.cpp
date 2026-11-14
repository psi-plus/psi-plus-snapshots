// SPDX-License-Identifier: LGPL-2.1-or-later
#include <QSignalSpy>
#include <QTest>
#include <QtCrypto>
#ifdef QT_STATICPLUGIN
#include "import_plugins.h"
#endif

class WinGssProviderTest : public QObject
{
    Q_OBJECT
private:
    QCA::Initializer initializer;
private Q_SLOTS:
    void available()
    {
        QVERIFY(QCA::isSupported("sasl", QStringLiteral("qca-wingss")));
    }

    void unsupportedMechanism()
    {
        QCA::SASL  sasl(nullptr, QStringLiteral("qca-wingss"));
        QSignalSpy errors(&sasl, &QCA::SASL::error);
        sasl.startClient(QStringLiteral("service"),
                         QStringLiteral("host.test"),
                         QStringList {QStringLiteral("unsupported-test-mechanism")});
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(sasl.authCondition(), QCA::SASL::NoMechanism);
    }

    void impossibleConstraints()
    {
        QCA::SASL sasl(nullptr, QStringLiteral("qca-wingss"));
        sasl.setConstraints(QCA::SASL::RequireForwardSecrecy);
        QSignalSpy errors(&sasl, &QCA::SASL::error);
        sasl.startClient(
            QStringLiteral("service"), QStringLiteral("host.test"), QStringList {QStringLiteral("GSSAPI")});
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(sasl.authCondition(), QCA::SASL::NoMechanism);
    }

    void serverWithoutDelegate()
    {
        for (QCA::Provider *provider : QCA::providers()) {
            if (provider->name() != QStringLiteral("qca-wingss") &&
                provider->features().contains(QStringLiteral("sasl")))
                QSKIP("This case requires the SSPI provider without another SASL provider");
        }
        QCA::SASL  sasl(nullptr, QStringLiteral("qca-wingss"));
        QSignalSpy errors(&sasl, &QCA::SASL::error);
        sasl.startServer(QStringLiteral("service"), QStringLiteral("host.test"), QStringLiteral("realm"));
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(sasl.authCondition(), QCA::SASL::NoMechanism);
    }
};

QTEST_GUILESS_MAIN(WinGssProviderTest)
#include "provider.moc"
