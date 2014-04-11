/*
 * userlist.cpp - high-level roster
 * Copyright (C) 2001, 2002  Justin Karneges
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <QApplication>
#include <QPixmap>
#include <QList>
#include <QtCrypto>
#include <QTextDocument> // for TextUtil::escape()
#include <QBuffer>
#include <QUrl>

#include "userlist.h"
#include "avatars.h"
#include "textutil.h"
#include "common.h"
#include "mucmanager.h"
#include "psioptions.h"
#include "jidutil.h"
#include "psiiconset.h"

using namespace XMPP;

static QString dot_truncate(const QString &in, int clip)
{
	if((int)in.length() <= clip)
		return in;
	QString s = in;
	s.truncate(clip);
	s += "...";
	return s;
}

//----------------------------------------------------------------------------
// UserResource
//----------------------------------------------------------------------------
UserResource::UserResource()
:Resource()
{
}

UserResource::UserResource(const Resource &r)
{
	setResource(r);
}

UserResource::~UserResource()
{
}

void UserResource::setResource(const Resource &r)
{
	setName(r.name());
	setStatus(r.status());
}

const QString & UserResource::versionString() const
{
	return v_ver;
}

const QString & UserResource::clientName() const
{
	return v_clientName;
}

const QString & UserResource::clientVersion() const
{
	return v_clientVersion;
}

const QString & UserResource::clientOS() const
{
	return v_clientOS;
}

void UserResource::setClient(const QString &name, const QString& version, const QString& os)
{
	v_clientName = name;
	v_clientVersion = version;
	v_clientOS = os;
	if (!v_clientName.isEmpty()) {
		v_ver = v_clientName + " " + v_clientVersion;
		if ( !v_clientOS.isEmpty() )
			v_ver += " / " + v_clientOS;
	}
	else {
		v_ver = "";
	}
}

/**
 * \brief Timezone offset in minutes (if available).
 */
Maybe<int> UserResource::timezoneOffset() const
{
	return v_tzo;
}

/**
 * \brief Timezone offset as string (or empty string if no data).
 *
 * String is formatted as "UTC[+|-]h[:mm]".
 */
const QString& UserResource::timezoneOffsetString() const
{
	return v_tzoString;
}

/**
 * \brief Set timezone offset (in minutes).
 */
void UserResource::setTimezone(Maybe<int> off)
{
	v_tzo = off;

	if (off.hasValue()) {
		QTime t = QTime(0, 0).addSecs(abs(off.value())*60);
		QString u = QString("UTC") + (off.value() < 0 ? "-" : "+");
		u += QString::number(t.hour());
		if (t.minute())
			u += QString(":%1").arg(t.minute());
		v_tzoString = u;
	}
	else
		v_tzoString = "";
}

const QString & UserResource::publicKeyID() const
{
	return v_keyID;
}

int UserResource::pgpVerifyStatus() const
{
	return v_pgpVerifyStatus;
}

QDateTime UserResource::sigTimestamp() const
{
	return sigts;
}

void UserResource::setPublicKeyID(const QString &s)
{
	v_keyID = s;
}

void UserResource::setPGPVerifyStatus(int x)
{
	v_pgpVerifyStatus = x;
}

void UserResource::setSigTimestamp(const QDateTime &ts)
{
	sigts = ts;
}

void UserResource::setTune(const QString& t)
{
	v_tune = t;
}

const QString& UserResource::tune() const
{
	return v_tune;
}

void UserResource::setGeoLocation(const GeoLocation& geoLocation)
{
	v_geoLocation = geoLocation;
}

const GeoLocation& UserResource::geoLocation() const
{
	return v_geoLocation;
}

/*void UserResource::setPhysicalLocation(const PhysicalLocation& physicalLocation)
{
	v_physicalLocation = physicalLocation;
}

const PhysicalLocation& UserResource::physicalLocation() const
{
	return v_physicalLocation;
}*/


bool operator<(const UserResource &r1, const UserResource &r2)
{
	return r1.priority() > r2.priority();
}

bool operator<=(const UserResource &r1, const UserResource &r2)
{
	return r1.priority() >= r2.priority();
}

bool operator==(const UserResource &r1, const UserResource &r2)
{
	return r1.priority() == r2.priority();
}

bool operator>(const UserResource &r1, const UserResource &r2)
{
	return r1.priority() < r2.priority();
}

bool operator>=(const UserResource &r1, const UserResource &r2)
{
	return r1.priority() <= r2.priority();
}


//----------------------------------------------------------------------------
// UserResourceList
//----------------------------------------------------------------------------
UserResourceList::UserResourceList()
:QList<UserResource>()
{
}

UserResourceList::~UserResourceList()
{
}

UserResourceList::Iterator UserResourceList::find(const QString & _find)
{
	for(UserResourceList::Iterator it = begin(); it != end(); ++it) {
		if((*it).name() == _find)
			return it;
	}

	return end();
}

UserResourceList::Iterator UserResourceList::priority()
{
	UserResourceList::Iterator highest = end();

	for(UserResourceList::Iterator it = begin(); it != end(); ++it) {
		if(highest == end() || (*it).priority() > (*highest).priority())
			highest = it;
	}

	return highest;
}

