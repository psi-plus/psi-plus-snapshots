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

#include "xmpp_vcard4.h"

#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimeZone>

namespace XMPP::VCard4 {

namespace {

#define INIT_D()                                                                                                       \
    do {                                                                                                               \
        if (!d)                                                                                                        \
            d = new VCardData;                                                                                         \
    } while (0)

    const QString VCARD_NAMESPACE = QLatin1String("urn:ietf:params:xml:ns:vcard-4.0");

    struct VCardHelper {
        static QString extractText(const QDomElement &element, const char *tagName)
        {
            QDomElement tagElement = element.firstChildElement(QString::fromLatin1(tagName));
            return tagElement.isNull() ? QString() : tagElement.text();
        }

        static QStringList extractTexts(const QDomElement &element, const char *tagName)
        {
            auto        tn = QString::fromLatin1(tagName);
            QStringList texts;
            QDomElement tagElement = element.firstChildElement(tn);
            while (!tagElement.isNull()) {
                texts.append(tagElement.text());
                tagElement = tagElement.nextSiblingElement(tn);
            }
            return texts;
        }

        static PStrings parseParameterList(const QDomElement &element, const QString &tagName,
                                           const QString &innetTagName)
        {
            PStrings    result;
            QDomElement tagElement = element.firstChildElement(tagName);
            while (!tagElement.isNull()) {
                Parameters parameters(tagElement.firstChildElement(QLatin1String("parameters")));
                QString    value = tagElement.firstChildElement(innetTagName).text();
                result.append({ parameters, value });
                tagElement = tagElement.nextSiblingElement(tagName);
            }
            return result;
        }

        static void serialize(QDomElement parent, const PHistorical &historical, const char *tagName)
        {
            std::visit(
                [&](auto const &v) mutable {
                    if (v.isNull()) {
                        return;
                    }
                    auto doc  = parent.ownerDocument();
                    auto bday = parent.appendChild(doc.createElement(QLatin1String(tagName))).toElement();
                    historical.first.addTo(bday);
                    using Tv = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<Tv, QString>) {
                        VCardHelper::addTextElement(doc, bday, QLatin1String("text"), QStringList { v });
                    } else if constexpr (std::is_same_v<Tv, QDate>) {
                        VCardHelper::addTextElement(doc, bday, QLatin1String("date"),
                                                    QStringList { v.toString(Qt::ISODate) });
                    } else if constexpr (std::is_same_v<Tv, QDateTime>) {
                        VCardHelper::addTextElement(doc, bday, QLatin1String("date-time"),
                                                    QStringList { v.toString(Qt::ISODate) });
                    } else if constexpr (std::is_same_v<Tv, QTime>) {
                        VCardHelper::addTextElement(doc, bday, QLatin1String("time"),
                                                    QStringList { v.toString(Qt::ISODate) });
                    }
                },
                historical.second);
        }

        template <typename ListType>
        static void addTextElement(QDomDocument &document, QDomElement &parent, const QString &tagName,
                                   const ListType &texts)
        {
            if (!texts.isEmpty()) {
                QDomElement element = document.createElement(tagName);
                for (const auto &text : texts) {
                    QDomElement textElement = element;
                    textElement.appendChild(document.createTextNode(text));
                }
                parent.appendChild(element);
            }
        }

