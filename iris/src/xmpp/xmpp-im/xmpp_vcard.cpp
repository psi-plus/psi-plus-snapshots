/*
 * xmpp_vcard.cpp - classes for handling vCards
 * Copyright (C) 2003  Michail Pishchagin
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

#include "xmpp_vcard.h"

#include "xmpp_xmlcommon.h"

#include <QBuffer>
#include <QDateTime>
#include <QImage> // needed for image format recognition
#include <QImageReader>
#include <QImageWriter>
#include <QtCrypto>
#include <QtDebug>
#include <qdom.h>

using namespace XMLHelper;

//----------------------------------------------------------------------------
// VCard
//----------------------------------------------------------------------------
QString openedImage2type(QIODevice *dev)
{
    QString format = QImageReader::imageFormat(dev).toUpper();

    // TODO: add more formats: PBM PGM PPM XBM XPM
    if (format == QLatin1String("PNG") || format == QLatin1String("PSIPNG")) // PsiPNG in normal case
        return QLatin1String("image/png");
    if (format == QLatin1String("MNG"))
        return QLatin1String("video/x-mng");
    if (format == QLatin1String("GIF"))
        return QLatin1String("image/gif");
    if (format == QLatin1String("JPEG"))
        return QLatin1String("image/jpeg");
    if (format == QLatin1String("BMP"))
        return QLatin1String("image/bmp");
    if (format == QLatin1String("WEBP"))
        return QLatin1String("image/webp");
    if (format == QLatin1String("XPM"))
        return QLatin1String("image/x-xpm");
    if (format == QLatin1String("SVG"))
        return QLatin1String("image/svg+xml");

    return QString();
}

QString image2type(const QByteArray &ba)
{
    if (ba.isEmpty())
        return QString();
    QBuffer buf;
    buf.setData(ba);
    buf.open(QIODevice::ReadOnly);
    return openedImage2type(&buf);
}

namespace XMPP {
// Long lines of encoded binary data SHOULD BE folded to 75 characters using the folding method defined in [MIME-DIR].
static QString foldString(const QString &s)
{
    QString ret;

    for (int i = 0; i < (int)s.length(); i++) {
        if (!(i % 75))
            ret += '\n';
        ret += s[i];
    }

    return ret;
}

class VCardPrivate : public QSharedData {
public:
    VCardPrivate();
    ~VCardPrivate();

    // do we need copy constructor?
    // VCardPrivate(const VCardPrivate &other) :
    //    QSharedData(other), version(other.version), fullName(other.fullName) { qDebug("Copy VCardPrivate"); }

    QString version;
    QString fullName;
    QString familyName, givenName, middleName, prefixName, suffixName;
    QString nickName;

    QByteArray photo;
    QString    photoURI;

    QString            bday;
    VCard::AddressList addressList;
    VCard::LabelList   labelList;
    VCard::PhoneList   phoneList;
    VCard::EmailList   emailList;
    QString            jid;
    QString            mailer;
    QString            timezone;
    VCard::Geo         geo;
    QString            title;
    QString            role;

    QByteArray logo;
    QString    logoURI;

    QSharedPointer<VCard> agent;
    QString               agentURI;

    VCard::Org  org;
    QStringList categories;
    QString     note;
    QString     prodId;
    QString     rev;
    QString     sortString;

    QByteArray sound;
    QString    soundURI, soundPhonetic;

    QString             uid;
    QString             url;
    QString             desc;
    VCard::PrivacyClass privacyClass;
    QByteArray          key;

    bool isEmpty() const;
};

VCardPrivate::VCardPrivate() { privacyClass = VCard::pcNone; }

VCardPrivate::~VCardPrivate() { }

bool VCardPrivate::isEmpty() const
{
    return !(!version.isEmpty() || !fullName.isEmpty() || !familyName.isEmpty() || !givenName.isEmpty()
             || !middleName.isEmpty() || !prefixName.isEmpty() || !suffixName.isEmpty() || !nickName.isEmpty()
             || !photo.isEmpty() || !photoURI.isEmpty() || !bday.isEmpty() || !addressList.isEmpty()
             || !labelList.isEmpty() || !phoneList.isEmpty() || !emailList.isEmpty() || !jid.isEmpty()
             || !mailer.isEmpty() || !timezone.isEmpty() || !geo.lat.isEmpty() || !geo.lon.isEmpty() || !title.isEmpty()
             || !role.isEmpty() || !logo.isEmpty() || !logoURI.isEmpty() || (agent && !agent->isEmpty())
             || !agentURI.isEmpty() || !org.name.isEmpty() || !org.unit.isEmpty() || !categories.isEmpty()
             || !note.isEmpty() || !prodId.isEmpty() || !rev.isEmpty() || !sortString.isEmpty() || !sound.isEmpty()
             || !soundURI.isEmpty() || !soundPhonetic.isEmpty() || !uid.isEmpty() || !url.isEmpty() || !desc.isEmpty()
             || (privacyClass != VCard::pcNone) || !key.isEmpty());
}

VCard::VCard() { }

VCard::VCard(const VCard &from) : d(from.d) { }

VCard &VCard::operator=(const VCard &from)
{
    d = from.d;
    return *this;
}

VCard::~VCard() { }

QDomElement VCard::toXml(QDomDocument *doc) const
{
    QDomElement v = doc->createElementNS("vcard-temp", "vCard");

    if (!d->version.isEmpty())
        v.appendChild(textTag(doc, "VERSION", d->version));
    if (!d->fullName.isEmpty())
        v.appendChild(textTag(doc, "FN", d->fullName));

    if (!d->familyName.isEmpty() || !d->givenName.isEmpty() || !d->middleName.isEmpty() || !d->prefixName.isEmpty()
        || !d->suffixName.isEmpty()) {
        QDomElement w = doc->createElement("N");

        if (!d->familyName.isEmpty())
            w.appendChild(textTag(doc, "FAMILY", d->familyName));
        if (!d->givenName.isEmpty())
            w.appendChild(textTag(doc, "GIVEN", d->givenName));
        if (!d->middleName.isEmpty())
            w.appendChild(textTag(doc, "MIDDLE", d->middleName));
        if (!d->prefixName.isEmpty())
            w.appendChild(textTag(doc, "PREFIX", d->prefixName));
        if (!d->suffixName.isEmpty())
            w.appendChild(textTag(doc, "SUFFIX", d->suffixName));

        v.appendChild(w);
    }

    if (!d->nickName.isEmpty())
        v.appendChild(textTag(doc, "NICKNAME", d->nickName));

    if (!d->photo.isEmpty() || !d->photoURI.isEmpty()) {
        QDomElement w = doc->createElement("PHOTO");

        if (!d->photo.isEmpty()) {
            w.appendChild(textTag(doc, "TYPE", image2type(d->photo)));
            w.appendChild(textTag(doc, "BINVAL", foldString(QCA::Base64().arrayToString(d->photo))));
        } else if (!d->photoURI.isEmpty())
            w.appendChild(textTag(doc, "EXTVAL", d->photoURI));

        v.appendChild(w);
    }

    if (!d->bday.isEmpty())
        v.appendChild(textTag(doc, "BDAY", d->bday));

    if (!d->addressList.isEmpty()) {
        for (const auto &a : qAsConst(d->addressList)) {
            QDomElement w = doc->createElement("ADR");

            if (a.home)
                w.appendChild(emptyTag(doc, "HOME"));
            if (a.work)
                w.appendChild(emptyTag(doc, "WORK"));
            if (a.postal)
                w.appendChild(emptyTag(doc, "POSTAL"));
            if (a.parcel)
                w.appendChild(emptyTag(doc, "PARCEL"));
            if (a.dom)
                w.appendChild(emptyTag(doc, "DOM"));
            if (a.intl)
                w.appendChild(emptyTag(doc, "INTL"));
            if (a.pref)
                w.appendChild(emptyTag(doc, "PREF"));

            if (!a.pobox.isEmpty())
                w.appendChild(textTag(doc, "POBOX", a.pobox));
            if (!a.extaddr.isEmpty())
                w.appendChild(textTag(doc, "EXTADR", a.extaddr));
            if (!a.street.isEmpty())
                w.appendChild(textTag(doc, "STREET", a.street));
            if (!a.locality.isEmpty())
                w.appendChild(textTag(doc, "LOCALITY", a.locality));
            if (!a.region.isEmpty())
                w.appendChild(textTag(doc, "REGION", a.region));
            if (!a.pcode.isEmpty())
                w.appendChild(textTag(doc, "PCODE", a.pcode));
            if (!a.country.isEmpty())
                w.appendChild(textTag(doc, "CTRY", a.country));

            v.appendChild(w);
        }
    }

    if (!d->labelList.isEmpty()) {
        for (const auto &l : qAsConst(d->labelList)) {
            QDomElement w = doc->createElement("LABEL");

            if (l.home)
                w.appendChild(emptyTag(doc, "HOME"));
            if (l.work)
                w.appendChild(emptyTag(doc, "WORK"));
            if (l.postal)
                w.appendChild(emptyTag(doc, "POSTAL"));
            if (l.parcel)
                w.appendChild(emptyTag(doc, "PARCEL"));
            if (l.dom)
                w.appendChild(emptyTag(doc, "DOM"));
            if (l.intl)
                w.appendChild(emptyTag(doc, "INTL"));
            if (l.pref)
                w.appendChild(emptyTag(doc, "PREF"));

            if (!l.lines.isEmpty()) {
                for (const auto &it : qAsConst(l.lines))
                    w.appendChild(textTag(doc, "LINE", it));
            }

            v.appendChild(w);
        }
    }

    if (!d->phoneList.isEmpty()) {
        for (const auto &p : qAsConst(d->phoneList)) {
            QDomElement w = doc->createElement("TEL");

            if (p.home)
                w.appendChild(emptyTag(doc, "HOME"));
            if (p.work)
                w.appendChild(emptyTag(doc, "WORK"));
            if (p.voice)
                w.appendChild(emptyTag(doc, "VOICE"));
            if (p.fax)
                w.appendChild(emptyTag(doc, "FAX"));
            if (p.pager)
                w.appendChild(emptyTag(doc, "PAGER"));
            if (p.msg)
                w.appendChild(emptyTag(doc, "MSG"));
            if (p.cell)
                w.appendChild(emptyTag(doc, "CELL"));
            if (p.video)
                w.appendChild(emptyTag(doc, "VIDEO"));
            if (p.bbs)
                w.appendChild(emptyTag(doc, "BBS"));
            if (p.modem)
                w.appendChild(emptyTag(doc, "MODEM"));
            if (p.isdn)
                w.appendChild(emptyTag(doc, "ISDN"));
            if (p.pcs)
                w.appendChild(emptyTag(doc, "PCS"));
            if (p.pref)
                w.appendChild(emptyTag(doc, "PREF"));

            if (!p.number.isEmpty())
                w.appendChild(textTag(doc, "NUMBER", p.number));

            v.appendChild(w);
        }
    }

    if (!d->emailList.isEmpty()) {
        for (const auto &e : qAsConst(d->emailList)) {
            QDomElement w = doc->createElement("EMAIL");

            if (e.pref)
                w.appendChild(emptyTag(doc, "PREF"));
            if (e.home)
                w.appendChild(emptyTag(doc, "HOME"));
            if (e.work)
                w.appendChild(emptyTag(doc, "WORK"));
            if (e.internet)
                w.appendChild(emptyTag(doc, "INTERNET"));
            if (e.x400)
                w.appendChild(emptyTag(doc, "X400"));

            if (!e.userid.isEmpty())
                w.appendChild(textTag(doc, "USERID", e.userid));

            v.appendChild(w);
        }
    }

    if (!d->jid.isEmpty())
        v.appendChild(textTag(doc, "JABBERID", d->jid));
    if (!d->mailer.isEmpty())
        v.appendChild(textTag(doc, "MAILER", d->mailer));
    if (!d->timezone.isEmpty())
        v.appendChild(textTag(doc, "TZ", d->timezone));

    if (!d->geo.lat.isEmpty() || !d->geo.lon.isEmpty()) {
        QDomElement w = doc->createElement("GEO");

        if (!d->geo.lat.isEmpty())
            w.appendChild(textTag(doc, "LAT", d->geo.lat));
        if (!d->geo.lon.isEmpty())
            w.appendChild(textTag(doc, "LON", d->geo.lon));

        v.appendChild(w);
    }

    if (!d->title.isEmpty())
        v.appendChild(textTag(doc, "TITLE", d->title));
    if (!d->role.isEmpty())
        v.appendChild(textTag(doc, "ROLE", d->role));

    if (!d->logo.isEmpty() || !d->logoURI.isEmpty()) {
        QDomElement w = doc->createElement("LOGO");

        if (!d->logo.isEmpty()) {
            w.appendChild(textTag(doc, "TYPE", image2type(d->logo)));
            w.appendChild(textTag(doc, "BINVAL", foldString(QCA::Base64().arrayToString(d->logo))));
        } else if (!d->logoURI.isEmpty())
            w.appendChild(textTag(doc, "EXTVAL", d->logoURI));

        v.appendChild(w);
    }

    if (!d->agentURI.isEmpty() || (d->agent && d->agent->isEmpty())) {
        QDomElement w = doc->createElement("AGENT");

        if (d->agent && !d->agent->isEmpty())
            w.appendChild(d->agent->toXml(doc));
        else if (!d->agentURI.isEmpty())
            w.appendChild(textTag(doc, "EXTVAL", d->agentURI));

        v.appendChild(w);
    }

    if (!d->org.name.isEmpty() || !d->org.unit.isEmpty()) {
        QDomElement w = doc->createElement("ORG");

        if (!d->org.name.isEmpty())
            w.appendChild(textTag(doc, "ORGNAME", d->org.name));

        if (!d->org.unit.isEmpty()) {
            for (const auto &unit : qAsConst(d->org.unit))
                w.appendChild(textTag(doc, "ORGUNIT", unit));
        }

        v.appendChild(w);
    }

    if (!d->categories.isEmpty()) {
        QDomElement w = doc->createElement("CATEGORIES");

        for (const auto &c : qAsConst(d->categories))
            w.appendChild(textTag(doc, "KEYWORD", c));

        v.appendChild(w);
    }

    if (!d->note.isEmpty())
        v.appendChild(textTag(doc, "NOTE", d->note));
    if (!d->prodId.isEmpty())
        v.appendChild(textTag(doc, "PRODID", d->prodId));
    if (!d->rev.isEmpty())
        v.appendChild(textTag(doc, "REV", d->rev));
    if (!d->sortString.isEmpty())
        v.appendChild(textTag(doc, "SORT-STRING", d->sortString));

    if (!d->sound.isEmpty() || !d->soundURI.isEmpty() || !d->soundPhonetic.isEmpty()) {
        QDomElement w = doc->createElement("SOUND");

        if (!d->sound.isEmpty())
            w.appendChild(textTag(doc, "BINVAL", foldString(QCA::Base64().arrayToString(d->sound))));
        else if (!d->soundURI.isEmpty())
            w.appendChild(textTag(doc, "EXTVAL", d->soundURI));
        else if (!d->soundPhonetic.isEmpty())
            w.appendChild(textTag(doc, "PHONETIC", d->soundPhonetic));

        v.appendChild(w);
    }

    if (!d->uid.isEmpty())
        v.appendChild(textTag(doc, "UID", d->uid));
    if (!d->url.isEmpty())
        v.appendChild(textTag(doc, "URL", d->url));
    if (!d->desc.isEmpty())
        v.appendChild(textTag(doc, "DESC", d->desc));

    if (d->privacyClass != pcNone) {
        QDomElement w = doc->createElement("CLASS");

        if (d->privacyClass == pcPublic)
            w.appendChild(emptyTag(doc, "PUBLIC"));
        else if (d->privacyClass == pcPrivate)
            w.appendChild(emptyTag(doc, "PRIVATE"));
        else if (d->privacyClass == pcConfidential)
            w.appendChild(emptyTag(doc, "CONFIDENTIAL"));

        v.appendChild(w);
    }

    if (!d->key.isEmpty()) {
        QDomElement w = doc->createElement("KEY");

        // TODO: Justin, please check out this code
        w.appendChild(textTag(doc, "TYPE", "text/plain"));              // FIXME
        w.appendChild(textTag(doc, "CRED", QString::fromUtf8(d->key))); // FIXME

        v.appendChild(w);
    }

    return v;
}

VCard VCard::fromXml(const QDomElement &q)
{
    if (q.tagName().toUpper() != "VCARD")
        return VCard();

    VCard v;
    v.d = new VCardPrivate;

    QDomNode n = q.firstChild();
    for (; !n.isNull(); n = n.nextSibling()) {
        QDomElement i = n.toElement();
        if (i.isNull())
            continue;

        QString tag = i.tagName().toUpper();

        QDomElement e;

        if (tag == "VERSION")
            v.d->version = i.text().trimmed();
        else if (tag == "FN")
            v.d->fullName = i.text().trimmed();
        else if (tag == "N") {
            v.d->familyName = subTagText(i, "FAMILY");
            v.d->givenName  = subTagText(i, "GIVEN");
            v.d->middleName = subTagText(i, "MIDDLE");
            v.d->prefixName = subTagText(i, "PREFIX");
            v.d->suffixName = subTagText(i, "SUFFIX");
        } else if (tag == "NICKNAME")
            v.d->nickName = i.text().trimmed();
        else if (tag == "PHOTO") {
            v.d->photo
                = QCA::Base64().stringToArray(subTagText(i, "BINVAL").replace(QRegExp("[\r\n]+"), "")).toByteArray();
            v.d->photoURI = subTagText(i, "EXTVAL");
        } else if (tag == "BDAY")
            v.d->bday = i.text().trimmed();
        else if (tag == "ADR") {
            Address a;

            a.home   = hasSubTag(i, "HOME");
            a.work   = hasSubTag(i, "WORK");
            a.postal = hasSubTag(i, "POSTAL");
            a.parcel = hasSubTag(i, "PARCEL");
            a.dom    = hasSubTag(i, "DOM");
            a.intl   = hasSubTag(i, "INTL");
            a.pref   = hasSubTag(i, "PREF");

            a.pobox    = subTagText(i, "POBOX");
            a.extaddr  = subTagText(i, "EXTADR");
            a.street   = subTagText(i, "STREET");
            a.locality = subTagText(i, "LOCALITY");
            a.region   = subTagText(i, "REGION");
            a.pcode    = subTagText(i, "PCODE");
            a.country  = subTagText(i, "CTRY");

            if (a.country.isEmpty()) // FIXME: Workaround for Psi prior to 0.9
                if (hasSubTag(i, "COUNTRY"))
                    a.country = subTagText(i, "COUNTRY");

            if (a.extaddr.isEmpty()) // FIXME: Workaround for Psi prior to 0.9
                if (hasSubTag(i, "EXTADD"))
                    a.extaddr = subTagText(i, "EXTADD");

            v.d->addressList.append(a);
        } else if (tag == "LABEL") {
            Label l;

            l.home   = hasSubTag(i, "HOME");
            l.work   = hasSubTag(i, "WORK");
            l.postal = hasSubTag(i, "POSTAL");
            l.parcel = hasSubTag(i, "PARCEL");
            l.dom    = hasSubTag(i, "DOM");
            l.intl   = hasSubTag(i, "INTL");
            l.pref   = hasSubTag(i, "PREF");

            QDomNode nn = i.firstChild();
            for (; !nn.isNull(); nn = nn.nextSibling()) {
                QDomElement ii = nn.toElement();
                if (ii.isNull())
                    continue;

                if (ii.tagName().toUpper() == "LINE")
                    l.lines.append(ii.text().trimmed());
            }

            v.d->labelList.append(l);
        } else if (tag == "TEL") {
            Phone p;

            p.home  = hasSubTag(i, "HOME");
            p.work  = hasSubTag(i, "WORK");
            p.voice = hasSubTag(i, "VOICE");
            p.fax   = hasSubTag(i, "FAX");
            p.pager = hasSubTag(i, "PAGER");
            p.msg   = hasSubTag(i, "MSG");
            p.cell  = hasSubTag(i, "CELL");
            p.video = hasSubTag(i, "VIDEO");
            p.bbs   = hasSubTag(i, "BBS");
            p.modem = hasSubTag(i, "MODEM");
            p.isdn  = hasSubTag(i, "ISDN");
            p.pcs   = hasSubTag(i, "PCS");
            p.pref  = hasSubTag(i, "PREF");

            p.number = subTagText(i, "NUMBER");

            if (!p.number.isEmpty()) {
                v.d->phoneList.append(p);

                auto it = std::find_if(v.d->phoneList.begin(), v.d->phoneList.end(),
                                       [number = p.number](const Phone &p) { return p.number == number; });
                if (it == v.d->phoneList.end()) {
                    v.d->phoneList.append(p);
                } else {
                    it->home  = (it->home || p.home);
                    it->work  = (it->work || p.work);
                    it->voice = (it->voice || p.voice);
                    it->fax   = (it->fax || p.fax);
                    it->pager = (it->pager || p.pager);
                    it->msg   = (it->msg || p.msg);
                    it->cell  = (it->cell || p.cell);
                    it->video = (it->video || p.video);
                    it->bbs   = (it->bbs || p.bbs);
                    it->modem = (it->modem || p.modem);
                    it->isdn  = (it->isdn || p.isdn);
                    it->pcs   = (it->pcs || p.pcs);
                    it->pref  = (it->pref || p.pref);
                }
            }
        } else if (tag == "EMAIL") {
            Email m;

            m.home     = hasSubTag(i, "HOME");
            m.work     = hasSubTag(i, "WORK");
            m.internet = hasSubTag(i, "INTERNET");
            m.x400     = hasSubTag(i, "X400");

            m.pref = hasSubTag(i, "PREF");

            m.userid = subTagText(i, "USERID").trimmed();
            if (!m.userid.isEmpty()) {
                auto it = std::find_if(v.d->emailList.begin(), v.d->emailList.end(),
                                       [user_id = m.userid](const Email &e) { return e.userid == user_id; });
                if (it == v.d->emailList.end()) {
                    v.d->emailList.append(m);
                } else {
                    it->home     = (it->home || m.home);
                    it->work     = (it->work || m.work);
                    it->internet = (it->internet || m.internet);
                    it->x400     = (it->x400 || m.x400);
                    it->pref     = (it->pref || m.pref);
                }
            }

        } else if (tag == "JABBERID")
            v.d->jid = i.text().trimmed();
        else if (tag == "MAILER")
            v.d->mailer = i.text().trimmed();
        else if (tag == "TZ")
            v.d->timezone = i.text().trimmed();
        else if (tag == "GEO") {
            v.d->geo.lat = subTagText(i, "LAT");
            v.d->geo.lon = subTagText(i, "LON");
        } else if (tag == "TITLE")
            v.d->title = i.text().trimmed();
        else if (tag == "ROLE")
            v.d->role = i.text().trimmed();
        else if (tag == "LOGO") {
            v.d->logo    = QCA::Base64().stringToArray(subTagText(i, "BINVAL").replace("\n", "")).toByteArray();
            v.d->logoURI = subTagText(i, "EXTVAL");
        } else if (tag == "AGENT") {
            e = i.firstChildElement("VCARD");
            if (!e.isNull()) {
                VCard a;
                if (a.fromXml(e)) {
                    if (!v.d->agent)
                        v.d->agent = QSharedPointer<VCard>(new VCard);
                    *(v.d->agent) = a;
                }
            }

            v.d->agentURI = subTagText(i, "EXTVAL");
        } else if (tag == "ORG") {
            v.d->org.name = subTagText(i, "ORGNAME");

            QDomNode nn = i.firstChild();
            for (; !nn.isNull(); nn = nn.nextSibling()) {
                QDomElement ii = nn.toElement();
                if (ii.isNull())
                    continue;

                if (ii.tagName().toUpper() == "ORGUNIT")
                    v.d->org.unit.append(ii.text().trimmed());
            }
        } else if (tag == "CATEGORIES") {
            QDomNode nn = i.firstChild();
            for (; !nn.isNull(); nn = nn.nextSibling()) {
                QDomElement ee = nn.toElement();
                if (ee.isNull())
                    continue;

                if (ee.tagName().toUpper() == "KEYWORD")
                    v.d->categories << ee.text().trimmed();
            }
        } else if (tag == "NOTE")
            v.d->note = i.text().trimmed();
        else if (tag == "PRODID")
            v.d->prodId = i.text().trimmed();
        else if (tag == "REV")
            v.d->rev = i.text().trimmed();
        else if (tag == "SORT-STRING")
            v.d->sortString = i.text().trimmed();
        else if (tag == "SOUND") {
            v.d->sound         = QCA::Base64().stringToArray(subTagText(i, "BINVAL").replace("\n", "")).toByteArray();
            v.d->soundURI      = subTagText(i, "EXTVAL");
            v.d->soundPhonetic = subTagText(i, "PHONETIC");
        } else if (tag == "UID")
            v.d->uid = i.text().trimmed();
        else if (tag == "URL")
            v.d->url = i.text().trimmed();
        else if (tag == "DESC")
            v.d->desc = i.text().trimmed();
        else if (tag == "CLASS") {
            if (hasSubTag(i, "PUBLIC"))
                v.d->privacyClass = pcPublic;
            else if (hasSubTag(i, "PRIVATE"))
                v.d->privacyClass = pcPrivate;
            else if (hasSubTag(i, "CONFIDENTIAL"))
                v.d->privacyClass = pcConfidential;
        } else if (tag == "KEY") {
            // TODO: Justin, please check out this code
            e            = i.firstChildElement("TYPE");
            QString type = "text/plain";
            if (!e.isNull())
                type = e.text().trimmed();

            e = i.firstChildElement("CRED");
            if (e.isNull())
                e = i.firstChildElement("BINVAL"); // case for very clever clients ;-)

            if (!e.isNull())
                v.d->key = e.text().toUtf8(); // FIXME
        }
    }

    return v;
}

bool VCard::isEmpty() const { return !d || d->isEmpty(); }

VCard VCard::makeEmpty()
{
    VCard vcard;
    vcard.d = new VCardPrivate;
    return vcard;
}

// Some constructors

VCard::Address::Address() { home = work = postal = parcel = dom = intl = pref = false; }

VCard::Label::Label() { home = work = postal = parcel = dom = intl = pref = false; }

VCard::Phone::Phone()
{
    home = work = voice = fax = pager = msg = cell = video = bbs = modem = isdn = pcs = pref = false;
}

VCard::Email::Email() { home = work = internet = x400 = pref = false; }

VCard::Geo::Geo() { }

VCard::Org::Org() { }

// vCard properties...

const QString &VCard::version() const { return d->version; }

void VCard::setVersion(const QString &v) { d->version = v; }

const QString &VCard::fullName() const { return d->fullName; }

void VCard::setFullName(const QString &n) { d->fullName = n; }

const QString &VCard::familyName() const { return d->familyName; }

void VCard::setFamilyName(const QString &n) { d->familyName = n; }

const QString &VCard::givenName() const { return d->givenName; }

void VCard::setGivenName(const QString &n) { d->givenName = n; }

const QString &VCard::middleName() const { return d->middleName; }

void VCard::setMiddleName(const QString &n) { d->middleName = n; }

const QString &VCard::prefixName() const { return d->prefixName; }

void VCard::setPrefixName(const QString &p) { d->prefixName = p; }

const QString &VCard::suffixName() const { return d->suffixName; }

void VCard::setSuffixName(const QString &s) { d->suffixName = s; }

const QString &VCard::nickName() const { return d->nickName; }

void VCard::setNickName(const QString &n) { d->nickName = n; }

const QByteArray &VCard::photo() const { return d->photo; }

void VCard::setPhoto(const QByteArray &i) { d->photo = i; }

const QString &VCard::photoURI() const { return d->photoURI; }

void VCard::setPhotoURI(const QString &p) { d->photoURI = p; }

const QDate VCard::bday() const { return QDate::fromString(d->bday); }

void VCard::setBday(const QDate &date) { d->bday = date.toString(); }

const QString &VCard::bdayStr() const { return d->bday; }

void VCard::setBdayStr(const QString &date) { d->bday = date; }

const VCard::AddressList &VCard::addressList() const { return d->addressList; }

void VCard::setAddressList(const VCard::AddressList &a) { d->addressList = a; }

const VCard::LabelList &VCard::labelList() const { return d->labelList; }

void VCard::setLabelList(const VCard::LabelList &l) { d->labelList = l; }

const VCard::PhoneList &VCard::phoneList() const { return d->phoneList; }

void VCard::setPhoneList(const VCard::PhoneList &p) { d->phoneList = p; }

const VCard::EmailList &VCard::emailList() const { return d->emailList; }

void VCard::setEmailList(const VCard::EmailList &e) { d->emailList = e; }

const QString &VCard::jid() const { return d->jid; }

void VCard::setJid(const QString &j) { d->jid = j; }

const QString &VCard::mailer() const { return d->mailer; }

void VCard::setMailer(const QString &m) { d->mailer = m; }

const QString &VCard::timezone() const { return d->timezone; }

void VCard::setTimezone(const QString &t) { d->timezone = t; }

const VCard::Geo &VCard::geo() const { return d->geo; }

void VCard::setGeo(const VCard::Geo &g) { d->geo = g; }

const QString &VCard::title() const { return d->title; }

void VCard::setTitle(const QString &t) { d->title = t; }

const QString &VCard::role() const { return d->role; }

void VCard::setRole(const QString &r) { d->role = r; }

const QByteArray &VCard::logo() const { return d->logo; }

void VCard::setLogo(const QByteArray &i) { d->logo = i; }

const QString &VCard::logoURI() const { return d->logoURI; }

void VCard::setLogoURI(const QString &l) { d->logoURI = l; }

VCard VCard::agent() const
{
    if (d->agent) {
        return *(d->agent); // implicit copy
    }
    return VCard();
}

void VCard::setAgent(const VCard &v)
{
    if (!d->agent)
        d->agent = QSharedPointer<VCard>(new VCard);
    *(d->agent) = v;
}

const QString VCard::agentURI() const { return d->agentURI; }

void VCard::setAgentURI(const QString &a) { d->agentURI = a; }

const VCard::Org &VCard::org() const { return d->org; }

void VCard::setOrg(const VCard::Org &o) { d->org = o; }

const QStringList &VCard::categories() const { return d->categories; }

void VCard::setCategories(const QStringList &c) { d->categories = c; }

const QString &VCard::note() const { return d->note; }

void VCard::setNote(const QString &n) { d->note = n; }

const QString &VCard::prodId() const { return d->prodId; }

void VCard::setProdId(const QString &p) { d->prodId = p; }

const QString &VCard::rev() const { return d->rev; }

void VCard::setRev(const QString &r) { d->rev = r; }

const QString &VCard::sortString() const { return d->sortString; }

void VCard::setSortString(const QString &s) { d->sortString = s; }

const QByteArray &VCard::sound() const { return d->sound; }

void VCard::setSound(const QByteArray &s) { d->sound = s; }

const QString &VCard::soundURI() const { return d->soundURI; }

void VCard::setSoundURI(const QString &s) { d->soundURI = s; }

const QString &VCard::soundPhonetic() const { return d->soundPhonetic; }

void VCard::setSoundPhonetic(const QString &s) { d->soundPhonetic = s; }

const QString &VCard::uid() const { return d->uid; }

void VCard::setUid(const QString &u) { d->uid = u; }

const QString &VCard::url() const { return d->url; }

void VCard::setUrl(const QString &u) { d->url = u; }

const QString &VCard::desc() const { return d->desc; }

void VCard::setDesc(const QString &desc) { d->desc = desc; }

const VCard::PrivacyClass &VCard::privacyClass() const { return d->privacyClass; }

void VCard::setPrivacyClass(const VCard::PrivacyClass &c) { d->privacyClass = c; }

const QByteArray &VCard::key() const { return d->key; }

void VCard::setKey(const QByteArray &k) { d->key = k; }
} // namespace XMPP