UserResourceList::ConstIterator UserResourceList::find(const QString & _find) const
{
	for(UserResourceList::ConstIterator it = begin(); it != end(); ++it) {
		if((*it).name() == _find)
			return it;
	}

	return end();
}

UserResourceList::ConstIterator UserResourceList::priority() const
{
	UserResourceList::ConstIterator highest = end();

	for(UserResourceList::ConstIterator it = begin(); it != end(); ++it) {
		if(highest == end() || (*it).priority() > (*highest).priority())
			highest = it;
	}

	return highest;
}

void UserResourceList::sort()
{
	qSort(*this);
}


//----------------------------------------------------------------------------
// UserListItem
//----------------------------------------------------------------------------
UserListItem::UserListItem(bool self)
{
	v_inList = false;
	v_self = self;
	v_private = false;
	v_isConference = false;
	v_avatarFactory = NULL;
	lastmsgtype = -1;
	v_pending = 0;
	v_hPending = 0;
}

UserListItem::~UserListItem()
{
}

bool UserListItem::inList() const
{
	return v_inList;
}

void UserListItem::setMood(const Mood& mood)
{
	v_mood = mood;
}

const Mood& UserListItem::mood() const
{
	return v_mood;
}

QStringList UserListItem::clients() const
{
	QStringList res;

	//if(isMuc()) return res; //temporary commented out until necessary patches will be fixed
	if(!userResourceList().isEmpty()) {
		UserResourceList srl = userResourceList();
		srl.sort();

		for(UserResourceList::ConstIterator rit = srl.begin(); rit != srl.end(); ++rit) {
			QString name = (*rit).clientName().toLower();
			res += findClient(name);
		}
	}
	return res;
}

