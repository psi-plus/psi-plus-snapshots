/*
 * Copyright (C) 2026  Sergei Ilinykh <rion4ik@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <QtCrypto>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class SecureFileUnitTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void roundTrip();
    void overwrite();
    void emptyFile();
    void sizeLimit();
    void rejectDirectory();
#ifndef Q_OS_WIN
    void rejectSymlink();
    void privatePermissions();
#endif

private:
    QCA::Initializer *m_init = nullptr;
};

void SecureFileUnitTest::initTestCase()
{
    m_init = new QCA::Initializer;
}

void SecureFileUnitTest::cleanupTestCase()
{
    delete m_init;
    m_init = nullptr;
}

void SecureFileUnitTest::roundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString    path = directory.filePath(QStringLiteral("secret.bin"));
    QCA::SecureArray expected(8);
    expected[0] = '\0';
    expected[1] = 's';
    expected[2] = 'e';
    expected[3] = 'c';
    expected[4] = 'r';
    expected[5] = 'e';
    expected[6] = 't';
    expected[7] = static_cast<char>(0xff);

    QCA::SecureFile file(path);
    QVERIFY(file.write(expected));
    QCOMPARE(file.error(), QCA::SecureFile::NoError);

    const QCA::SecureArray actual = file.read();
    QCOMPARE(file.error(), QCA::SecureFile::NoError);
    QVERIFY(actual == expected);
}

void SecureFileUnitTest::overwrite()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QCA::SecureFile file(directory.filePath(QStringLiteral("secret.bin")));
    QVERIFY(file.write(QCA::SecureArray("first secret")));
    QVERIFY(file.write(QCA::SecureArray("second secret")));

    const QCA::SecureArray actual = file.read();
    QCOMPARE(file.error(), QCA::SecureFile::NoError);
    QVERIFY(actual == QCA::SecureArray("second secret"));
}

void SecureFileUnitTest::emptyFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QCA::SecureFile file(directory.filePath(QStringLiteral("empty.bin")));
    QVERIFY(file.write(QCA::SecureArray()));

    const QCA::SecureArray actual = file.read();
    QVERIFY(actual.isEmpty());
    QCOMPARE(file.error(), QCA::SecureFile::NoError);
}

void SecureFileUnitTest::sizeLimit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString   path = directory.filePath(QStringLiteral("limited.bin"));
    QCA::SecureFile file(path);
    file.setMaximumSize(4);

    QVERIFY(!file.write(QCA::SecureArray("12345")));
    QCOMPARE(file.error(), QCA::SecureFile::TooLarge);

    QFile ordinary(path);
    QVERIFY(ordinary.open(QIODevice::WriteOnly));
    QCOMPARE(ordinary.write("12345", 5), qint64(5));
    ordinary.close();

    const QCA::SecureArray result = file.read();
    QVERIFY(result.isEmpty());
    QCOMPARE(file.error(), QCA::SecureFile::TooLarge);
}

void SecureFileUnitTest::rejectDirectory()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QCA::SecureFile        file(directory.path());
    const QCA::SecureArray result = file.read();
    QVERIFY(result.isEmpty());
    QCOMPARE(file.error(), QCA::SecureFile::InvalidFile);
    QVERIFY(!file.write(QCA::SecureArray("secret")));
    QCOMPARE(file.error(), QCA::SecureFile::InvalidFile);
}

#ifndef Q_OS_WIN
void SecureFileUnitTest::rejectSymlink()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString target = directory.filePath(QStringLiteral("target.bin"));
    QFile         ordinary(target);
    QVERIFY(ordinary.open(QIODevice::WriteOnly));
    QCOMPARE(ordinary.write("secret", 6), qint64(6));
    ordinary.close();

    const QString link = directory.filePath(QStringLiteral("link.bin"));
    QVERIFY(QFile::link(target, link));

    QCA::SecureFile        file(link);
    const QCA::SecureArray result = file.read();
    QVERIFY(result.isEmpty());
    QCOMPARE(file.error(), QCA::SecureFile::InvalidFile);
    QVERIFY(!file.write(QCA::SecureArray("replacement")));
    QCOMPARE(file.error(), QCA::SecureFile::InvalidFile);
}

void SecureFileUnitTest::privatePermissions()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString   path = directory.filePath(QStringLiteral("secret.bin"));
    QCA::SecureFile file(path);
    QVERIFY(file.write(QCA::SecureArray("secret")));

    const QFile::Permissions permissions = QFileInfo(path).permissions();
    QVERIFY(permissions.testFlag(QFile::ReadOwner));
    QVERIFY(permissions.testFlag(QFile::WriteOwner));
    QVERIFY(!permissions.testFlag(QFile::ReadGroup));
    QVERIFY(!permissions.testFlag(QFile::WriteGroup));
    QVERIFY(!permissions.testFlag(QFile::ReadOther));
    QVERIFY(!permissions.testFlag(QFile::WriteOther));
}
#endif

QTEST_GUILESS_MAIN(SecureFileUnitTest)
#include "securefileunittest.moc"