        template <typename T>
        static void serializeList(QDomElement &parent, const QList<std::pair<Parameters, T>> &list,
                                  const QString &tagName, const QString &innerTagName = QLatin1String("text"))
        {
            auto document = parent.ownerDocument();
            for (const auto &entry : list) {
                QDomElement element = document.createElement(tagName);
                entry.first.addTo(element);
                if constexpr (std::is_same_v<T, QString>) {
                    addTextElement(document, element, QLatin1String("text"), QStringList { entry.second });
                } else if constexpr (std::is_same_v<T, QUrl> || std::is_same_v<T, UriValue>) {
                    addTextElement(document, element, QLatin1String("uri"), QStringList { entry.second.toString() });
                } else if constexpr (std::is_same_v<T, UriOrText>) {
                    std::visit(
                        [&](auto v) {
                            using Tv = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<Tv, QUrl>) {
                                addTextElement(document, element, QLatin1String("uri"), QStringList { v.toString() });
                            } else {
                                addTextElement(document, element, QLatin1String("text"), QStringList { v });
                            }
                        },
                        entry.second);
                } else if constexpr (std::is_same_v<T, QStringList>) {
                    for (auto const &s : entry.second) {
                        element.appendChild(document.createElement(QLatin1String("text")))
                            .appendChild(document.createTextNode(s));
                    }
                } else if constexpr (std::is_same_v<T, TimeZone>) {
                    std::visit(
                        [&](auto v) {
                            using Tv = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<Tv, QUrl>) {
                                addTextElement(document, element, QLatin1String("uri"), QStringList { v.toString() });
                            } else if constexpr (std::is_same_v<Tv, int>) {
                                addTextElement(
                                    document, element, QLatin1String("utc-offset"),
                                    QStringList {
                                        QLatin1Char(v < 0 ? '-' : '+')
                                        + QString("%1%2")
                                              .arg(std::abs(v) / 3600, 2, 10, QChar::fromLatin1('0'))
                                              .arg((std::abs(v) % 3600) / 60, 2, 10, QChar::fromLatin1('0')) });
                            } else {
                                addTextElement(document, element, QLatin1String("text"), QStringList { v });
                            }
                        },
                        entry.second);
                } else {
                    throw std::logic_error("should never happen. some type is not supported");
                }
                parent.appendChild(element);
            }
        }

        static VCard4::Gender stringToGender(const QString &genderStr)
        {
            if (genderStr.compare(QLatin1String("M"), Qt::CaseInsensitive) == 0) {
                return Gender::Male;
            } else if (genderStr.compare(QLatin1String("F"), Qt::CaseInsensitive) == 0) {
                return Gender::Female;
            } else if (genderStr.compare(QLatin1String("O"), Qt::CaseInsensitive) == 0) {
                return Gender::Other;
            } else if (genderStr.compare(QLatin1String("N"), Qt::CaseInsensitive) == 0) {
                return Gender::None;
            } else if (genderStr.compare(QLatin1String("U"), Qt::CaseInsensitive) == 0) {
                return Gender::Unknown;
            } else {
                return Gender::Undefined;
            }
        }

        static QString genderToString(Gender gender)
        {
            switch (gender) {
            case Gender::Male:
                return QLatin1String("M");
            case Gender::Female:
                return QLatin1String("F");
            case Gender::Other:
                return QLatin1String("O");
            case Gender::None:
                return QLatin1String("N");
            case Gender::Unknown:
                return QLatin1String("U");
            default:
                return QLatin1String("");
            }
        }

        template <typename ItemT>
        static void fillContainer(QDomElement parent, const char *tagName,
                                  QList<std::pair<Parameters, ItemT>> &container)
        {
            auto tn = QString::fromLatin1(tagName);
            for (auto e = parent.firstChildElement(tn); !e.isNull(); e = e.nextSiblingElement(tn)) {
                Parameters parameters(e.firstChildElement(QLatin1String("parameters")));
                if constexpr (std::is_same_v<ItemT, QString>) {
                    container.append({ parameters, VCardHelper::extractText(e, "text") });
                } else if constexpr (std::is_same_v<ItemT, QUrl> || std::is_same_v<ItemT, UriValue>) {
                    container.append({ parameters, ItemT(VCardHelper::extractText(e, "uri")) });
                } else if constexpr (std::is_same_v<ItemT, UriOrText>) {
                    auto uri = VCardHelper::extractText(e, "uri");
                    if (uri.isEmpty()) {
                        auto text = VCardHelper::extractText(e, "text");
                        container.append({ parameters, text });
                    } else {
                        container.append({ parameters, QUrl(uri) });
                    }
                } else if constexpr (std::is_same_v<ItemT, QStringList>) {
                    container.append({ parameters, VCardHelper::extractTexts(e, "text") });
                } else if constexpr (std::is_same_v<ItemT, TimeZone>) {
                    auto text = VCardHelper::extractText(e, "text");
                    if (text.isEmpty()) {
                        auto uri = VCardHelper::extractText(e, "uri");
                        if (uri.isEmpty()) {
                            auto offset = VCardHelper::extractText(e, "utc-offset");
                            bool neg    = offset.startsWith(QLatin1Char('-'));
                            if (neg || offset.startsWith(QLatin1Char('+'))) {
                                QStringView sv { offset };
                                container.append(
                                    { parameters,
                                      (sv.mid(1, 2).toInt() * 3600 + sv.mid(3, 2).toInt() * 60) * (neg ? 1 : -1) });
                            } else {
                                container.append({ parameters, 0 });
                            }
                        } else {
                            container.append({ parameters, QUrl(uri) });
                        }
                    } else {
                        container.append({ parameters, text });
                    }
                }
            }
        };

        static void unserialize(const QDomElement &parent, const char *tagName, PHistorical &to)
        {
            QDomElement source = parent.firstChildElement(QLatin1String(tagName));
            if (source.isNull()) {
                return;
            }
            to.first = Parameters(source.firstChildElement(QLatin1String("parameters")));
            auto v   = VCardHelper::extractText(source, "date");
            if (v.isNull()) {
                v = VCardHelper::extractText(source, "date-time");
                if (v.isNull()) {
                    v = VCardHelper::extractText(source, "time");
                    if (v.isNull()) {
                        to.second = VCardHelper::extractText(source, "text");
                    } else {
                        to.second = QTime::fromString(v, Qt::ISODate);
                    }
                } else {
                    to.second = QDateTime::fromString(v, Qt::ISODate);
                }
            } else {
                to.second = QDate::fromString(v, Qt::ISODate);
            }
        }

        static bool isNull(const PHistorical &h)
        {
            return std::visit([](auto const &v) { return v.isNull(); }, h.second);
        }
    };

} // namespace