QString UserListItem::findClient(QString name) const
{
	QString res;
	if(name.startsWith("adium"))
		res = "adium";
	else if(name.startsWith("google talk user account") || name.contains("android.com"))
		res = "android";
	else if(name.startsWith("simpleapps.ru"))
		res = "antihack-bot";
	else if(name.startsWith("aqq"))
		res = "aqq";
	else if(name.startsWith("asterisk"))
		res = "asterisk";
	else if(name.startsWith("bayanicq") || name.startsWith("barobin.com"))
		res = "bayanicq";
	else if(name.startsWith("barracuda"))
		res = "barracuda-im";
	else if(name.startsWith("beem-project"))
		res = "beem";
	else if(name.startsWith("bitlbee"))
		res = "bitlbee";
	else if((name.startsWith("simpleapps.ru") && name.contains("blacksmith")) || name.startsWith("blacksmith"))
		res = "blacksmith-bot";
	else if(name.startsWith("bluejabb"))
		res = "bluejabb";
	else if(name.startsWith("[bombus]") || name.contains("avalon"))
		res = "bombus-avalon";
	else if(name.startsWith("java.util.Random"))
		res = "bombus-avalon-old";
	else if(name.startsWith("klub54.wen.ru") || name.startsWith("bombusklub") || name.startsWith("jabber.pdg.pl"))
		res = "bombus-klub";
	else if(name.startsWith("bombus-im.org") && name.contains("java"))
		res = "bombus-old";
	else if(name.startsWith("bombusmod-qd.wen.ru") || name.startsWith("bombusqd"))
		res = "bombusqd";
	else if(name.startsWith("bombusng-qd.googlecode.com"))
		res = "bombusqd-ng";
	else if(name.startsWith("bombus-im.org") && name.contains("android"))
		res = "bombuslime";
	else if(name.startsWith("bombusmod"))
		res = "bombusmod";
	else if(name.startsWith("BombusMod.net.ru") || name.startsWith("ex-im.name"))
		res = "bombusmod-old";
	else if(name.startsWith("bombusng-md"))
		res = "bombusng-md";
	else if(name.startsWith("bombus-ng"))
		res = "bombusng";
	else if(name.startsWith("bombus.pl"))
		res = "bombuspl";
	else if(name.startsWith("bombus+") || name.startsWith("voffk.org.ru"))
		res = "bombusplus";
	else if(name.startsWith("bombus-im.org") || name.startsWith("bombus"))
		res = "bombus";
	else if(name.startsWith("jame") || name.startsWith("jabrss") || name.startsWith("pako bot") || name.startsWith("fatal-bot") || name.startsWith("storm") || name.startsWith("sulci") || name.startsWith("sleekbot") || name.startsWith("sofserver") || name.startsWith("neutrina") || name.startsWith("yamaneko") || name.startsWith("talisman") || name.contains(QString::fromUtf8(QByteArray::fromHex("efbbbfd0b3d0bed0b2d0bdd0bed0b1d0bed182"))) || name.startsWith(QString::fromUtf8(QByteArray::fromHex("efbbbfcf84ceb1cebbceb9cf82cebcceb1ceb7"))))
		res = "bot";
	else if(name.startsWith("buddydroid"))
		res = "buddydroid";
	else if(name.startsWith("fin.jabber.ru"))
		res = "capsula-bot";
	else if(name.startsWith("centerim"))
		res = "centerim";
	else if(name.startsWith("chatopus.com"))
		res = "chatopus";
	else if(name.startsWith("coccinella"))
		res = "coccinella";
	else if(name.startsWith("dictbot"))
		res = "dictbot";
	else if(name.startsWith("Digsby Client"))
		res = "digsby";
	else if(name.startsWith("ekg2"))
		res = "ekg2";
	else if(name.startsWith("emess"))
		res = "emess";
	else if(name.startsWith("emess.eqx.su"))
		res = "emess-old";
	else if(name.startsWith("erlim.a7x-im.com"))
		res = "erlim";
	else if(name.startsWith("exodus"))
		res = "exodus";
	else if((name.startsWith("svn.posix.ru") && name.contains("fatal-bot")) || name.startsWith("fatal-"))
		res = "fatal-bot";
	else if(name.startsWith("hat.freize.org"))
		res = "freize";
	else if(name.startsWith("freomessenger.com"))
		res = "freo";
	else if(name.startsWith("freqbot"))
		res = "freqbot";
	else if(name.startsWith("google.com 1.0.0.66"))
		res = "fring";
	else if(name.startsWith("gaim"))
		res = "gaim";
	else if(name.startsWith("gajim"))
		res = "gajim";
	else if(name.startsWith("j-cool.ru"))
		res = "gamebot";
	else if(name.startsWith("weather2jabber") || name.contains("gismeteo.ru"))
		res = "gismeteo";
	else if(name.startsWith("gizmo"))
		res = "gizmo";
	else if(name.startsWith("camaya.net") && name.contains("gloox"))
		res = "gloox";
	else if(name.startsWith("glu.net"))
		res = "glu";
	else if(name.startsWith("gluxibot"))
		res = "gluxibot";
	else if(name.startsWith("google.com") && name.contains("client"))
		res = "gtalk";
	else if(name.startsWith("android.com") && name.contains("gtalk"))
		res = "gtalk-android";
	else if(name.startsWith("habahaba.im"))
		res = "habahaba";
	else if(name.startsWith("hipchat.com"))
		res = "hipchat";
	else if(name.startsWith("aspro.users.ru") && name.contains("historian-bot"))
		res = "historian-bot";
	else if(name.startsWith("ichat") || name.contains("apple.com"))
		res = "ichat";
	else if(name.startsWith("icq mobile"))
		res = "icq-mobile";
	else if(name.startsWith("imadering"))
		res = "imadering";
	else if(name.startsWith("imov"))
		res = "imov";
	else if(name.startsWith("imformer.ru"))
		res = "imformer-bot";
	else if(name.startsWith("isida"))
		res = "isida-bot";
	else if(name.startsWith("jabber.el"))
		res = "jabber.el";
	else if(name.startsWith("memegenerator.net") && name.contains("bolgenos-popov"))
		res = "jabber-popov";
	else if(name.contains("jabbim"))
		res = "jabbim";
	else if(name.startsWith("jabbin"))
		res = "jabbin";
	else if(name.startsWith("jabbroid"))
		res = "jabbroid";
	else if(name.startsWith("jabiru"))
		res = "jabiru";
	else if(name.startsWith("jajc") || name.startsWith("just another jabber client"))
		res = "jajc";
	else if(name.startsWith("qabber.ru") && name.contains("jame-bot"))
		res = "jamebot";
	else if(name.startsWith("jappix"))
		res = "jappix";
	else if(name.contains("japyt"))
		res = "japyt";
	else if(name.startsWith("jasmineicq.ru"))
		res = "jasmine";
	else if(name.startsWith("jimm.net.ru") && name.contains("android"))
		res = "jimm-android";
	else if(name.startsWith("jimm"))
		res = "jimm-aspro";
	else if(name.startsWith("jitsi.org"))
		res = "jitsi";
	else if(name.startsWith("jbot"))
		res = "jbother";
	else if(name.startsWith("pjc"))
		res = "jubo";
	else if(name.startsWith("jtalk"))
		res = "jtalk";
	else if(name.startsWith("juick"))
		res = "juick";
	else if(name.startsWith("kadu"))
		res = "kadu";
	else if(name.startsWith("bluendo") || name.startsWith("lampiro"))
		res = "lampiro";
	else if(name.startsWith("leechcraft"))
		res = "leechcraft-azoth";
	else if(name.startsWith("libpurple"))
		res = "libpurple";
	else if(name.startsWith("pidgin.im"))
		res = "libpurple-old";
	else if(name.startsWith("irssi-xmpp"))
		res = "loudmouth";
	else if(name.startsWith("kopete"))
		res = "kopete";
	else if(name.startsWith("magnet2.ru"))
		res = "magnet2-bot";
	else if(name.startsWith("mail.google.com"))
		res = "mail.google.com";
	else if(name.startsWith("mrim") || name.startsWith("svn.xmpp.ru") || name.startsWith("none") || name.contains("mail.ru") || name.contains("list.ru") || name.contains("bk.ru") || name.contains("inbox.ru"))
		res = "mailruagent";
	else if(name.startsWith("mobileagent"))
		res = "mailruagent.sis";
	else if(name.startsWith("mobile mail agent"))
		res = "mailruagent.mob";
	else if(name.startsWith("tomclaw.com") && name.contains("mandarin_im"))
		res = "mandarin";
	else if(name.startsWith("mcabber"))
		res = "mcabber";
	else if(name.startsWith("mchat"))
		res = "mchat";
	else if(name.startsWith("meebo"))
		res = "meebo";
	else if(name.startsWith("code.google.com") && name.contains("qxmpp"))
		res = "meegim";
	else if(name.startsWith("megafonvolga.ru"))
		res = "megafon";
	else if(name.startsWith("miranda"))
		res = "miranda";
	else if(name.startsWith("nightly.miranda.im") || name.startsWith("miranda-ng.org"))
		res = "miranda-ng";
	else if(name.startsWith("hotcoffee"))
		res = "mirandahotcoffee";
	else if(name.startsWith("monal.im"))
		res = "monal";
	else if(name.startsWith("movamessenger"))
		res = "movamessenger.sis";
	else if(name.startsWith("msn") || name.startsWith("delx.net.au"))
		res = "msnmessenger";
	else if(name.startsWith("sleekxmpp.com") && name.contains("1.1.10"))
		res = "nekbot";
	else if(name.startsWith("nimbuzz"))
		res = "nimbuzz";
	else if(name.startsWith("omnipresence") || name.startsWith("home.gna.org"))
		res = "omnipresence";
	else if(name.startsWith("om"))
		res = "om.beeonline.ru";
	else if(name.startsWith("process-one.net"))
		res = "oneteamiphone";
	else if(name.startsWith("oneteam"))
		res = "oneteam";
	else if(name.startsWith("osiris"))
		res = "osiris";
	else if(name.startsWith("chat.ovi.com") || name.startsWith("chat.nokia.com") || name.startsWith("nokiachat") || name.startsWith("ovi contacts"))
		res = "ovi-chat";
	else if(name.startsWith("pandion"))
		res = "pandion";
	else if(name.startsWith("palringo"))
		res = "palringo";
	else if(name.startsWith("sleekxmpp.com"))
		res = "poezio";
	else if(name.startsWith("psi+") || name.startsWith("psi-dev"))
		res = "psiplus";
	else if(name.startsWith("psi"))
		res = "psi";
	else if(name.startsWith("pidgin") || name.startsWith(QString::fromUtf8(QByteArray::fromHex("d0bfd0b8d0b4d0b6d0b8d0bd"))))
		res = "pidgin";
	else if(name.startsWith("pyicqt.googlecode.com") || name.startsWith("icq transport"))
		res = "pyicq-t";
	else if(name.startsWith("qip.ru"))
		res = "qip";
	else if(name.startsWith("pda.qip.ru") || name.startsWith("qip pda"))
		res = "qippda";
	else if(name.startsWith("qip mobile") || (name.startsWith("qip.ru") && name.contains("QIP")))
		res = "qipmobile";
	else if(name.startsWith("qip infium") || name.startsWith("qip 2010") || name.startsWith("qip 2012") || name.startsWith("2010.qip.ru"))
		res = "qipinfium";
	else if(name.startsWith("qutim"))
		res = "qutim";
	else if(name.startsWith("apps.radio-t.com"))
		res = "radio-t";
	else if(name.startsWith("code.matthewwild.co.uk") && name.contains("riddim"))
		res = "riddim";
	else if(name.startsWith("xmpp4moz") || name.startsWith("hyperstruct.net"))
		res = "sameplace";
	else if(name.startsWith("sapo messenger mac") || name.startsWith("messenger.sapo.pt"))
		res = "sapo";
	else if(name.startsWith("sawim.ru"))
		res = "sawim";
	else if(name.startsWith("simpleapps.ru") && name.contains("security-bot"))
		res = "security-bot";
	else if(name.startsWith("siemens native jabber client"))
		res = "siejc";
	else if(name.startsWith("sim"))
		res = "sim";
	else if(name.startsWith("sip-communicator") || name.startsWith("sip communicator") || name.startsWith("jitsi"))
		res = "sip-communicator";
	else if((name.startsWith("igniterealtime.org") && name.contains("smack")) || name.startsWith("smack"))
		res = "smack-api";
	else if(name.startsWith("snapi-bot.googlecode.com") || (name.startsWith("github.com") && name.contains("")))
		res = "snapi-snup-bot";
	else if(name.startsWith("sonicrevolution"))
		res = "sonic-revolution";
	else if(name.startsWith("spark im client"))
		res = "spark";
	else if(name.startsWith("spectrum") || name.startsWith("binarytransport"))
		res = "spectrum";
	else if(name.startsWith("swift"))
		res = "swift";
	else if((name.startsWith("jabber-net.ru") && name.contains("talisman-bot")) || name.startsWith("j-tmb.ru"))
		res = "talisman-bot";
	else if(name.startsWith("talkonaut") || name.startsWith("google.com 1.0.0.84"))
		res = "talkonaut";
	else if(name.startsWith("talkgadget.google.com"))
		res = "talkgadget.google.com";
	else if(name.startsWith("talk.google.com") || name.startsWith("google.com 1.0.0.104"))
		res = "talk.google.com";
	else if(name.startsWith("google.com"))
		res = "google.com";
	else if(name.startsWith("tkabber"))
		res = "tkabber";
	else if(name.startsWith("telepathy"))
		res = "telepathy.freedesktop.org";
	else if(name.startsWith("tigase.org"))
		res = "tigase";
	else if(name.startsWith("trillian"))
		res = "trillian";
	else if(name.startsWith("ultimate-bot.googlecode.com"))
		res = "ultimate-bot";
	else if(name.startsWith("jabbrik.ru") || name.startsWith("jabrvista.net.ru"))
		res = "utah-bot";
	else if(name.startsWith("palringo.com"))
		res = "utalk";
	else if(name.startsWith("jabber weather.com transport"))
		res = "weather.com";
	else if(name.startsWith("chat.jabbercity.ru") || name.startsWith("web-am31.dyndns-ip.com"))
		res = "webclient";
	else if(name.startsWith("weonlydo"))
		res = "weonlydo";
	else if(name.startsWith("weonlydo.com") && name.contains("XMPP"))
		res = "wod-xmpp";
	else if(name.startsWith("wtw"))
		res = "wtw";
	else if(name.contains("vacuum"))
		res = "vacuum";
	else if(name.startsWith("vk.com") || name.startsWith("pyvk-t") || name.contains("vkontakte"))
		res = "vkontakte";
	else if(name.startsWith(QString::fromUtf8(QByteArray::fromHex("d18f2ed0bed0bdd0bbd0b0d0b9d0bd"))) || name.startsWith("online.yandex.ru"))
		res = "yaonline";
	else if(name.startsWith("ya.online"))
		res = "yaonlinej2me";
	else if(name.startsWith("yandexmail"))
		res = "yaonlinesymbian";
	else if(name.startsWith("yabber instant messenger"))
		res = "yabber";
	else if(name.startsWith("yaonline"))
		res = "yaonlinesymbian";
	else if(name.startsWith("xabber"))
		res = "xabber";
	else if(name.startsWith("xu-6.jabbrik.ru"))
		res = "xu6-bot";
	else if(name.startsWith("botx.ir"))
		res = "zeus-bot";
	else res = "unknown";

return res;
}

