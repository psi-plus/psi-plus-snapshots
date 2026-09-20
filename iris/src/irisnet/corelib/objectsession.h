/*
 * Copyright (C) 2008  Justin Karneges
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

#ifndef OBJECTSESSION_H
#define OBJECTSESSION_H

#include <QObject>

namespace XMPP {
class ObjectSessionPrivate;
class ObjectSessionWatcherPrivate;

#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
using ObjectSessionArgument = QGenericArgument;
#else
using ObjectSessionArgument = QMetaMethodArgument;
#endif

class ObjectSession : public QObject {
    Q_OBJECT

public:
    ObjectSession(QObject *parent = nullptr);
    ~ObjectSession();

    // clear all deferred requests, invalidate watchers
    void reset();

    bool isDeferred(QObject *obj, const char *method);
    void
         defer(QObject *obj, const char *method, ObjectSessionArgument val0 = ObjectSessionArgument(),
               ObjectSessionArgument val1 = ObjectSessionArgument(), ObjectSessionArgument val2 = ObjectSessionArgument(),
               ObjectSessionArgument val3 = ObjectSessionArgument(), ObjectSessionArgument val4 = ObjectSessionArgument(),
               ObjectSessionArgument val5 = ObjectSessionArgument(), ObjectSessionArgument val6 = ObjectSessionArgument(),
               ObjectSessionArgument val7 = ObjectSessionArgument(), ObjectSessionArgument val8 = ObjectSessionArgument(),
               ObjectSessionArgument val9 = ObjectSessionArgument());
    void deferExclusive(
        QObject *obj, const char *method, ObjectSessionArgument val0 = ObjectSessionArgument(),
        ObjectSessionArgument val1 = ObjectSessionArgument(), ObjectSessionArgument val2 = ObjectSessionArgument(),
        ObjectSessionArgument val3 = ObjectSessionArgument(), ObjectSessionArgument val4 = ObjectSessionArgument(),
        ObjectSessionArgument val5 = ObjectSessionArgument(), ObjectSessionArgument val6 = ObjectSessionArgument(),
        ObjectSessionArgument val7 = ObjectSessionArgument(), ObjectSessionArgument val8 = ObjectSessionArgument(),
        ObjectSessionArgument val9 = ObjectSessionArgument());

    void pause();
    void resume();

private:
    friend class ObjectSessionWatcher;
    ObjectSessionPrivate *d;
};

class ObjectSessionWatcher {
public:
    ObjectSessionWatcher(ObjectSession *sess);
    ~ObjectSessionWatcher();

    bool isValid() const;

private:
    friend class ObjectSessionPrivate;
    ObjectSessionWatcherPrivate *d;
};
} // namespace XMPP

#endif // OBJECTSESSION_H