Names::Names(const QDomElement &element)
{
    surname    = VCardHelper::extractTexts(element, "surname");
    given      = VCardHelper::extractTexts(element, "given");
    additional = VCardHelper::extractTexts(element, "additional");
    prefix     = VCardHelper::extractTexts(element, "prefix");
    suffix     = VCardHelper::extractTexts(element, "suffix");
}

QDomElement Names::toXmlElement(QDomDocument &document) const
{
    QDomElement nameElement = document.createElement(QLatin1String("n"));
    VCardHelper::addTextElement(document, nameElement, QLatin1String("surname"), surname);
    VCardHelper::addTextElement(document, nameElement, QLatin1String("given"), given);
    VCardHelper::addTextElement(document, nameElement, QLatin1String("additional"), additional);
    VCardHelper::addTextElement(document, nameElement, QLatin1String("prefix"), prefix);
    VCardHelper::addTextElement(document, nameElement, QLatin1String("suffix"), suffix);
    return nameElement;
}

bool Names::isEmpty() const noexcept
{
    return surname.isEmpty() && given.isEmpty() && additional.isEmpty() && prefix.isEmpty() && suffix.isEmpty();
}

Address::Address(const QDomElement &element)
{
    pobox    = VCardHelper::extractTexts(element, "pobox");
    ext      = VCardHelper::extractTexts(element, "ext");
    street   = VCardHelper::extractTexts(element, "street");
    locality = VCardHelper::extractTexts(element, "locality");
    region   = VCardHelper::extractTexts(element, "region");
    code     = VCardHelper::extractTexts(element, "code");
    country  = VCardHelper::extractTexts(element, "country");
}

QDomElement Address::toXmlElement(QDomDocument &document) const
{
    QDomElement addressElement = document.createElement(QLatin1String("adr"));
    VCardHelper::addTextElement(document, addressElement, QLatin1String("pobox"), pobox);
    VCardHelper::addTextElement(document, addressElement, QLatin1String("ext"), ext);
    VCardHelper::addTextElement(document, addressElement, QLatin1String("street"), street);
    VCardHelper::addTextElement(document, addressElement, QLatin1String("locality"), locality);
    VCardHelper::addTextElement(document, addressElement, QLatin1String("region"), region);
    VCardHelper::addTextElement(document, addressElement, QLatin1String("code"), code);
    VCardHelper::addTextElement(document, addressElement, QLatin1String("country"), country);
    return addressElement;
}

bool Address::isEmpty() const noexcept
{
    return pobox.isEmpty() && ext.isEmpty() && street.isEmpty() && locality.isEmpty() && region.isEmpty()
        && code.isEmpty() && country.isEmpty();
}