void UserListItem::setActivity(const Activity& activity)
{
	v_activity = activity;
}

const Activity& UserListItem::activity() const
{
	return v_activity;
}

void UserListItem::setTune(const QString& t)
{
	v_tune = t;
}

const QString& UserListItem::tune() const
{
	return v_tune;
}

void UserListItem::setGeoLocation(const GeoLocation& geoLocation)
{
	v_geoLocation = geoLocation;
}

const GeoLocation& UserListItem::geoLocation() const
{
	return v_geoLocation;
}

/*void UserListItem::setPhysicalLocation(const PhysicalLocation& physicalLocation)
{
	v_physicalLocation = physicalLocation;
}

const PhysicalLocation& UserListItem::physicalLocation() const
{
	return v_physicalLocation;
}*/

void UserListItem::setAvatarFactory(AvatarFactory* av)
{
	v_avatarFactory = av;
}

void UserListItem::setJid(const Jid &j)
{
	LiveRosterItem::setJid(j);

	int n = jid().full().indexOf('@');
	if(n == -1)
		v_isTransport = true;
	else
		v_isTransport = false;
}

bool UserListItem::isTransport() const
{
	return v_isTransport;
}

bool UserListItem::isConference() const
{
	return v_isConference;
}

void UserListItem::setConference(bool b)
{
	v_isConference = b;
}

