/*
 * ndns.h - native DNS resolution
 * Copyright (C) 2001-2002  Justin Karneges
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

#ifndef CS_NDNS_H
#define CS_NDNS_H

#include "netnames.h"

#include <QtCore>
#include <QtNetwork>

// CS_NAMESPACE_BEGIN
class NDns : public QObject {
    Q_OBJECT
public:
    NDns(QObject *parent = nullptr);
    ~NDns();

    void resolve(const QString &);
    void stop();
    bool isBusy() const;

    QHostAddress result() const;
    QString      resultString() const;

signals:
    void resultsReady();

private slots:
    void dns_resultsReady(const QList<XMPP::NameRecord> &);
    void dns_error(XMPP::NameResolver::Error);

private:
    XMPP::NameResolver dns;
    bool               busy;
    QHostAddress       addr;
};

// CS_NAMESPACE_END

#endif // CS_NDNS_H
