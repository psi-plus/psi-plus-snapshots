/*
 * xmpp_vcard4.cpp - classes for handling vCards according to rfc6351
 * Copyright (C) 2024  Sergei Ilinykh
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

#ifndef XMPP_VCARD4_H
#define XMPP_VCARD4_H

#include "xmpp_vcard.h"

#include <QDate>
#include <QDateTime>
#include <QDomElement>
#include <QExplicitlySharedDataPointer>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <variant>

class QFile;

/**
 * This code represents implementation of RFC 6351/6350 as well as XEP-0292
 */

namespace XMPP::VCard4 {

enum class Gender { Undefined, Male, Female, Other, None, Unknown };

class Parameters {
public:
    Parameters() = default;
    Parameters(const QDomElement &element);
    void addTo(QDomElement parent) const;
    bool isEmpty() const;

    QStringList type;
    QString     language;
    QString     altid;
    QString     pid;
    int         pref = 0; // Preference (1 to 100)
    QString     geo;
    QString     tz; // Time zone
    QString     label;
};

class Names {
public:
    Names() = default;
    Names(const QDomElement &element);
    QDomElement toXmlElement(QDomDocument &document) const;
    bool        isEmpty() const noexcept;

    QStringList surname;
    QStringList given;
    QStringList additional;
    QStringList prefix;
    QStringList suffix;
};

class Address {
public:
    Address() = default;
    Address(const QDomElement &element);
    Address(const XMPP::VCard::Address &legacyAddress) :
        pobox({ legacyAddress.pobox }), extaddr({ legacyAddress.extaddr }), street({ legacyAddress.street }),
        locality({ legacyAddress.locality }), region({ legacyAddress.region }), code({ legacyAddress.pcode }),
        country({ legacyAddress.country })
    {
    }
    QDomElement toXmlElement(QDomDocument &document) const;
    bool        isEmpty() const noexcept;

    QStringList pobox;
    QStringList extaddr;
    QStringList street;
    QStringList locality;
    QStringList region;
    QStringList code;
    QStringList country;
};

class UriValue {
public:
    UriValue() = default;
    explicit UriValue(const QString &uri);
    explicit UriValue(const QByteArray &data, const QString &mime);
    QString toString() const;
    inline  operator QString() const { return toString(); }
    bool    isEmpty() const { return url.isEmpty() && data.isEmpty(); }

    QUrl       url;
    QByteArray data;
    QString    mediaType;
};

using UriOrText  = std::variant<QUrl, QString>;
using TimeZone   = std::variant<QUrl, QString, int>;
using Historical = std::variant<QDateTime, QDate, QTime, QString>;

template <typename T> struct ItemBase {
    Parameters parameters;
    T          data;
};

template <typename T> struct Item : public ItemBase<T> {
    operator QString() const { return this->data; } // maybe convertible by Qt means
    operator QDate() const { return QDate(this->data); }
    operator QUrl() const { return QUrl(this->data); }
};

template <> struct Item<QDate> : public ItemBase<QDate> {
    operator QString() const { return data.toString(Qt::ISODate); }
    operator QDate() const { return data; }
};

template <> struct Item<QDateTime> : public ItemBase<QDateTime> {
    operator QString() const { return data.toString(Qt::ISODate); }
    operator QDate() const { return data.date(); }
};

template <> struct Item<QStringList> : public ItemBase<QStringList> {
    operator QString() const { return data.value(0); }
};

template <> struct Item<Historical> : public ItemBase<Historical> {
    operator QString() const
    {
        return std::visit(
            [](auto const &v) {
                using Tv = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<Tv, QString>) {
                    return v;
                } else {
                    return v.toString(Qt::ISODate);
                }
            },
            data);
    }
    operator QDate() const
    {
        return std::visit(
            [](auto const &v) {
                using Tv = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<Tv, QDate>) {
                    return v;
                }
                if constexpr (std::is_same_v<Tv, QDateTime>) {
                    return v.date();
                } else {
                    return QDate {};
                }
            },
            data);
    }
};

template <> struct Item<UriOrText> : public ItemBase<UriOrText> {
    operator QString() const
    {
        return std::visit(
            [](auto const &v) {
                using Tv = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<Tv, QString>) {
                    return v;
                } else {
                    return v.toString();
                }
            },
            data);
    }
};

using PStringList = Item<QStringList>;
using PString     = Item<QString>;
using PUri        = Item<QUrl>;
using PDate       = Item<QDate>;
using PAdvUri     = Item<UriValue>;
using PAddress    = Item<Address>;
using PNames      = Item<Names>;
using PUriOrText  = Item<UriOrText>;
using PTimeZone   = Item<TimeZone>;
using PHistorical = Item<Historical>;

template <typename T> class TaggedList : public QList<T> {
public:
    using item_type = T;

    T preferred() const
    {
        if (this->empty()) {
            return {};
        }
        return *std::ranges::max_element(
            *this, [](auto const &a, auto const &b) { return a.parameters.pref > b.parameters.pref; });
    }

    operator QString() const { return preferred(); }
    operator QUrl() const { return preferred(); }
};

template <> class TaggedList<PAdvUri> : public QList<PAdvUri> {
public:
    using item_type = PAdvUri;

    operator QByteArray() const
    {
        // take first preferred data uri and its data
        if (this->empty()) {
            return {};
        }
        return std::ranges::max_element(*this,
                                        [](auto const &a, auto const &b) {
                                            return ((int(!a.data.data.isEmpty()) << 8) + a.parameters.pref)
                                                > ((int(!b.data.data.isEmpty()) << 8) + b.parameters.pref);
                                        })
            ->data.data;
    }
};