Parameters::Parameters(const QDomElement &element)
{
    if (element.isNull()) {
        return;
    }

    language = element.firstChildElement("language").text();
    altid    = element.firstChildElement("altid").text();

    QDomElement pidElement = element.firstChildElement("pid");
    while (!pidElement.isNull()) {
        pid.append(pidElement.text());
        pidElement = pidElement.nextSiblingElement("pid");
    }

    QDomElement prefElement = element.firstChildElement("pref");
    if (!prefElement.isNull()) {
        pref = prefElement.text().toInt();
    }

    QDomElement typeElement = element.firstChildElement("type");
    while (!typeElement.isNull()) {
        QDomElement textElement = typeElement.firstChildElement("text");
        while (!textElement.isNull()) {
            type.append(textElement.text());
            textElement = textElement.nextSiblingElement("text");
        }
        typeElement = typeElement.nextSiblingElement("type");
    }

    geo   = element.firstChildElement("geo").text();
    tz    = element.firstChildElement("tz").text();
    label = element.firstChildElement("label").text();
}

void Parameters::addTo(QDomElement parent) const
{
    QDomDocument document = parent.ownerDocument();

    auto pel = document.createElement(QLatin1String("parameters"));

    if (!language.isEmpty()) {
        QDomElement element = document.createElement("language");
        element.appendChild(document.createTextNode(language));
        pel.appendChild(element);
    }

    if (!altid.isEmpty()) {
        QDomElement element = document.createElement("altid");
        element.appendChild(document.createTextNode(altid));
        pel.appendChild(element);
    }

    for (auto const &value : pid) {
        QDomElement element = document.createElement("pid");
        element.appendChild(document.createTextNode(value));
        pel.appendChild(element);
    }

    if (pref > 0) {
        QDomElement element = document.createElement("pref");
        element.appendChild(document.createTextNode(QString::number(pref)));
        pel.appendChild(element);
    }

    if (!type.isEmpty()) {
        QDomElement typeElement = document.createElement("type");
        for (const QString &value : type) {
            QDomElement textElement = document.createElement("text");
            textElement.appendChild(document.createTextNode(value));
            typeElement.appendChild(textElement);
        }
        pel.appendChild(typeElement);
    }

    if (!geo.isEmpty()) {
        QDomElement element = document.createElement("geo");
        element.appendChild(document.createTextNode(geo));
        pel.appendChild(element);
    }

    if (!tz.isEmpty()) {
        QDomElement element = document.createElement("tz");
        element.appendChild(document.createTextNode(tz));
        pel.appendChild(element);
    }

    if (!label.isEmpty()) {
        QDomElement element = document.createElement("label");
        element.appendChild(document.createTextNode(label));
        pel.appendChild(element);
    }

    if (pel.hasChildNodes()) {
        parent.appendChild(pel);
    }
}

bool Parameters::isEmpty() const
{
    return language.isEmpty() && altid.isEmpty() && pid.isEmpty() && pref == -1 && type.isEmpty() && geo.isEmpty()
        && tz.isEmpty() && label.isEmpty();
}

UriValue::UriValue(const QString &uri)
{
    if (uri.startsWith(QLatin1String("data:"))) {
        static QRegularExpression re(QLatin1String("data:([^;]+);base64,(.*)"),
                                     QRegularExpression::MultilineOption
                                         | QRegularExpression::DotMatchesEverythingOption);
        QRegularExpressionMatch   match = re.match(uri);
        if (match.hasMatch()) {
            mediaType = match.captured(1);
            data      = QByteArray::fromBase64(match.captured(2).trimmed().toUtf8());
        }
    } else {
        url = QUrl(uri);
    }
}

QString UriValue::toString() const
{
    if (!mediaType.isEmpty()) {
        return QString(QLatin1String("data:%1;base64,%2")).arg(mediaType, QString(data.toBase64()));
    } else {
        return url.toString();
    }
}

class VCard::VCardData : public QSharedData {
public:
    PUris   source; // any nuumber of uris
    QString kind;   // none is ok

    // Identification Properties
    PStrings       fullName; // at least one full name
    PNames         names;    // at most 1
    PStringLists   nickname;
    PAdvUris       photo;
    PHistorical    bday;        // at most 1
    PHistorical    anniversary; // at most 1
    VCard4::Gender gender;
    QString        genderComment;

    // Delivery Addressing Properties
    QList<std::pair<Parameters, Address>> addresses; // any number of addresses

    // Communications Properties
    PUrisOrTexts tels;   // any number of telephones
    PStrings     emails; // any number of emails
    PUris        impp;   // any number impp
    PStrings     lang;   // any number of languages

