/*
 * xmpp_features.h
 * Copyright (C) 2003  Justin Karneges
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

#ifndef XMPP_FEATURES_H
#define XMPP_FEATURES_H

#include <QSet>
#include <QStringList>

class QString;

namespace XMPP {
class Features {
public:
    Features();
    Features(const QStringList &);
    Features(const QSet<QString> &);
    Features(const QString &);
    ~Features();

    QStringList list() const; // actual featurelist
    void        setList(const QStringList &);
    void        setList(const QSet<QString> &);
    void        addFeature(const QString &);

    // features
    inline bool isEmpty() const { return _list.isEmpty(); }
    bool        hasRegister() const;
    bool        hasSearch() const;
    bool        hasMulticast() const;
    bool        hasGroupchat() const;
    bool        hasVoice() const;
    bool        hasDisco() const;
    bool        hasChatState() const;
    bool        hasCommand() const;
    bool        hasGateway() const;
    bool        hasVersion() const;
    bool        hasVCard() const;
    bool        hasMessageCarbons() const;
    bool        hasJingleFT() const;
    bool        hasJingleIceUdp() const;
    bool        hasJingleIce() const;

    [[deprecated]] inline bool canRegister() const { return hasRegister(); }
    [[deprecated]] inline bool canSearch() const { return hasSearch(); }
    [[deprecated]] inline bool canMulticast() const { return hasMulticast(); }
    [[deprecated]] inline bool canGroupchat() const { return hasGroupchat(); }
    [[deprecated]] inline bool canVoice() const { return hasVoice(); }
    [[deprecated]] inline bool canDisco() const { return hasDisco(); }
    [[deprecated]] inline bool canChatState() const { return hasChatState(); }
    [[deprecated]] inline bool canCommand() const { return hasCommand(); }
    [[deprecated]] inline bool isGateway() const { return hasGateway(); }
    [[deprecated]] inline bool haveVCard() const { return hasVCard(); }
    [[deprecated]] inline bool canMessageCarbons() const { return hasMessageCarbons(); }

    enum FeatureID {
        FID_Invalid = -1,
        FID_None,
        FID_Register,
        FID_Search,
        FID_Groupchat,
        FID_Disco,
        FID_Gateway,
        FID_VCard,
        FID_AHCommand,
        FID_QueryVersion,
        FID_MessageCarbons,

        // private Psi actions
        FID_Add
    };

    // useful functions
    inline bool test(const char *ns) const { return test(QSet<QString>() << QLatin1String(ns)); }
    bool        test(const QStringList &) const;
    bool        test(const QSet<QString> &) const;

    QString        name() const;
    static QString name(long id);
    static QString name(const QString &feature);

    long           id() const;
    static long    id(const QString &feature);
    static QString feature(long id);

    Features &  operator<<(const QString &feature);
    inline bool operator==(const Features &other) { return _list == other._list; }

    class FeatureName;

private:
    QSet<QString> _list;
};
} // namespace XMPP

#endif // XMPP_FEATURES_H