void UserListItem::setPending(int p, int h)
{
	v_pending = p;
	v_hPending = h;
}

QString UserListItem::pending() const
{
	QString str;
	if (v_hPending)
		str = QString("[%1/%2]").arg(v_pending).arg(v_hPending);
	else if (v_pending)
		str = QString("[%1]").arg(v_pending);
	return str;
}

bool UserListItem::isAvailable() const
{
	return !v_url.isEmpty();
}

bool UserListItem::isHidden() const
{
	return groups().contains(qApp->translate("PsiContact", "Hidden"));
}

bool UserListItem::isAway() const
{
	int status;
	if(!isAvailable())
		status = STATUS_OFFLINE;
	else
		status = makeSTATUS((*userResourceList().priority()).status());

	if(status == STATUS_AWAY || status == STATUS_XA || status == STATUS_DND)
		return true;
	else
		return false;
}

QDateTime UserListItem::lastAvailable() const
{
	return v_t;
}

int UserListItem::lastMessageType() const
{
	return lastmsgtype;
}

void UserListItem::setLastMessageType(const int mtype)
{
//	printf("setting message type to %i\n", mtype);
	lastmsgtype = mtype;
}

const QString & UserListItem::presenceError() const
{
	return v_perr;
}

bool UserListItem::isSelf() const
{
	return v_self;
}

void UserListItem::setInList(bool b)
{
	v_inList = b;
}

void UserListItem::setLastAvailable(const QDateTime &t)
{
	v_t = t;
}

void UserListItem::setPresenceError(const QString &e)
{
	v_perr = e;
}

