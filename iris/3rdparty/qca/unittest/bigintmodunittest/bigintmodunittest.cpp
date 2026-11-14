#include <QTest>
#include <QtCrypto>

class BigIntegerModUnitTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void modularPower();
    void modularInverse();

private:
    QCA::Initializer *initializer_ = nullptr;
};

void BigIntegerModUnitTest::initTestCase()
{
    initializer_ = new QCA::Initializer;
}

void BigIntegerModUnitTest::cleanupTestCase()
{
    delete initializer_;
}

void BigIntegerModUnitTest::modularPower()
{
    QCOMPARE(QCA::BigIntegerMath::modPow(QCA::BigInteger(4), QCA::BigInteger(13), QCA::BigInteger(497)),
             QCA::BigInteger(445));
    QCOMPARE(QCA::BigIntegerMath::modPow(QCA::BigInteger(2), QCA::BigInteger(256), QCA::BigInteger(1000000007)),
             QCA::BigInteger(792845266));
    QCOMPARE(QCA::BigIntegerMath::modPow(QCA::BigInteger("12345678901234567890"),
                                         QCA::BigInteger(12345),
                                         QCA::BigInteger("98765432109876543210987654321")),
             QCA::BigInteger("46424977923491832562457887692"));
    QCOMPARE(QCA::BigIntegerMath::modPow(QCA::BigInteger(123), QCA::BigInteger(0), QCA::BigInteger(17)),
             QCA::BigInteger(1));
    QCOMPARE(QCA::BigIntegerMath::modPow(QCA::BigInteger(123), QCA::BigInteger(-1), QCA::BigInteger(17)),
             QCA::BigInteger(0));
}

void BigIntegerModUnitTest::modularInverse()
{
    QCA::BigInteger inverse;

    QVERIFY(QCA::BigIntegerMath::modInverse(QCA::BigInteger(3), QCA::BigInteger(11), &inverse));
    QCOMPARE(inverse, QCA::BigInteger(4));

    QVERIFY(QCA::BigIntegerMath::modInverse(QCA::BigInteger(-3), QCA::BigInteger(11), &inverse));
    QCOMPARE(inverse, QCA::BigInteger(7));

    QVERIFY(!QCA::BigIntegerMath::modInverse(QCA::BigInteger(6), QCA::BigInteger(9), &inverse));
    QVERIFY(!QCA::BigIntegerMath::modInverse(QCA::BigInteger(3), QCA::BigInteger(1), &inverse));
    QVERIFY(!QCA::BigIntegerMath::modInverse(QCA::BigInteger(3), QCA::BigInteger(11), nullptr));
}

QTEST_GUILESS_MAIN(BigIntegerModUnitTest)
#include "bigintmodunittest.moc"