using PStringLists = TaggedList<PStringList>;
using PStrings     = TaggedList<PString>;
using PUris        = TaggedList<PUri>;
using PAdvUris     = TaggedList<PAdvUri>;
using PAddresses   = TaggedList<PAddress>;
using PUrisOrTexts = TaggedList<PUriOrText>;
using PTimeZones   = TaggedList<PTimeZone>;

class VCard {
public:
    VCard();
    VCard(const QDomElement &element);
    VCard(const VCard &other);

    ~VCard();

    VCard &operator=(const VCard &);
    void   detach();

    bool isEmpty() const;

    inline bool     isNull() const { return d != nullptr; }
    inline explicit operator bool() const { return isNull(); }

    QDomElement toXmlElement(QDomDocument &document) const;

    static VCard fromFile(const QString &filename);
    static VCard fromDevice(QIODevice *dev);
    bool         save(const QString &filename) const;

    void        fromVCardTemp(const XMPP::VCard &tempVCard);
    XMPP::VCard toVCardTemp() const;

    // Getters and setters
    PStrings       fullName() const;
    void           setFullName(const PStrings &fullName);
    Item<QString> &setFullName(const QString &fullName);

    PNames       names() const;
    void         setNames(const PNames &names);
    Item<Names> &setNames(const Names &names);

    PStringLists       nickName() const;
    void               setNickName(const PStringLists &nickname);
    Item<QStringList> &setNickName(const QStringList &nickname);

    PStrings       emails() const;
    void           setEmails(const PStrings &emails);
    Item<QString> &setEmails(const QString &email);

    PUrisOrTexts     phones() const;
    void             setPhones(const PUrisOrTexts &tels);
    Item<UriOrText> &setPhones(const UriOrText &phone);

    PStringLists       org() const;
    void               setOrg(const PStringLists &org);
    Item<QStringList> &setOrg(const QStringList &org);

    PStrings       title() const;
    void           setTitle(const PStrings &title);
    Item<QString> &setTitle(const QString &title);

    PStrings       role() const;
    void           setRole(const PStrings &role);
    Item<QString> &setRole(const QString &role);

    PStrings       note() const;
    void           setNote(const PStrings &note);
    Item<QString> &setNote(const QString &note);

    PUris       urls() const;
    void        setUrls(const PUris &urls);
    Item<QUrl> &setUrls(const QUrl &url);

    PHistorical       bday() const;
    void              setBday(const PHistorical &bday);
    Item<Historical> &setBday(const Historical &bday);

    PHistorical       anniversary() const;
    void              setAnniversary(const PHistorical &anniversary);
    Item<Historical> &setAnniversary(const Historical &anniversary);

    Gender gender() const;
    void   setGender(Gender gender);

    QString genderComment() const;
    void    setGenderComment(const QString &comment);

    QString uid() const;
    void    setUid(const QString &uid);

    QString kind() const;
    void    setKind(const QString &kind);

    PStringLists       categories() const;
    void               setCategories(const PStringLists &categories);
    Item<QStringList> &setCategories(const QStringList &categories);

    PUris       busyTimeUrl() const;
    void        setBusyTimeUrl(const PUris &busyTimeUrl);
    Item<QUrl> &setBusyTimeUrl(const QUrl &url);

    PUris       calendarRequestUri() const;
    void        setCalendarRequestUri(const PUris &calendarRequestUri);
    Item<QUrl> &setCalendarRequestUri(const QUrl &url);

    PUris       calendarUri() const;
    void        setCalendarUri(const PUris &calendarUri);
    Item<QUrl> &setCalendarUri(const QUrl &url);

    QHash<int, QString> clientPidMap() const;
    void                setClientPidMap(const QHash<int, QString> &clientPidMap);

    PUris       geo() const;
    void        setGeo(const PUris &geo);
    Item<QUrl> &setGeo(const QUrl &url);

    PUris       impp() const;
    void        setImpp(const PUris &impp);
    Item<QUrl> &setImpp(const QUrl &url);

    PUrisOrTexts     key() const;
    void             setKey(const PUrisOrTexts &key);
    Item<UriOrText> &setKey(const UriOrText &key);

    PStrings       languages() const;
    void           setLanguages(const PStrings &lang);
    Item<QString> &setLanguages(const QString &lang);

    PAdvUris        logo() const;
    void            setLogo(const PAdvUris &logo);
    Item<UriValue> &setLogo(const UriValue &logo);

    PUris       member() const;
    void        setMember(const PUris &member);
    Item<QUrl> &setMember(const QUrl &member);

    PAdvUris        photo() const;
    void            setPhoto(const PAdvUris &photo);
    Item<UriValue> &setPhoto(const UriValue &photo);

    QString prodid() const;
    void    setProdid(const QString &prodid);

    PUrisOrTexts     related() const;
    void             setRelated(const PUrisOrTexts &related);
    Item<UriOrText> &setRelated(const UriOrText &related);

    QDateTime rev() const;
    void      setRev(const QDateTime &rev);

    PAdvUris        sound() const;
    void            setSound(const PAdvUris &sound);
    Item<UriValue> &setSound(const UriValue &sound);

    PUris       source() const;
    void        setSource(const PUris &source);
    Item<QUrl> &setSource(const QUrl &source);

    PTimeZones      timeZone() const;
    void            setTimeZone(const PTimeZones &timeZone);
    Item<TimeZone> &setTimeZone(const TimeZone &timeZone);

    PAddresses     addresses() const;
    void           setAddresses(const PAddresses &addresses);
    Item<Address> &setAddresses(const Address &addresses);

private:
    class VCardData;
    QExplicitlySharedDataPointer<VCardData> d;
};

} // namespace VCard4

#endif // XMPP_VCARD4_H
