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

#include <QDate>
#include <QDateTime>
#include <QDomElement>
#include <QExplicitlySharedDataPointer>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <variant>

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
    QDomElement toXmlElement(QDomDocument &document) const;
    bool        isEmpty() const noexcept;

    QStringList pobox;
    QStringList ext;
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

using PStringList = std::pair<Parameters, QStringList>;
using PString     = std::pair<Parameters, QString>;
using PUri        = std::pair<Parameters, QUrl>;
using PDate       = std::pair<Parameters, QDate>;
using PAdvUri     = std::pair<Parameters, UriValue>;
using PAddress    = std::pair<Parameters, Address>;
using PNames      = std::pair<Parameters, Names>;
using PUriOrText  = std::pair<Parameters, UriOrText>;
using PTimeZone   = std::pair<Parameters, TimeZone>;
using PHistorical = std::pair<Parameters, Historical>;

using PStringLists = QList<PStringList>;
using PStrings     = QList<PString>;
using PUris        = QList<PUri>;
using PAdvUris     = QList<PAdvUri>;
using PAddresses   = QList<PAddress>;
using PUrisOrTexts = QList<PUriOrText>;
using PTimeZones   = QList<PTimeZone>;

class VCard {
public:
    VCard();
    VCard(const QDomElement &element);
    ~VCard();

    bool     isEmpty() const;
    explicit operator bool() const;

    QDomElement toXmlElement(QDomDocument &document) const;

    static VCard fromFile(const QString &filename);
    bool         save(const QString &filename) const;

    // Getters and setters
    PStrings fullName() const;
    void     setFullName(const PStrings &fullName);

    const PNames &names() const;
    void          setNames(const PNames &names);

    PStringLists nickname() const;
    void         setNickname(const PStringLists &nickname);

    PStrings emails() const;
    void     setEmails(const PStrings &emails);

    PUrisOrTexts tels() const;
    void         setTels(const PUrisOrTexts &tels);

    PStringLists org() const;
    void         setOrg(const PStringLists &org);

    PStrings title() const;
    void     setTitle(const PStrings &title);

    PStrings role() const;
    void     setRole(const PStrings &role);

    PStrings note() const;
    void     setNote(const PStrings &note);

    PUris urls() const;
    void  setUrls(const PUris &urls);

    PHistorical bday() const;
    void        setBday(const PHistorical &bday);

    PHistorical anniversary() const;
    void        setAnniversary(const PHistorical &anniversary);

    Gender gender() const;
    void   setGender(Gender gender);

    QString genderComment() const;
    void    setGenderComment(const QString &comment);

    QString uid() const;
    void    setUid(const QString &uid);

    QString kind() const;
    void    setKind(const QString &kind);

    PStringLists categories() const;
    void         setCategories(const PStringLists &categories);

    PUris busyTimeUrl() const;
    void  setBusyTimeUrl(const PUris &busyTimeUrl);

    PUris calendarRequestUri() const;
    void  setCalendarRequestUri(const PUris &calendarRequestUri);

    PUris calendarUri() const;
    void  setCalendarUri(const PUris &calendarUri);

    QHash<int, QString> clientPidMap() const;
    void                setClientPidMap(const QHash<int, QString> &clientPidMap);

    PUris geo() const;
    void  setGeo(const PUris &geo);

    PUris impp() const;
    void  setImpp(const PUris &impp);

    PUrisOrTexts key() const;
    void         setKey(const PUrisOrTexts &key);

    PStrings lang() const;
    void     setLang(const PStrings &lang);

    PAdvUris logo() const;
    void     setLogo(const PAdvUris &logo);

    PUris member() const;
    void  setMember(const PUris &member);

    PAdvUris photo() const;
    void     setPhoto(const PAdvUris &photo);

    QString prodid() const;
    void    setProdid(const QString &prodid);

    PUrisOrTexts related() const;
    void         setRelated(const PUrisOrTexts &related);

    QDateTime rev() const;
    void      setRev(const QDateTime &rev);

    PAdvUris sound() const;
    void     setSound(const PAdvUris &sound);

    PUris source() const;
    void  setSource(const PUris &source);

    PTimeZones timeZone() const;
    void       setTimeZone(const PTimeZones &timeZone);

    PAddresses addresses() const;
    void       setAddresses(const PAddresses &addresses);

private:
    class VCardData;
    QExplicitlySharedDataPointer<VCardData> d;
};

} // namespace VCard4

#endif // XMPP_VCARD4_H
