/*
 * Copyright (C) 2021  Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <QObject>

#include <map>
#include <memory>

#include "xmpp_features.h"

class QDomElement;

namespace XMPP {

class EncryptedSession : public QObject {
    Q_OBJECT
public:
    void write(const QDomElement &xml);
    void writeIncoming(const QDomElement &xml);
    void write(const QByteArray &xml);
    void writeIncoming(const QByteArray &xml);

    QByteArray read();
    QByteArray readOutgoing();
};

class EncryptionMethod : public QObject {
    Q_OBJECT
public:
    enum Capability {
        XmppStanza  = 0x1, /* XML */
        DataMessage = 0x2, /* all at once */
        DataStream  = 0x4  /* incremental */
    };
    Q_DECLARE_FLAGS(Capabilities, Capability)

    virtual QString           id() const                             = 0;
    virtual QString           name() const                           = 0;
    virtual Capabilities      capabilities() const                   = 0;
    virtual EncryptedSession *startSession(const Capabilities &caps) = 0;
    virtual Features          features()                             = 0;
};

class EncryptionManager : public QObject {
    Q_OBJECT
public:
    using MethodId   = QString;
    using MethodName = QString;

    ~EncryptionManager();
    using MethodsMap = std::map<MethodId, MethodName>;

    void       registerMethod(EncryptionMethod *algo);
    void       unregisterMethod(EncryptionMethod *algo);
    MethodsMap methods(EncryptionMethod::Capabilities caps = EncryptionMethod::Capabilities(0xff)) const;

private:
    class Private;
    std::unique_ptr<Private> d;
};

}