UserResourceList & UserListItem::userResourceList()
{
	return v_url;
}

UserResourceList::Iterator UserListItem::priority()
{
	return v_url.priority();
}

const UserResourceList & UserListItem::userResourceList() const
{
	return v_url;
}

UserResourceList::ConstIterator UserListItem::priority() const
{
	return v_url.priority();
}

QString UserListItem::makeTip(bool trim, bool doLinkify) const
{
	return "<qt>" + makeBareTip(trim,doLinkify) + "</qt>";
}

QString UserListItem::makeBareTip(bool trim, bool doLinkify) const
{
	// NOTE: If you add something to the tooltip,
	// you most probably want to wrap it with TextUtil::escape()

	QString str;
	int s = PsiIconset::instance()->system().iconSize();
	str +=QString("<style type='text/css'> \
		.layer1 { white-space:pre; margin-left:%1px;} \
		.layer2 { white-space:normal; margin-left:%1px;} \
	</style>").arg(s+2);

	QString imgTag = "icon name"; // or 'img src' if appropriate QMimeSourceFactory is installed. but mblsha noticed that QMimeSourceFactory unloads sometimes
	bool useAvatar = false;
	bool mucItem = false;

	if(!userResourceList().isEmpty()) {
		mucItem = userResourceList()[0].status().hasMUCItem();
	}

	if (v_avatarFactory
		&& (isPrivate() ? !v_avatarFactory->getMucAvatar(jid().full()).isNull() : !v_avatarFactory->getAvatar(jid().bare()).isNull())
		&& PsiOptions::instance()->getOption("options.ui.contactlist.tooltip.avatar").toBool())
	{
		useAvatar = true;
	}

	str += "<table cellspacing=\"3\"><tr>";
	str += "<td>";

	if (useAvatar) {
		str += QString("<icon name=\"avatars/%1\">").arg(isPrivate() ? TextUtil::escape(jid().full()) : jid().bare());
		str += "</td><td width=\"10\"></td>";
		str += "<td>";
	}

	QString nick = JIDUtil::nickOrJid(name(), jid().full());
	if (!mucItem) {
		if(jid().full() != nick)
			str += QString("<div style='white-space:pre'>%1 &lt;%2&gt;</div>").arg(TextUtil::escape(nick)).arg(TextUtil::escape(JIDUtil::toString(jid(),true)));
		else
			str += QString("<div style='white-space:pre'>%1</div>").arg(TextUtil::escape(nick));
	}

	// subscription
	if(!v_self && !v_isConference && subscription().type() != Subscription::Both && !mucItem)
		str += QString("<div style='white-space:pre'>") + QObject::tr("Subscription") + ": " + subscription().toString() + "</div>";

	if(!v_keyID.isEmpty() && PsiOptions::instance()->getOption("options.ui.contactlist.tooltip.pgp").toBool())
		str += QString("<div style='white-space:pre'><%1=\"%2\"> ").arg(imgTag).arg("psi/pgp") + QObject::tr("OpenPGP") + ": " + v_keyID.right(8) + "</div>";

	// User Mood
	if (!mood().isNull()) {
		str += QString("<div style='white-space:pre'><%1=\"mood/%2\"> ").arg(imgTag).arg(mood().typeValue()) + QObject::tr("Mood") + ": " + mood().typeText();
		if (!mood().text().isEmpty())
			str += QString(" (") + TextUtil::escape(mood().text()) + QString(")");
		str += "</div>";
	}

	// User Activity
	if (!activity().isNull()) {
		QString act = activity().typeValue();
		if (activity().specificType() != Activity::UnknownSpecific && activity().specificType() != Activity::Other && !activity().specificTypeValue().isEmpty()) {
			act += "_" + activity().specificTypeValue();
		}
		str += QString("<div style='white-space:pre'><%1=\"activities/%2\"> ").arg(imgTag).arg(act) + QObject::tr("Activity") + ": " + activity().typeText();
		if (activity().specificType() != Activity::UnknownSpecific) {
			str += QString(" - ") + activity().specificTypeText();
		}
		if (!activity().text().isEmpty())
			str += QString(" (") + TextUtil::escape(activity().text()) + QString(")");
		str += "</div>";
	}

	// User Tune
	if (!tune().isEmpty())
		str += QString("<div style='white-space:pre'><%1=\"%2\"> ").arg(imgTag).arg("psi/notification_roster_tune") + QObject::tr("Listening to") + ": " + TextUtil::escape(tune()) + "</div>";

	// User Physical Location
	//if (!physicalLocation().isNull())
	//	str += QString("<div style='white-space:pre'>") + QObject::tr("Location") + ": " + TextUtil::escape(physicalLocation().toString()) + "</div>";

	// User Geolocation
	if (!geoLocation().isNull() && PsiOptions::instance()->getOption("options.ui.contactlist.tooltip.geolocation").toBool())
		str += QString("<div style='white-space:pre'><table cellspacing=\"0\"><tr><td><%1=\"%2\"> </td><td><div>%3</div></td></tr></table></div>") \
		.arg(imgTag).arg("system/geolocation").arg(TextUtil::escape(geoLocation().toString().trimmed()));

	// resources
	if(!userResourceList().isEmpty()) {
		UserResourceList srl = userResourceList();
		srl.sort();

		for(UserResourceList::ConstIterator rit = srl.begin(); rit != srl.end(); ++rit) {
			const UserResource &r = *rit;

			QString name;
			if(!r.name().isEmpty())
				name = r.name();
			else
				name = QObject::tr("[blank]");

			QString secstr;
			if(isSecure(r.name()) && PsiOptions::instance()->getOption("options.ui.contactlist.tooltip.pgp").toBool())
				secstr += QString(" <%1=\"psi/cryptoYes\">").arg(imgTag);
			QString hr;
			if (!mucItem)
				hr = "<hr/>";
			str += hr + "<div style='white-space:pre'>";

			PsiIcon *statusIcon = PsiIconset::instance()->statusPtr(jid(), makeSTATUS(r.status()));
			if (statusIcon) {
				QByteArray imageArray;
				QBuffer buff(&imageArray);
				statusIcon->image().save(&buff, "png");
				QString imgBase64(QUrl::toPercentEncoding(imageArray.toBase64()));
				str += QString("<img src=\"data:image/png;base64,%1\" alt=\"img\"/>").arg(imgBase64);
			}

			str += QString(" <b>%1</b> ").arg(TextUtil::escape(name)) + QString("(%1)").arg(r.priority());
			if (!r.status().mucItem().jid().isEmpty())
				str += QString(" &lt;%1&gt;").arg(TextUtil::escape(JIDUtil::toString(r.status().mucItem().jid(),true)));
			str += secstr + "</div>";

			if(!r.publicKeyID().isEmpty() && PsiOptions::instance()->getOption("options.ui.contactlist.tooltip.pgp").toBool()) {
				int v = r.pgpVerifyStatus();
				if(v == QCA::SecureMessageSignature::Valid || v == QCA::SecureMessageSignature::InvalidSignature || v == QCA::SecureMessageSignature::InvalidKey || v == QCA::SecureMessageSignature::NoKey) {
					if(v == QCA::SecureMessageSignature::Valid) {
						QString d = r.sigTimestamp().toString(Qt::DefaultLocaleShortDate);
						str += QString("<div class='layer1'><%1=\"%2\"> ").arg(imgTag).arg("psi/gpg-yes") + QObject::tr("Signed") + ": " + "<font color=\"#2A993B\">" + d + "</font>";
					}
					else if(v == QCA::SecureMessageSignature::NoKey) {
						QString d = r.sigTimestamp().toString(Qt::DefaultLocaleShortDate);
						str += QString("<div class='layer1'><%1=\"%2\"> ").arg(imgTag).arg("psi/keyUnknown") + QObject::tr("Signed") + ": " + d;
					}
					else if(v == QCA::SecureMessageSignature::InvalidSignature || v == QCA::SecureMessageSignature::InvalidKey) {
						str += QString("<div class='layer1'><%1=\"%2\"> ").arg(imgTag).arg("psi/keyBad") + "<font color=\"#810000\">" + QObject::tr("Bad signature") + "</font>";
					}

					if(v_keyID != r.publicKeyID())
						str += QString(" [%1]").arg(r.publicKeyID().right(8));
					str += "</div>";
				}
			}

			// client
			if(!r.versionString().isEmpty() && PsiOptions::instance()->getOption("options.ui.contactlist.tooltip.client-version").toBool()) {
				QString ver = r.versionString();
				if(trim)
					ver = dot_truncate(ver, 80);
				ver = TextUtil::escape(ver);
				str += QString("<div class='layer1'><%1=\"%2\"> ").arg(imgTag).arg("clients/" + findClient(r.clientName().toLower())) + QObject::tr("Using") + QString(": %3").arg(ver) + "</div>";
			}


			// Entity Time
			if (r.timezoneOffset().hasValue()) {
				QDateTime dt = QDateTime::currentDateTime().toUTC().addSecs(r.timezoneOffset().value()*60);
				str += QString("<div class='layer1'><%1=\"%2\"> ").arg(imgTag).arg("psi/time") + QObject::tr("Time") + QString(": %1 (%2)").arg(dt.toString(Qt::DefaultLocaleShortDate)).arg(r.timezoneOffsetString()) + "</div>";
			}

			// MUC
			if(!v_isConference && r.status().hasMUCItem()) {
				MUCItem::Affiliation a = r.status().mucItem().affiliation();
				QString aff;
				if(a == MUCItem::Owner)
					aff = "affiliation/owner";
				else if(a == MUCItem::Admin)
					aff = "affiliation/admin";
				else if(a == MUCItem::Member)
					aff = "affiliation/member";
				else if(a == MUCItem::Outcast)
					aff = "affiliation/outcast";
				else
					aff = "affiliation/noaffiliation";
				//if(!r.status().mucItem().jid().isEmpty())
				//	str += QString("<div class='layer1'>") + QObject::tr("JID: %1").arg(JIDUtil::toString(r.status().mucItem().jid(),true)) + QString("</div>");
				if(r.status().mucItem().role() != MUCItem::NoRole)
					str += QString("<div class='layer2'><table cellspacing=\"0\"><tr><td><%1=\"%2\"> </td><td>").arg(imgTag).arg(aff);
					str += QString("<div style='white-space:pre'>") + QObject::tr("Role: %1").arg(MUCManager::roleToString(r.status().mucItem().role())) + QString("</div>");
					str += QString("<div style='white-space:pre'>") + QObject::tr("Affiliation: %1").arg(MUCManager::affiliationToString(r.status().mucItem().affiliation())) + QString("</td></tr></table></div>");
			}

			// last status
			if(r.status().timeStamp().isValid() && PsiOptions::instance()->getOption("options.ui.contactlist.tooltip.last-status").toBool()) {
				QString d = r.status().timeStamp().toString(Qt::DefaultLocaleShortDate);
				str += QString("<div class='layer1'><%1=\"%2\"> ").arg(imgTag).arg("psi/info") + QObject::tr("Last Status") + ": " + d + "</div>";
			}

			// status message
			QString s = r.status().status();
			if(!s.isEmpty()) {
				QString head = QObject::tr("Status Message");
				if(trim)
					s = TextUtil::plain2rich(clipStatus(s, 200, 12));
				else
					s = TextUtil::plain2rich(s);
				if ( doLinkify )
					s = TextUtil::linkify(s);
				if( PsiOptions::instance()->getOption("options.ui.emoticons.use-emoticons").toBool() && !doLinkify )
					s = TextUtil::emoticonify(s);
				if( !doLinkify && PsiOptions::instance()->getOption("options.ui.chat.legacy-formatting").toBool() )
					s = TextUtil::legacyFormat(s);

				str += QString("<div class='layer2'><table cellspacing=\"0\"><tr><td><%1=\"%2\"> </td><td><div><u>%3</u>: %4</div></td></tr></table></div>") \
				.arg(imgTag).arg("psi/action_templates_edit").arg(head).arg(s);
			}
		}
	}
	else {
		// last available
		if(!lastAvailable().isNull()) {
			QString d = lastAvailable().toString(Qt::DefaultLocaleShortDate);
			str += QString("<div style='white-space:pre'><%1=\"%2\"> ").arg(imgTag).arg("psi/info") + QObject::tr("Last Available") + ": " + d + "</div>";
		}

		// presence error
		if(!v_perr.isEmpty()) {
			QStringList err = v_perr.split('\n');
			str += QString("<div style='white-space:pre'>") + QObject::tr("Presence Error") + QString(": %1").arg(TextUtil::escape(err[0])) + "</div>";
			err.pop_front();
			foreach (QString line, err)
				str += "<div>" + TextUtil::escape(line) + "</div>";
		}

		// status message
		QString s = lastUnavailableStatus().status();
		if(!s.isEmpty()) {
			QString head = QObject::tr("Last Status Message");
			if(trim)
				s = TextUtil::plain2rich(clipStatus(s, 200, 12));
			else {
				s = TextUtil::plain2rich(clipStatus(s, 200, 12));
				if ( doLinkify )
					s = TextUtil::linkify(s);
			}
			str += QString("<div class='layer2'><table cellspacing=\"0\"><tr><td><%1=\"%2\"> </td><td><div><u>%3</u>: %4</div></td></tr></table></div>") \
			.arg(imgTag).arg("psi/action_templates_edit").arg(head).arg(s);
		}
	}

	str += "</td>";
	str += "</tr></table>";

	return str;
}