    // Geographical Properties
    PTimeZones timeZone; // any number of time zones
    PUris      geo;      // any number of geo locations

    // Organizational Properties
    PStrings     title;
    PStrings     role;
    PAdvUris     logo;
    PStringLists org;
    PUris        member;
    PUrisOrTexts related;

    // Explanatory Properties
    PStringLists        categories;
    PStrings            note;
    QString             prodid; // at most 1
    QDateTime           rev;    // at most 1
    PAdvUris            sound;
    QString             uid; // at most 1
    QHash<int, QString> clientPidMap;
    PUris               urls;
    PUrisOrTexts        key;

    // Calendar Properties
    PUris busyTimeUrl;
    PUris calendarRequestUri;
    PUris calendarUri;

    VCardData() { }

    VCardData(const QDomElement &element)
    {
        if (element.namespaceURI() != VCARD_NAMESPACE)
            return;

        auto foreachElement = [](auto parent, auto tagName, auto callback) {
            for (auto e = parent.firstChildElement(tagName); !e.isNull(); e = e.nextSiblingElement(tagName)) {
                callback(e);
            }
        };

        QDomElement nameElement = element.firstChildElement(QLatin1String("n"));
        if (!nameElement.isNull()) {
            Parameters parameters(nameElement.firstChildElement(QLatin1String("parameters")));
            names = { parameters, Names(nameElement) };
        }

        VCardHelper::fillContainer(element, "fn", fullName);
        VCardHelper::fillContainer(element, "nickname", nickname);
        VCardHelper::fillContainer(element, "org", org);
        VCardHelper::fillContainer(element, "categories", categories);
        VCardHelper::fillContainer(element, "title", title);
        VCardHelper::fillContainer(element, "role", role);
        VCardHelper::fillContainer(element, "note", note);
        VCardHelper::fillContainer(element, "fburl", busyTimeUrl);
        VCardHelper::fillContainer(element, "caladruri", calendarRequestUri);
        VCardHelper::fillContainer(element, "url", urls);
        VCardHelper::fillContainer(element, "caluri", calendarUri);
        VCardHelper::fillContainer(element, "impp", impp);
        VCardHelper::fillContainer(element, "geo", geo);
        VCardHelper::fillContainer(element, "tel", tels);
        VCardHelper::fillContainer(element, "email", emails);
        VCardHelper::fillContainer(element, "key", key);

        VCardHelper::unserialize(element, "bday", bday);
        VCardHelper::unserialize(element, "anniversary", anniversary);

        QDomElement genderElement = element.firstChildElement(QLatin1String("sex"));
        if (!genderElement.isNull()) {
            gender = VCardHelper::stringToGender(genderElement.text());
        }

        QDomElement genderCommentElement = genderElement.nextSiblingElement(QLatin1String("identity"));
        if (!genderCommentElement.isNull()) {
            genderComment = VCardHelper::extractText(genderCommentElement, "text");
        }

        uid  = VCardHelper::extractText(element, "uid");
        kind = VCardHelper::extractText(element, "kind");

        foreachElement(element, QLatin1String("clientpidmap"), [this](QDomElement e) {
            int     sourceId = VCardHelper::extractText(e, "sourceid").toInt();
            QString uri      = VCardHelper::extractText(e, "uri");
            clientPidMap.insert(sourceId, uri);
        });

        foreachElement(element, QLatin1String("lang"), [this](QDomElement e) {
            Parameters parameters(e.firstChildElement(QLatin1String("parameters")));
            QString    langValue = VCardHelper::extractText(e, "language-tag");
            lang.append({ parameters, langValue });
        });

        VCardHelper::fillContainer(element, "logo", logo);
        VCardHelper::fillContainer(element, "member", member);
        VCardHelper::fillContainer(element, "photo", photo);
        VCardHelper::fillContainer(element, "sound", sound);
        VCardHelper::fillContainer(element, "source", source);
        VCardHelper::fillContainer(element, "tz", timeZone);

        prodid = VCardHelper::extractText(element, "prodid");

        VCardHelper::fillContainer(element, "related", related);

        QDomElement revElement = element.firstChildElement(QLatin1String("rev"));
        if (!revElement.isNull()) {
            rev = QDateTime::fromString(VCardHelper::extractText(revElement, "timestamp"), Qt::ISODate);
        }

        foreachElement(element, QLatin1String("adr"), [this](QDomElement e) {
            Parameters parameters(e.firstChildElement(QLatin1String("parameters")));
            Address    addressValue(e);
            addresses.append({ parameters, addressValue });
        });
    }