QString UserListItem::makeDesc() const
{
	return makeTip(false);
}

bool UserListItem::isPrivate() const
{
	return v_private;
}

void UserListItem::setPrivate(bool b)
{
	v_private = b;
}

bool UserListItem::isSecure() const
{
	return !secList.isEmpty();
}

bool UserListItem::isSecure(const QString &rname) const
{
	for(QStringList::ConstIterator it = secList.begin(); it != secList.end(); ++it) {
		if(*it == rname)
			return true;
	}
	return false;
}

void UserListItem::setSecure(const QString &rname, bool b)
{
	foreach(const QString s, secList) {
		if(s == rname) {
			if(!b)
				secList.removeAll(s);
			return;
		}
	}
	if(b)
		secList.append(rname);
}

const QString & UserListItem::publicKeyID() const
{
	return v_keyID;
}

void UserListItem::setPublicKeyID(const QString &k)
{
	v_keyID = k;
}


//----------------------------------------------------------------------------
// UserList
//----------------------------------------------------------------------------
UserList::UserList()
{
}

UserList::~UserList()
{
}

UserListItem *UserList::find(const XMPP::Jid &j)
{
	foreach(UserListItem* i, *this) {
		if(i->jid().compare(j))
			return i;
	}
	return 0;
}

void UserList::setGroupsDelimiter(const QString &groupsDelimiter)
{
	_groupsDelimiter = groupsDelimiter;
}

QString UserList::groupsDelimiter() const
{
	return _groupsDelimiter;
}

bool UserList::hasGroupsDelimiter() const
{
	return groupsDelimiter().indexOf(QRegExp("^[0-9A-Za-z]?$")) == -1;
}