    bool isEmpty() const
    {
        return fullName.isEmpty() && names.second.isEmpty() && nickname.isEmpty() && emails.isEmpty() && tels.isEmpty()
            && org.isEmpty() && title.isEmpty() && role.isEmpty() && note.isEmpty() && urls.isEmpty()
            && VCardHelper::isNull(bday) && VCardHelper::isNull(anniversary) && gender == VCard4::Gender::Undefined
            && uid.isEmpty() && kind.isEmpty() && categories.isEmpty() && busyTimeUrl.isEmpty()
            && calendarRequestUri.isEmpty() && calendarUri.isEmpty() && clientPidMap.isEmpty() && geo.isEmpty()
            && impp.isEmpty() && key.isEmpty() && lang.isEmpty() && logo.isEmpty() && member.isEmpty()
            && photo.isEmpty() && prodid.isEmpty() && related.isEmpty() && rev.isNull() && sound.isEmpty()
            && source.isEmpty() && timeZone.isEmpty() && addresses.isEmpty();
    }
};

VCard::VCard() : d(nullptr) { }

VCard::VCard(const QDomElement &element) : d(new VCardData(element)) { }

VCard::~VCard() = default;

bool VCard::isEmpty() const
{
    if (!d)
        return true;
    return d->isEmpty();
}

VCard::operator bool() const { return d != nullptr; }

QDomElement VCard::toXmlElement(QDomDocument &document) const
{
    if (!d) {
        return {};
    }

    QDomElement vCardElement = document.createElement(QLatin1String("vcard"));

    VCardHelper::serializeList(vCardElement, d->fullName, QLatin1String("fn"));
    if (!d->names.second.isEmpty()) {
        auto e = vCardElement.appendChild(d->names.second.toXmlElement(document)).toElement();
        d->names.first.addTo(e);
    }
    VCardHelper::serializeList(vCardElement, d->nickname, QLatin1String("nickname"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->emails, QLatin1String("email"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->tels, QLatin1String("tel"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->org, QLatin1String("org"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->title, QLatin1String("title"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->role, QLatin1String("role"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->note, QLatin1String("note"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->urls, QLatin1String("url"), QLatin1String("uri"));

    VCardHelper::serialize(vCardElement, d->bday, "bday");
    VCardHelper::serialize(vCardElement, d->bday, "anniversary");

    if (d->gender != VCard4::Gender::Undefined) {
        QDomElement genderElement = document.createElement(QLatin1String("gender"));
        genderElement.appendChild(document.createTextNode(VCardHelper::genderToString(d->gender)));
        if (!d->genderComment.isEmpty()) {
            VCardHelper::addTextElement(document, genderElement, QLatin1String("identity"), d->genderComment);
        }
        vCardElement.appendChild(genderElement);
    }

    VCardHelper::serializeList(vCardElement, d->categories, QLatin1String("categories"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->busyTimeUrl, QLatin1String("fburl"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->calendarRequestUri, QLatin1String("caladruri"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->calendarUri, QLatin1String("caluri"), QLatin1String("uri"));
    for (auto it = d->clientPidMap.cbegin(); it != d->clientPidMap.cend(); ++it) {
        auto m = vCardElement.appendChild(document.createElement(QLatin1String("clientpidmap")));
        m.appendChild(document.createElement(QLatin1String("sourceid")))
            .appendChild(document.createTextNode(QString::number(it.key())));
        m.appendChild(document.createElement(QLatin1String("uri"))).appendChild(document.createTextNode(it.value()));
    }
    VCardHelper::serializeList(vCardElement, d->geo, QLatin1String("geo"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->impp, QLatin1String("impp"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->key, QLatin1String("key"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->lang, QLatin1String("lang"), QLatin1String("language-tag"));
    VCardHelper::serializeList(vCardElement, d->logo, QLatin1String("logo"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->member, QLatin1String("member"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->photo, QLatin1String("photo"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->related, QLatin1String("related"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->timeZone, QLatin1String("tz"), QLatin1String("text"));
    VCardHelper::serializeList(vCardElement, d->sound, QLatin1String("sound"), QLatin1String("uri"));
    VCardHelper::serializeList(vCardElement, d->source, QLatin1String("source"), QLatin1String("uri"));

    if (d->rev.isValid()) {
        auto revE = vCardElement.appendChild(document.createElement(QLatin1String("rev")))
                        .appendChild(document.createTextNode(d->rev.toString(Qt::ISODate)));
    }

    for (const auto &address : d->addresses) {
        QDomElement adrElement = document.createElement(QLatin1String("adr"));
        address.first.addTo(adrElement);
        adrElement.appendChild(address.second.toXmlElement(document));
        vCardElement.appendChild(adrElement);
    }

    return vCardElement;
}

VCard VCard::fromFile(const QString &filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return VCard();

    QDomDocument doc;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!doc.setContent(&file, true)) {
#else
    if (!doc.setContent(&file, QDomDocument::ParseOption::UseNamespaceProcessing)) {
#endif
        file.close();
        return VCard();
    }
    file.close();

    QDomElement root = doc.documentElement();
    if (root.tagName() != QLatin1String("vcards") || root.namespaceURI() != VCARD_NAMESPACE)
        return VCard();

    QDomElement vCardElement = root.firstChildElement(QLatin1String("vcard"));
    if (vCardElement.isNull())
        return VCard();

    return VCard(vCardElement);
}

bool VCard::save(const QString &filename) const
{
    if (!d)
        return false;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QDomDocument doc;

    QDomProcessingInstruction instr = doc.createProcessingInstruction("xml", "version='1.0' encoding='UTF-8'");
    doc.appendChild(instr);

    QDomElement root = doc.createElementNS(VCARD_NAMESPACE, QLatin1String("vcards"));
    doc.appendChild(root);

    QDomElement vCardElement = toXmlElement(doc);
    root.appendChild(vCardElement);

    QTextStream stream(&file);
    doc.save(stream, 4);
    file.close();

    return true;
}

// Getters and setters implementation

PStrings VCard::fullName() const { return d->fullName; }

void VCard::setFullName(const PStrings &fullName)
{
    INIT_D();
    d->fullName = fullName;
}

const PNames &VCard::names() const { return d->names; }

void VCard::setNames(const PNames &names)
{
    INIT_D();
    d->names = names;
}

PStringLists VCard::nickname() const { return d ? d->nickname : PStringLists(); }

void VCard::setNickname(const PStringLists &nickname)
{
    INIT_D();
    d->nickname = nickname;
}

PStrings VCard::emails() const { return d ? d->emails : PStrings(); }

void VCard::setEmails(const PStrings &emails)
{
    INIT_D();
    d->emails = emails;
}

PUrisOrTexts VCard::tels() const { return d ? d->tels : PUrisOrTexts(); }

void VCard::setTels(const PUrisOrTexts &tels)
{
    INIT_D();
    d->tels = tels;
}

PStringLists VCard::org() const { return d ? d->org : PStringLists(); }

void VCard::setOrg(const PStringLists &org)
{
    INIT_D();
    d->org = org;
}

PStrings VCard::title() const { return d ? d->title : PStrings(); }

void VCard::setTitle(const PStrings &title)
{
    INIT_D();
    d->title = title;
}

PStrings VCard::role() const { return d ? d->role : PStrings(); }

void VCard::setRole(const PStrings &role)
{
    INIT_D();
    d->role = role;
}

PStrings VCard::note() const { return d ? d->note : PStrings(); }

void VCard::setNote(const PStrings &note)
{
    INIT_D();
    d->note = note;
}

PUris VCard::urls() const { return d ? d->urls : PUris(); }

void VCard::setUrls(const PUris &urls)
{
    INIT_D();
    d->urls = urls;
}

PHistorical VCard::bday() const { return d ? d->bday : PHistorical(); }

void VCard::setBday(const PHistorical &bday)
{
    INIT_D();
    d->bday = bday;
}

PHistorical VCard::anniversary() const { return d ? d->anniversary : PHistorical(); }

void VCard::setAnniversary(const PHistorical &anniversary)
{
    INIT_D();
    d->anniversary = anniversary;
}

VCard4::Gender VCard::gender() const { return d ? d->gender : VCard4::Gender::Undefined; }

void VCard::setGender(Gender gender)
{
    INIT_D();
    d->gender = gender;
}

QString VCard::genderComment() const { return d ? d->genderComment : QString(); }

void VCard::setGenderComment(const QString &comment)
{
    INIT_D();
    d->genderComment = comment;
}

QString VCard::uid() const { return d ? d->uid : QString(); }

void VCard::setUid(const QString &uid)
{
    INIT_D();
    d->uid = uid;
}

QString VCard::kind() const { return d ? d->kind : QString(); }

void VCard::setKind(const QString &kind)
{
    INIT_D();
    d->kind = kind;
}

PStringLists VCard::categories() const { return d ? d->categories : PStringLists(); }

void VCard::setCategories(const PStringLists &categories)
{
    INIT_D();
    d->categories = categories;
}

PUris VCard::busyTimeUrl() const { return d ? d->busyTimeUrl : PUris(); }

void VCard::setBusyTimeUrl(const PUris &busyTimeUrl)
{
    INIT_D();
    d->busyTimeUrl = busyTimeUrl;
}

PUris VCard::calendarRequestUri() const { return d ? d->calendarRequestUri : PUris(); }

void VCard::setCalendarRequestUri(const PUris &calendarRequestUri)
{
    INIT_D();
    d->calendarRequestUri = calendarRequestUri;
}

PUris VCard::calendarUri() const { return d ? d->calendarUri : PUris(); }

void VCard::setCalendarUri(const PUris &calendarUri)
{
    INIT_D();
    d->calendarUri = calendarUri;
}

QHash<int, QString> VCard::clientPidMap() const { return d ? d->clientPidMap : QHash<int, QString>(); }

void VCard::setClientPidMap(const QHash<int, QString> &clientPidMap)
{
    INIT_D();
    d->clientPidMap = clientPidMap;
}

PUris VCard::geo() const { return d ? d->geo : PUris(); }

void VCard::setGeo(const PUris &geo)
{
    INIT_D();
    d->geo = geo;
}

PUris VCard::impp() const { return d ? d->impp : PUris(); }

void VCard::setImpp(const PUris &impp)
{
    INIT_D();
    d->impp = impp;
}

PUrisOrTexts VCard::key() const { return d ? d->key : PUrisOrTexts(); }

void VCard::setKey(const PUrisOrTexts &key)
{
    INIT_D();
    d->key = key;
}

PStrings VCard::lang() const { return d ? d->lang : PStrings(); }

void VCard::setLang(const PStrings &lang)
{
    INIT_D();
    d->lang = lang;
}

PAdvUris VCard::logo() const { return d ? d->logo : PAdvUris(); }

void VCard::setLogo(const PAdvUris &logo)
{
    INIT_D();
    d->logo = logo;
}

PUris VCard::member() const { return d ? d->member : PUris(); }

void VCard::setMember(const PUris &member)
{
    INIT_D();
    d->member = member;
}

PAdvUris VCard::photo() const { return d ? d->photo : PAdvUris(); }

void VCard::setPhoto(const PAdvUris &photo)
{
    INIT_D();
    d->photo = photo;
}

QString VCard::prodid() const { return d ? d->prodid : QString(); }

void VCard::setProdid(const QString &prodid)
{
    INIT_D();
    d->prodid = prodid;
}

PUrisOrTexts VCard::related() const { return d ? d->related : PUrisOrTexts(); }

void VCard::setRelated(const PUrisOrTexts &related)
{
    INIT_D();
    d->related = related;
}

QDateTime VCard::rev() const { return d ? d->rev : QDateTime(); }

void VCard::setRev(const QDateTime &rev)
{
    INIT_D();
    d->rev = rev;
}

PAdvUris VCard::sound() const { return d ? d->sound : PAdvUris(); }

void VCard::setSound(const PAdvUris &sound)
{
    INIT_D();
    d->sound = sound;
}

PUris VCard::source() const { return d ? d->source : PUris(); }

void VCard::setSource(const PUris &source)
{
    INIT_D();
    d->source = source;
}

PTimeZones VCard::timeZone() const { return d ? d->timeZone : PTimeZones(); }

void VCard::setTimeZone(const PTimeZones &timeZone)
{
    INIT_D();
    d->timeZone = timeZone;
}

QList<std::pair<Parameters, Address>> VCard::addresses() const
{
    return d ? d->addresses : QList<std::pair<Parameters, Address>>();
}

void VCard::setAddresses(const QList<std::pair<Parameters, Address>> &addresses)
{
    INIT_D();
    d->addresses = addresses;
}

} // namespace VCard4
