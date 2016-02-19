			/*
 * protocol.cpp - XMPP-Core protocol state machine
 * Copyright (C) 2004  Justin Karneges
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
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

// TODO: let the app know if tls is required
//       require mutual auth for server out/in
//       report ErrProtocol if server uses wrong NS
//       use send() instead of writeElement() in CoreProtocol

#include "protocol.h"

#include <qca.h>
#include <QList>
#include <QByteArray>
#include <QtCrypto>

#ifdef XMPP_TEST
#include "td.h"
#endif

//#define IRIS_SM_DEBUG

using namespace XMPP;

// printArray
//
// This function prints out an array of bytes as latin characters, converting
// non-printable bytes into hex values as necessary.  Useful for displaying
// QByteArrays for debugging purposes.
static QString printArray(const QByteArray &a)
{
	QString s;
	for(int n = 0; n < a.size(); ++n) {
		unsigned char c = (unsigned char)a[(int)n];
		if(c < 32 || c >= 127) {
			QString str;
			str.sprintf("[%02x]", c);
			s += str;
		}
		else
			s += c;
	}
	return s;
}

// firstChildElement
//
// Get an element's first child element
static QDomElement firstChildElement(const QDomElement &e)
{
	for(QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
		if(n.isElement())
			return n.toElement();
	}
	return QDomElement();
}

//----------------------------------------------------------------------------
// Version
//----------------------------------------------------------------------------
Version::Version(int maj, int min)
{
	major = maj;
	minor = min;
}

//----------------------------------------------------------------------------
// StreamFeatures
//----------------------------------------------------------------------------
StreamFeatures::StreamFeatures()
{
	tls_supported = false;
	sasl_supported = false;
	bind_supported = false;
	tls_required = false;
	compress_supported = false;
}

//----------------------------------------------------------------------------
// BasicProtocol
//----------------------------------------------------------------------------
BasicProtocol::SASLCondEntry BasicProtocol::saslCondTable[] =
{
	{ "aborted",                Aborted },
	{ "incorrect-encoding",     IncorrectEncoding },
	{ "invalid-authzid",        InvalidAuthzid },
	{ "invalid-mechanism",      InvalidMech },
	{ "mechanism-too-weak",     MechTooWeak },
	{ "not-authorized",         NotAuthorized },
	{ "temporary-auth-failure", TemporaryAuthFailure },
	{ 0, 0 },
};

BasicProtocol::StreamCondEntry BasicProtocol::streamCondTable[] =
{
	{ "bad-format",               BadFormat },
	{ "bad-namespace-prefix",     BadNamespacePrefix },
	{ "conflict",                 Conflict },
	{ "connection-timeout",       ConnectionTimeout },
	{ "host-gone",                HostGone },
	{ "host-unknown",             HostUnknown },
	{ "improper-addressing",      ImproperAddressing },
	{ "internal-server-error",    InternalServerError },
	{ "invalid-from",             InvalidFrom },
	{ "invalid-namespace",        InvalidNamespace },
	{ "invalid-xml",              InvalidXml },
	{ "not-authorized",           StreamNotAuthorized },
	{ "policy-violation",         PolicyViolation },
	{ "remote-connection-failed", RemoteConnectionFailed },
	{ "resource-constraint",      ResourceConstraint },
	{ "restricted-xml",           RestrictedXml },
	{ "see-other-host",           SeeOtherHost },
	{ "system-shutdown",          SystemShutdown },
	{ "undefined-condition",      UndefinedCondition },
	{ "unsupported-encoding",     UnsupportedEncoding },
	{ "unsupported-stanza-type",  UnsupportedStanzaType },
	{ "unsupported-version",      UnsupportedVersion },
	{ "not-well-formed",          NotWellFormed },
	{ 0, 0 },
};

BasicProtocol::BasicProtocol()
:XmlProtocol()
{
	init();
}

BasicProtocol::~BasicProtocol()
{
}

void BasicProtocol::init()
{
	errCond = -1;
	sasl_authed = false;
	doShutdown = false;
	delayedError = false;
	closeError = false;
	ready = false;
	stanzasPending = 0;
	stanzasWritten = 0;
}

void BasicProtocol::reset()
{
	XmlProtocol::reset();
	init();

	to = QString();
	from = QString();
	id = QString();
	lang = QString();
	version = Version(1,0);
	errText = QString();
	errAppSpec = QDomElement();
	otherHost = QString();
	spare.resize(0);
	sasl_mech = QString();
	sasl_mechlist.clear();
	sasl_step.resize(0);
	stanzaToRecv = QDomElement();
	sendList.clear();
}

void BasicProtocol::sendStanza(const QDomElement &e)
{
	SendItem i;
	i.stanzaToSend = e;
	sendList += i;
}

void BasicProtocol::sendDirect(const QString &s)
{
	SendItem i;
	i.stringToSend = s;
	sendList += i;
}

void BasicProtocol::sendWhitespace()
{
	SendItem i;
	i.doWhitespace = true;
	sendList += i;
}

QDomElement BasicProtocol::recvStanza()
{
	QDomElement e = stanzaToRecv;
	stanzaToRecv = QDomElement();
	return e;
}

void BasicProtocol::shutdown()
{
	doShutdown = true;
}

void BasicProtocol::shutdownWithError(int cond, const QString &str)
{
	otherHost = str;
	delayErrorAndClose(cond);
}

bool BasicProtocol::isReady() const
{
	return ready;
}

void BasicProtocol::setReady(bool b)
{
	ready = b;
}

QString BasicProtocol::saslMech() const
{
	return sasl_mech;
}

QByteArray BasicProtocol::saslStep() const
{
	return sasl_step;
}

void BasicProtocol::setSASLMechList(const QStringList &list)
{
	sasl_mechlist = list;
}

void BasicProtocol::setSASLFirst(const QString &mech, const QByteArray &step)
{
	sasl_mech = mech;
	sasl_step = step;
}

void BasicProtocol::setSASLNext(const QByteArray &step)
{
	sasl_step = step;
}

void BasicProtocol::setSASLAuthed()
{
	sasl_authed = true;
}

int BasicProtocol::stringToSASLCond(const QString &s)
{
	for(int n = 0; saslCondTable[n].str; ++n) {
		if(s == saslCondTable[n].str)
			return saslCondTable[n].cond;
	}
	return -1;
}

int BasicProtocol::stringToStreamCond(const QString &s)
{
	for(int n = 0; streamCondTable[n].str; ++n) {
		if(s == streamCondTable[n].str)
			return streamCondTable[n].cond;
	}
	return -1;
}

QString BasicProtocol::saslCondToString(int x)
{
	for(int n = 0; saslCondTable[n].str; ++n) {
		if(x == saslCondTable[n].cond)
			return saslCondTable[n].str;
	}
	return QString();
}

QString BasicProtocol::streamCondToString(int x)
{
	for(int n = 0; streamCondTable[n].str; ++n) {
		if(x == streamCondTable[n].cond)
			return streamCondTable[n].str;
	}
	return QString();
}

void BasicProtocol::extractStreamError(const QDomElement &e)
{
	QString text;
	QDomElement appSpec;

	QDomElement t = firstChildElement(e);
	if(t.isNull() || t.namespaceURI() != NS_STREAMS) {
		// probably old-style error
		errCond = -1;
		errText = e.text();
	}
	else
		errCond = stringToStreamCond(t.tagName());

	if(errCond != -1) {
		if(errCond == SeeOtherHost)
			otherHost = t.text();

		t = e.elementsByTagNameNS(NS_STREAMS, "text").item(0).toElement();
		if(!t.isNull())
			text = t.text();

		// find first non-standard namespaced element
		QDomNodeList nl = e.childNodes();
		for(int n = 0; n < nl.count(); ++n) {
			QDomNode i = nl.item(n);
			if(i.isElement() && i.namespaceURI() != NS_STREAMS) {
				appSpec = i.toElement();
				break;
			}
		}

		errText = text;
		errAppSpec = appSpec;
	}
}

void BasicProtocol::send(const QDomElement &e, bool clip)
{
	writeElement(e, TypeElement, false, clip);
}

void BasicProtocol::sendStreamError(int cond, const QString &text, const QDomElement &appSpec)
{
	QDomElement se = doc.createElementNS(NS_ETHERX, "stream:error");
	QDomElement err = doc.createElementNS(NS_STREAMS, streamCondToString(cond));
	if(!otherHost.isEmpty())
		err.appendChild(doc.createTextNode(otherHost));
	se.appendChild(err);
	if(!text.isEmpty()) {
		QDomElement te = doc.createElementNS(NS_STREAMS, "text");
		te.setAttributeNS(NS_XML, "xml:lang", "en");
		te.appendChild(doc.createTextNode(text));
		se.appendChild(te);
	}
	se.appendChild(appSpec);

	writeElement(se, 100, false);
}

void BasicProtocol::sendStreamError(const QString &text)
{
	QDomElement se = doc.createElementNS(NS_ETHERX, "stream:error");
	se.appendChild(doc.createTextNode(text));

	writeElement(se, 100, false);
}

bool BasicProtocol::errorAndClose(int cond, const QString &text, const QDomElement &appSpec)
{
	closeError = true;
	errCond = cond;
	errText = text;
	errAppSpec = appSpec;
	sendStreamError(cond, text, appSpec);
	return close();
}

bool BasicProtocol::error(int code)
{
	event = EError;
	errorCode = code;
	return true;
}

void BasicProtocol::delayErrorAndClose(int cond, const QString &text, const QDomElement &appSpec)
{
	errorCode = ErrStream;
	errCond = cond;
	errText = text;
	errAppSpec = appSpec;
	delayedError = true;
}

void BasicProtocol::delayError(int code)
{
	errorCode = code;
	delayedError = true;
}

QDomElement BasicProtocol::docElement()
{
	// create the root element
	QDomElement e = doc.createElementNS(NS_ETHERX, "stream:stream");

	QString defns = defaultNamespace();
	const QStringList list = extraNamespaces();

	// HACK: using attributes seems to be the only way to get additional namespaces in here
	if(!defns.isEmpty())
		e.setAttribute("xmlns", defns);
	for(QStringList::ConstIterator it = list.begin(); it != list.end();) {
		QString prefix = *(it++);
		QString uri = *(it++);
		e.setAttribute(QString("xmlns:") + prefix, uri);
	}

	// additional attributes
	if(!isIncoming() && !to.isEmpty())
		e.setAttribute("to", to);
	if(isIncoming() && !from.isEmpty())
		e.setAttribute("from", from);
	if(!id.isEmpty())
		e.setAttribute("id", id);
	if(!lang.isEmpty())
		e.setAttributeNS(NS_XML, "xml:lang", lang);
	if(version.major > 0 || version.minor > 0)
		e.setAttribute("version", QString::number(version.major) + '.' + QString::number(version.minor));

	return e;
}

void BasicProtocol::handleDocOpen(const Parser::Event &pe)
{
	if(isIncoming()) {
		if(xmlEncoding() != "UTF-8") {
			delayErrorAndClose(UnsupportedEncoding);
			return;
		}
	}

	if(pe.namespaceURI() == NS_ETHERX && pe.localName() == "stream") {
		QXmlAttributes atts = pe.atts();

		// grab the version
		int major = 0;
		int minor = 0;
		QString verstr = atts.value("version");
		if(!verstr.isEmpty()) {
			int n = verstr.indexOf('.');
			if(n != -1) {
				major = verstr.mid(0, n).toInt();
				minor = verstr.mid(n+1).toInt();
			}
			else {
				major = verstr.toInt();
				minor = 0;
			}
		}
		version = Version(major, minor);

		if(isIncoming()) {
			to = atts.value("to");
			QString peerLang = atts.value(NS_XML, "lang");
			if(!peerLang.isEmpty())
				lang = peerLang;
		}
		// outgoing
		else {
			from = atts.value("from");
			lang = atts.value(NS_XML, "lang");
			id = atts.value("id");
		}

		handleStreamOpen(pe);
	}
	else {
		if(isIncoming())
			delayErrorAndClose(BadFormat);
		else
			delayError(ErrProtocol);
	}
}

bool BasicProtocol::handleError()
{
	if(isIncoming())
		return errorAndClose(NotWellFormed);
	else
		return error(ErrParse);
}

bool BasicProtocol::handleCloseFinished()
{
	if(closeError) {
		event = EError;
		errorCode = ErrStream;
		// note: errCond and friends are already set at this point
	}
	else
		event = EClosed;
	return true;
}

bool BasicProtocol::doStep(const QDomElement &e)
{
	// handle pending error
	if(delayedError) {
		if(isIncoming())
			return errorAndClose(errCond, errText, errAppSpec);
		else
			return error(errorCode);
	}

	// shutdown?
	if(doShutdown) {
		doShutdown = false;
		return close();
	}

	if(!e.isNull()) {
		// check for error
		if(e.namespaceURI() == NS_ETHERX && e.tagName() == "error") {
			extractStreamError(e);
			return error(ErrStream);
		}
	}

	if(ready) {
		// stanzas written?
		if(stanzasWritten > 0) {
			--stanzasWritten;
			event = EStanzaSent;
			return true;
		}
		// send items?
		if(!sendList.isEmpty()) {
			SendItem i;
			{
				QList<SendItem>::Iterator it = sendList.begin();
				i = (*it);
				sendList.erase(it);
			}

			// outgoing stanza?
			if(!i.stanzaToSend.isNull()) {
				++stanzasPending;
				writeElement(i.stanzaToSend, TypeStanza, true);
				event = ESend;
			}
			// direct send?
			else if(!i.stringToSend.isEmpty()) {
				writeString(i.stringToSend, TypeDirect, true);
				event = ESend;
			}
			// whitespace keepalive?
			else if(i.doWhitespace) {
				writeString("\n", TypePing, false);
				event = ESend;
			}
			return true;
		}
		else {
			// if we have pending outgoing stanzas, ask for write notification
			if(stanzasPending)
				notify |= NSend;
		}
	}

	return doStep2(e);
}

void BasicProtocol::itemWritten(int id, int)
{
	if(id == TypeStanza) {
		--stanzasPending;
		++stanzasWritten;
	}
}

QString BasicProtocol::defaultNamespace()
{
	// default none
	return QString();
}

QStringList BasicProtocol::extraNamespaces()
{
	// default none
	return QStringList();
}

void BasicProtocol::handleStreamOpen(const Parser::Event &)
{
	// default does nothing
}

//----------------------------------------------------------------------------
// CoreProtocol
//----------------------------------------------------------------------------
CoreProtocol::CoreProtocol()
:BasicProtocol()
{
	init();
	sm_resumption_supported = false;
	sm_resumption_id = "";
	sm_receive_count = 0;
	sm_server_last_handled = 0;
}

CoreProtocol::~CoreProtocol()
{
	//fprintf(stderr, "\tCoreProtocol::~CoreProtocol()\n");
}

void CoreProtocol::init()
{
	step = Start;

	// ??
	server = false;
	dialback = false;
	dialback_verify = false;

	// settings
	jid_ = Jid();
	password = QString();
	oldOnly = false;
	allowPlain = false;
	doTLS = true;
	doAuth = true;
	doCompress = true;
	doBinding = true;

	// input
	user = QString();
	host = QString();

	// status
	old = false;
	digest = false;
	tls_started = false;
	sasl_started = false;
	compress_started = false;

	sm_started = false;
}

void CoreProtocol::reset()
{
	BasicProtocol::reset();
	init();
}

void CoreProtocol::startTimer(int seconds) {
	sm_ack_last_requested.start();
	notify |= NTimeout;
	need = NNotify;
	timeout_sec = seconds;
}

void CoreProtocol::sendStanza(const QDomElement &e, bool notify) {
	if (isStreamManagementActive()) {
#ifdef XMPP_TEST
		if (notify) qDebug() << "Want notification for stanza";
#endif
		sm_send_queue.push_back(qMakePair(e, notify));
#ifdef IRIS_SM_DEBUG
		qDebug() << "sm_send_queue: ";
#endif
		for (QList<QPair<QDomElement, bool> >::iterator i = sm_send_queue.begin(); i != sm_send_queue.end(); ++i) {
			QPair<QDomElement, bool> entry = *i;
#ifdef IRIS_SM_DEBUG
			qDebug() << "\t" << entry.first.tagName() << " : " << entry.second;
#endif
		}
		if (sm_send_queue.length() > 5 && sm_send_queue.length() % 4 == 0) requestSMAcknowlegement();
		startTimer(20);
	}
	//qDebug() << "CoreProtocol::sendStanza";
	BasicProtocol::sendStanza(e);
}

void CoreProtocol::startClientOut(const Jid &_jid, bool _oldOnly, bool tlsActive, bool _doAuth, bool _doCompress)
{
	jid_ = _jid;
	to = _jid.domain();
	oldOnly = _oldOnly;
	doAuth = _doAuth;
	doCompress = _doCompress;
	tls_started = tlsActive;

	if(oldOnly)
		version = Version(0,0);
	startConnect();
}

void CoreProtocol::startServerOut(const QString &_to)
{
	server = true;
	to = _to;
	startConnect();
}

void CoreProtocol::startDialbackOut(const QString &_to, const QString &_from)
{
	server = true;
	dialback = true;
	to = _to;
	self_from = _from;
	startConnect();
}

void CoreProtocol::startDialbackVerifyOut(const QString &_to, const QString &_from, const QString &id, const QString &key)
{
	server = true;
	dialback = true;
	dialback_verify = true;
	to = _to;
	self_from = _from;
	dialback_id = id;
	dialback_key = key;
	startConnect();
}

void CoreProtocol::startClientIn(const QString &_id)
{
	id = _id;
	startAccept();
}

void CoreProtocol::startServerIn(const QString &_id)
{
	server = true;
	id = _id;
	startAccept();
}

void CoreProtocol::setLang(const QString &s)
{
	lang = s;
}

void CoreProtocol::setAllowTLS(bool b)
{
	doTLS = b;
}

void CoreProtocol::setAllowBind(bool b)
{
	doBinding = b;
}

void CoreProtocol::setAllowPlain(bool b)
{
	allowPlain = b;
}

const Jid& CoreProtocol::jid() const
{
	return jid_;
}

void CoreProtocol::setPassword(const QString &s)
{
	password = s;
}

void CoreProtocol::setFrom(const QString &s)
{
	from = s;
}

void CoreProtocol::setDialbackKey(const QString &s)
{
	dialback_key = s;
}

bool CoreProtocol::loginComplete()
{
	setReady(true);

	// deal with stream management
	if(!sm_started && features.sm_supported) {
		if (sm_resumption_supported && sm_resumption_id != "") {
			QDomElement e = doc.createElementNS(NS_STREAM_MANAGEMENT, "resume");
			e.setAttribute("previd", sm_resumption_id);
			qulonglong lasthandledid = getSMLastHandledId();
			e.setAttribute("h", lasthandledid);
			send(e);
		} else {
			QDomElement e = doc.createElementNS(NS_STREAM_MANAGEMENT, "enable");
			e.setAttribute("resume", "true");
			send(e);
		}
		event = ESend;
		step = GetSMResponse;
	} else {
		event = EReady;
		step = Done;
	}
	return true;
}

int CoreProtocol::getOldErrorCode(const QDomElement &e)
{
	QDomElement err = e.elementsByTagNameNS(NS_CLIENT, "error").item(0).toElement();
	if(err.isNull() || !err.hasAttribute("code"))
		return -1;
	return err.attribute("code").toInt();
}

/*QString CoreProtocol::xmlToString(const QDomElement &e, bool clip)
{
	// determine an appropriate 'fakeNS' to use
	QString ns;
	if(e.prefix() == "stream")
		ns = NS_ETHERX;
	else if(e.prefix() == "db")
		ns = NS_DIALBACK;
	else
		ns = NS_CLIENT;
	return ::xmlToString(e, ns, "stream:stream", clip);
}*/

bool CoreProtocol::stepAdvancesParser() const
{
	if(stepRequiresElement())
		return true;
	else if(isReady())
		return true;
	return false;
}

// all element-needing steps need to be registered here
bool CoreProtocol::stepRequiresElement() const
{
	switch(step) {
		case GetFeatures:
		case GetTLSProceed:
		case GetCompressProceed:
		case GetSASLChallenge:
		case GetBindResponse:
		case GetAuthGetResponse:
		case GetAuthSetResponse:
		case GetRequest:
		case GetSASLResponse:
		case GetSMResponse:
			return true;
	}
	return false;
}

void CoreProtocol::stringSend(const QString &s)
{
#ifdef XMPP_TEST
	TD::outgoingTag(s);
#endif
}

void CoreProtocol::stringRecv(const QString &s)
{
#ifdef XMPP_TEST
	TD::incomingTag(s);
#endif
}

QString CoreProtocol::defaultNamespace()
{
	if(server)
		return NS_SERVER;
	else
		return NS_CLIENT;
}

QStringList CoreProtocol::extraNamespaces()
{
	QStringList list;
	if(dialback) {
		list += "db";
		list += NS_DIALBACK;
	}
	return list;
}

void CoreProtocol::handleStreamOpen(const Parser::Event &pe)
{
	if(isIncoming()) {
		QString ns = pe.nsprefix();
		QString db;
		if(server) {
			db = pe.nsprefix("db");
			if(!db.isEmpty())
				dialback = true;
		}

		// verify namespace
		if((!server && ns != NS_CLIENT) || (server && ns != NS_SERVER) || (dialback && db != NS_DIALBACK)) {
			delayErrorAndClose(InvalidNamespace);
			return;
		}

		// verify version
		if(version.major < 1 && !dialback) {
			delayErrorAndClose(UnsupportedVersion);
			return;
		}
	}
	else {
		if(!dialback) {
			if(version.major >= 1 && !oldOnly)
				old = false;
			else
				old = true;
		}
	}
}

void CoreProtocol::elementSend(const QDomElement &e)
{
#ifdef XMPP_TEST
	TD::outgoingXml(e);
#endif
}

void CoreProtocol::elementRecv(const QDomElement &e)
{
#ifdef XMPP_TEST
	TD::incomingXml(e);
#endif
}

bool CoreProtocol::doStep2(const QDomElement &e)
{
	if(dialback)
		return dialbackStep(e);
	else
		return normalStep(e);
}

bool CoreProtocol::isValidStanza(const QDomElement &e) const
{
	QString s = e.tagName();
	Stanza::Kind kind = Stanza::kind(s);
	if(e.namespaceURI() == (server ? NS_SERVER : NS_CLIENT) && (kind == Stanza::Message || kind == Stanza::Presence || kind == Stanza::IQ))
		return true;
	else
		return false;
}

bool CoreProtocol::streamManagementHandleStanza(const QDomElement &e)
{
	QString s = e.tagName();
	if(s == "r") {
		qulonglong last_handled_id = getSMLastHandledId();
		QDomElement e = doc.createElementNS(NS_STREAM_MANAGEMENT, "a");
		e.setAttribute("h", last_handled_id);
#ifdef IRIS_SM_DEBUG
		qWarning() << "Stream Management: Sending acknowledgment with h=" << last_handled_id;
#endif
		send(e);
		event = ESend;
		return true;
	} else if (s == "a") {
#ifdef IRIS_SM_DEBUG
		qWarning() << "Received ack response from server";
#endif
		processSMAcknowlegement(e.attribute("h").toULong());
		event = EAck;
		return true;
	} else {
		need = NNotify;
		notify |= NRecv;
		return false;
	}
}

unsigned long CoreProtocol::getNewSMId() {
	unsigned long sm_id = sm_receive_count;
	sm_receive_queue.push_back(qMakePair(sm_id, false));
	sm_receive_count++;
	if (sm_receive_count == (unsigned long)-1) sm_receive_count = 0; /* why do we skip -1? */
#ifdef IRIS_SM_DEBUG
	qWarning() << "Current SM id: " << sm_id;
#endif
	return sm_id;
}

unsigned long CoreProtocol::getSMLastHandledId() {
	if (sm_receive_queue.isEmpty()) {
		return sm_receive_count - 1;
	} else {
		QPair<unsigned long, bool> queue_item;
		unsigned long last_handled_id = sm_receive_count - 1;;
		do {
			queue_item = sm_receive_queue.front();
			if (queue_item.second == true) {
				last_handled_id = queue_item.first;
				sm_receive_queue.pop_front();
			}
		} while (queue_item.second == true && !sm_receive_queue.isEmpty());
		return last_handled_id;
	}
}

void CoreProtocol::markStanzaHandled(unsigned long id) {
	for(QList<QPair<unsigned long, bool> >::Iterator it = sm_receive_queue.begin(); it != sm_receive_queue.end(); ++it ) {
		if (it->first == id) {
			it->second = true;
			return;
		}
	}
#ifdef IRIS_SM_DEBUG
	qWarning() << "Stream Management: Higher level client marked unknown stanza handled!";
#endif
}

void CoreProtocol::markLastMessageStanzaAcked() {
	if (sm_receive_queue.isEmpty()) {
#ifdef IRIS_SM_DEBUG
		qWarning() << "Stream Management: Higher level client marked unexistant stanza as acked.";
#endif
		return;
	}
#ifdef IRIS_SM_DEBUG
	qWarning() << "Previous list: " << sm_receive_queue;
#endif
	for(QList<QPair<unsigned long, bool> >::Iterator it = sm_receive_queue.begin(); it != sm_receive_queue.end(); ++it ) {
		if (it->second == false) {
			it->second = true;
			return;
		}
	}
}

bool CoreProtocol::isStreamManagementActive() const {
	return sm_started;
}

void CoreProtocol::requestSMAcknowlegement() {
#ifdef IRIS_SM_DEBUG
	qDebug() << "Now I'd request acknowledgement from the server.";
#endif
	sendDirect(QString("<r xmlns='" NS_STREAM_MANAGEMENT "'/>"));
	startTimer(20);
}

int CoreProtocol::getNotableStanzasAcked() {
	return sm_stanzas_notify;
}

ClientStream::SMState CoreProtocol::getSMState() const {
#ifdef IRIS_SM_DEBUG
	qDebug("\tCoreProtocol::getSMState()");
#endif
	ClientStream::SMState state;
	state.sm_receive_queue = sm_receive_queue;
	state.sm_send_queue = sm_send_queue;
	state.sm_receive_count = sm_receive_count;

	state.sm_server_last_handled = sm_server_last_handled;
	state.sm_stanzas_notify = sm_stanzas_notify;

	state.sm_resumtion_supported = sm_resumption_supported;
	state.sm_resumption_id = sm_resumption_id;
	state.sm_resumption_location = sm_resumption_location;
	return state;
}

void CoreProtocol::setSMState(ClientStream::SMState &state) {
	fprintf(stderr, "\tCoreProtocol::setSMState()\n");
	sm_receive_queue = state.sm_receive_queue;
	sm_send_queue = state.sm_send_queue;
	sm_receive_count = state.sm_receive_count;

	sm_server_last_handled = state.sm_server_last_handled;
	sm_stanzas_notify = state.sm_stanzas_notify;

	sm_resumption_supported = state.sm_resumtion_supported;
	sm_resumption_id = state.sm_resumption_id;
}

void CoreProtocol::processSMAcknowlegement(unsigned long last_handled) {
	int handled_stanzas = 0;
	int notifies = 0;
	if (sm_server_last_handled == 0) handled_stanzas = last_handled + 1;
	else handled_stanzas = last_handled - sm_server_last_handled;
	sm_server_last_handled = last_handled;

	for (int n = 0; n < handled_stanzas && !sm_send_queue.isEmpty() ; ++n) {
		QPair<QDomElement, bool> entry = sm_send_queue.first();
		sm_send_queue.pop_front();
		if (entry.second) notifies++;
	}
	sm_stanzas_notify = notifies;
}

bool CoreProtocol::grabPendingItem(const Jid &to, const Jid &from, int type, DBItem *item)
{
	for(QList<DBItem>::Iterator it = dbpending.begin(); it != dbpending.end(); ++it) {
		const DBItem &i = *it;
		if(i.type == type && i.to.compare(to) && i.from.compare(from)) {
			const DBItem &i = (*it);
			*item = i;
			dbpending.erase(it);
			return true;
		}
	}
	return false;
}

bool CoreProtocol::dialbackStep(const QDomElement &e)
{
	if(step == Start) {
		setReady(true);
		step = Done;
		event = EReady;
		return true;
	}

	if(!dbrequests.isEmpty()) {
		// process a request
		DBItem i;
		{
			QList<DBItem>::Iterator it = dbrequests.begin();
			i = (*it);
			dbrequests.erase(it);
		}

		QDomElement r;
		if(i.type == DBItem::ResultRequest) {
			r = doc.createElementNS(NS_DIALBACK, "db:result");
			r.setAttribute("to", i.to.full());
			r.setAttribute("from", i.from.full());
			r.appendChild(doc.createTextNode(i.key));
			dbpending += i;
		}
		else if(i.type == DBItem::ResultGrant) {
			r = doc.createElementNS(NS_DIALBACK, "db:result");
			r.setAttribute("to", i.to.full());
			r.setAttribute("from", i.from.full());
			r.setAttribute("type", i.ok ? "valid" : "invalid");
			if(i.ok) {
				i.type = DBItem::Validated;
				dbvalidated += i;
			}
			else {
				// TODO: disconnect after writing element
			}
		}
		else if(i.type == DBItem::VerifyRequest) {
			r = doc.createElementNS(NS_DIALBACK, "db:verify");
			r.setAttribute("to", i.to.full());
			r.setAttribute("from", i.from.full());
			r.setAttribute("id", i.id);
			r.appendChild(doc.createTextNode(i.key));
			dbpending += i;
		}
		// VerifyGrant
		else {
			r = doc.createElementNS(NS_DIALBACK, "db:verify");
			r.setAttribute("to", i.to.full());
			r.setAttribute("from", i.from.full());
			r.setAttribute("id", i.id);
			r.setAttribute("type", i.ok ? "valid" : "invalid");
		}

		writeElement(r, TypeElement, false);
		event = ESend;
		return true;
	}

	if(!e.isNull()) {
		if(e.namespaceURI() == NS_DIALBACK) {
			if(e.tagName() == "result") {
				Jid to(Jid(e.attribute("to")).domain());
				Jid from(Jid(e.attribute("from")).domain());
				if(isIncoming()) {
					QString key = e.text();
					// TODO: report event
				}
				else {
					bool ok = (e.attribute("type") == "valid") ? true: false;
					DBItem i;
					if(grabPendingItem(from, to, DBItem::ResultRequest, &i)) {
						if(ok) {
							i.type = DBItem::Validated;
							i.ok = true;
							dbvalidated += i;
							// TODO: report event
						}
						else {
							// TODO: report event
						}
					}
				}
			}
			else if(e.tagName() == "verify") {
				Jid to(Jid(e.attribute("to")).domain());
				Jid from(Jid(e.attribute("from")).domain());
				QString id = e.attribute("id");
				if(isIncoming()) {
					QString key = e.text();
					// TODO: report event
				}
				else {
					bool ok = (e.attribute("type") == "valid") ? true: false;
					DBItem i;
					if(grabPendingItem(from, to, DBItem::VerifyRequest, &i)) {
						if(ok) {
							// TODO: report event
						}
						else {
							// TODO: report event
						}
					}
				}
			}
		}
		else {
			if(isReady()) {
				if(isValidStanza(e)) {
					// TODO: disconnect if stanza is from unverified sender
					// TODO: ignore packets from receiving servers
					stanzaToRecv = e;
					event = EStanzaReady;
					return true;
				}
			}
		}
	}

	need = NNotify;
	notify |= NRecv;
	return false;
}

bool CoreProtocol::normalStep(const QDomElement &e)
{
	if(step == Start) {
		if(isIncoming()) {
			need = NSASLMechs;
			step = SendFeatures;
			return false;
		}
		else {
			if(old) {
				if(doAuth)
					step = HandleAuthGet;
				else
					return loginComplete();
			}
			else
				step = GetFeatures;

			return processStep();
		}
	}
	else if(step == HandleFeatures) {
		// deal with TLS?
		if(doTLS && !tls_started && !sasl_authed && features.tls_supported) {
			QDomElement e = doc.createElementNS(NS_TLS, "starttls");

			send(e, true);
			event = ESend;
			step = GetTLSProceed;
			return true;
		}

		// Should we go further ?
		if (!doAuth)
			return loginComplete();

		// Deal with compression
		if (doCompress && !compress_started && features.compress_supported && features.compression_mechs.contains("zlib")) {
			QDomElement e = doc.createElementNS(NS_COMPRESS_PROTOCOL, "compress");
			QDomElement m = doc.createElementNS(NS_COMPRESS_PROTOCOL, "method");
			m.appendChild(doc.createTextNode("zlib"));
			e.appendChild(m);
			send(e,true);
			event = ESend;
			step = GetCompressProceed;
			return true;
		}

		// deal with SASL?
		if(!sasl_authed) {
			if(!features.sasl_supported) {
				// SASL MUST be supported
				//event = EError;
				//errorCode = ErrProtocol;
				//return true;

				// Fall back on auth for non-compliant servers
				step = HandleAuthGet;
				old = true;
				return true;
			}

#ifdef XMPP_TEST
			TD::msg("starting SASL authentication...");
#endif
			need = NSASLFirst;
			step = GetSASLFirst;
			return false;
		}

		if(server) {
			return loginComplete();
		}
		else {
			if(!doBinding)
				return loginComplete();
		}

		// deal with bind
		if(!features.bind_supported) {
			// bind MUST be supported
			event = EError;
			errorCode = ErrProtocol;
			return true;
		}

		if (sm_resumption_supported && sm_resumption_id != "") {
			// try to resume;
			fprintf(stderr, "\tResume session\n");
			return loginComplete();
		} else {
			QDomElement e = doc.createElement("iq");
			e.setAttribute("type", "set");
			e.setAttribute("id", "bind_1");
			QDomElement b = doc.createElementNS(NS_BIND, "bind");

			// request specific resource?
			QString resource = jid_.resource();
			if(!resource.isEmpty()) {
				QDomElement r = doc.createElement("resource");
				r.appendChild(doc.createTextNode(jid_.resource()));
				b.appendChild(r);
			}

			e.appendChild(b);

			send(e);
			event = ESend;
			step = GetBindResponse;
			return true;
		}
	}
	else if(step == GetSASLFirst) {
		QDomElement e = doc.createElementNS(NS_SASL, "auth");
		e.setAttribute("mechanism", sasl_mech);
		if(!sasl_step.isEmpty()) {
#ifdef XMPP_TEST
			TD::msg(QString("SASL OUT: [%1]").arg(printArray(sasl_step)));
#endif
			e.appendChild(doc.createTextNode(QCA::Base64().arrayToString(sasl_step)));
		}

		send(e, true);
		event = ESend;
		step = GetSASLChallenge;
		return true;
	}
	else if(step == GetSASLNext) {
		if(isIncoming()) {
			if(sasl_authed) {
				QDomElement e = doc.createElementNS(NS_SASL, "success");
				writeElement(e, TypeElement, false, true);
				event = ESend;
				step = IncHandleSASLSuccess;
				return true;
			}
			else {
				QByteArray stepData = sasl_step;
				QDomElement e = doc.createElementNS(NS_SASL, "challenge");
				if(!stepData.isEmpty())
					e.appendChild(doc.createTextNode(QCA::Base64().arrayToString(stepData)));

				writeElement(e, TypeElement, false, true);
				event = ESend;
				step = GetSASLResponse;
				return true;
			}
		}
		else {
			// already authed?  then ignore last client step
			//   (this happens if "additional data with success"
			//   is used)
			if(sasl_authed)
			{
				event = ESASLSuccess;
				step = HandleSASLSuccess;
				return true;
			}

			QByteArray stepData = sasl_step;
#ifdef XMPP_TEST
			TD::msg(QString("SASL OUT: [%1]").arg(printArray(sasl_step)));
#endif
			QDomElement e = doc.createElementNS(NS_SASL, "response");
			if(!stepData.isEmpty())
				e.appendChild(doc.createTextNode(QCA::Base64().arrayToString(stepData)));

			send(e, true);
			event = ESend;
			step = GetSASLChallenge;
			return true;
		}
	}
	else if(step == HandleSASLSuccess) {
		need = NSASLLayer;
		spare = resetStream();
		step = Start;
		return false;
	}
	else if(step == HandleAuthGet) {
		QDomElement e = doc.createElement("iq");
		e.setAttribute("to", to);
		e.setAttribute("type", "get");
		e.setAttribute("id", "auth_1");
		QDomElement q = doc.createElementNS("jabber:iq:auth", "query");
		QDomElement u = doc.createElement("username");
		u.appendChild(doc.createTextNode(jid_.node()));
		q.appendChild(u);
		e.appendChild(q);

		send(e);
		event = ESend;
		step = GetAuthGetResponse;
		return true;
	}
	else if(step == HandleAuthSet) {
		QDomElement e = doc.createElement("iq");
		e.setAttribute("to", to);
		e.setAttribute("type", "set");
		e.setAttribute("id", "auth_2");
		QDomElement q = doc.createElementNS("jabber:iq:auth", "query");
		QDomElement u = doc.createElement("username");
		u.appendChild(doc.createTextNode(jid_.node()));
		q.appendChild(u);
		QDomElement p;
		if(digest) {
			// need SHA1 here
			//if(!QCA::isSupported(QCA::CAP_SHA1))
			//	QCA::insertProvider(createProviderHash());

			p = doc.createElement("digest");
			QByteArray cs = id.toUtf8() + password.toUtf8();
			p.appendChild(doc.createTextNode(QCA::Hash("sha1").hashToString(cs)));
		}
		else {
			p = doc.createElement("password");
			p.appendChild(doc.createTextNode(password));
		}
		q.appendChild(p);
		QDomElement r = doc.createElement("resource");
		r.appendChild(doc.createTextNode(jid_.resource()));
		q.appendChild(r);
		e.appendChild(q);

		send(e, true);
		event = ESend;
		step = GetAuthSetResponse;
		return true;
	}
	// server
	else if(step == SendFeatures) {
		QDomElement f = doc.createElementNS(NS_ETHERX, "stream:features");
		if(!tls_started && !sasl_authed) { // don't offer tls if we are already sasl'd
			QDomElement tls = doc.createElementNS(NS_TLS, "starttls");
			f.appendChild(tls);
		}

		if(sasl_authed) {
			if(!server) {
				QDomElement bind = doc.createElementNS(NS_BIND, "bind");
				f.appendChild(bind);
			}
		}
		else {
			QDomElement mechs = doc.createElementNS(NS_SASL, "mechanisms");
			foreach (const QString & it, sasl_mechlist) {
				QDomElement m = doc.createElement("mechanism");
				m.appendChild(doc.createTextNode(it));
				mechs.appendChild(m);
			}
			f.appendChild(mechs);
		}

		writeElement(f, TypeElement, false);
		event = ESend;
		step = GetRequest;
		return true;
	}
	// server
	else if(step == HandleTLS) {
		tls_started = true;
		need = NStartTLS;
		spare = resetStream();
		step = Start;
		return false;
	}
	// server
	else if(step == IncHandleSASLSuccess) {
		event = ESASLSuccess;
		spare = resetStream();
		step = Start;
		printf("sasl success\n");
		return true;
	}
	else if(step == GetFeatures) {
		// we are waiting for stream features
		if(e.namespaceURI() == NS_ETHERX && e.tagName() == "features") {
			// extract features
			StreamFeatures f;
			QDomElement s = e.elementsByTagNameNS(NS_TLS, "starttls").item(0).toElement();
			if(!s.isNull()) {
				f.tls_supported = true;
				f.tls_required = s.elementsByTagNameNS(NS_TLS, "required").count() > 0;
			}
			QDomElement m = e.elementsByTagNameNS(NS_SASL, "mechanisms").item(0).toElement();
			if(!m.isNull()) {
				f.sasl_supported = true;
				QDomNodeList l = m.elementsByTagNameNS(NS_SASL, "mechanism");
				for(int n = 0; n < l.count(); ++n)
					f.sasl_mechs += l.item(n).toElement().text();
			}
			QDomElement c = e.elementsByTagNameNS(NS_COMPRESS_FEATURE, "compression").item(0).toElement();
			if(!c.isNull()) {
				f.compress_supported = true;
				QDomNodeList l = c.elementsByTagNameNS(NS_COMPRESS_FEATURE, "method");
				for(int n = 0; n < l.count(); ++n)
					f.compression_mechs += l.item(n).toElement().text();
			}
			QDomElement b = e.elementsByTagNameNS(NS_BIND, "bind").item(0).toElement();
			if(!b.isNull())
				f.bind_supported = true;
			QDomElement h = e.elementsByTagNameNS(NS_HOSTS, "hosts").item(0).toElement();
			if(!h.isNull()) {
				QDomNodeList l = h.elementsByTagNameNS(NS_HOSTS, "host");
				for(int n = 0; n < l.count(); ++n)
					f.hosts += l.item(n).toElement().text();
				hosts += f.hosts;
			}

			// check for XEP-0198 support if we are already authed
			if (sasl_authed) {
				QDomElement sm = e.elementsByTagNameNS(NS_STREAM_MANAGEMENT, "sm").item(0).toElement();
				if (!sm.isNull()) f.sm_supported = true;
				else f.sm_supported = false;
			}

			if(f.tls_supported) {
#ifdef XMPP_TEST
				QString s = "STARTTLS is available";
				if(f.tls_required)
					s += " (required)";
				TD::msg(s);
#endif
			}
			if(f.sasl_supported) {
#ifdef XMPP_TEST
				QString s = "SASL mechs:";
				for(QStringList::ConstIterator it = f.sasl_mechs.begin(); it != f.sasl_mechs.end(); ++it)
					s += QString(" [%1]").arg((*it));
				TD::msg(s);
#endif
			}
			if(f.compress_supported) {
#ifdef XMPP_TEST
				QString s = "Compression mechs:";
				for(QStringList::ConstIterator it = f.compression_mechs.begin(); it != f.compression_mechs.end(); ++it)
					s += QString(" [%1]").arg((*it));
				TD::msg(s);
#endif
			}

			event = EFeatures;
			features = f;
			step = HandleFeatures;
			return true;
		}
		else {
			// ignore
		}
	}
	else if(step == GetTLSProceed) {
		// waiting for proceed to starttls
		if(e.namespaceURI() == NS_TLS) {
			if(e.tagName() == "proceed") {
#ifdef XMPP_TEST
				TD::msg("Server wants us to proceed with ssl handshake");
#endif
				tls_started = true;
				need = NStartTLS;
				spare = resetStream();
				step = Start;
				return false;
			}
			else if(e.tagName() == "failure") {
				event = EError;
				errorCode = ErrStartTLS;
				return true;
			}
			else {
				event = EError;
				errorCode = ErrProtocol;
				return true;
			}
		}
		else {
			// ignore
		}
	}
	else if(step == GetCompressProceed) {
		// waiting for proceed to compression
		if(e.namespaceURI() == NS_COMPRESS_PROTOCOL) {
			if(e.tagName() == "compressed") {
#ifdef XMPP_TEST
				TD::msg("Server wants us to proceed with compression");
#endif
				compress_started = true;
				need = NCompress;
				spare = resetStream();
				step = Start;
				return false;
			}
			else if(e.tagName() == "failure") {
				event = EError;
				errorCode = ErrCompress;
				return true;
			}
			else {
				event = EError;
				errorCode = ErrProtocol;
				return true;
			}
		}
		else {
			// ignore
		}
	}
	else if(step == GetSASLChallenge) {
		// waiting for sasl challenge/success/fail
		if(e.namespaceURI() == NS_SASL) {
			if(e.tagName() == "challenge") {
				QByteArray a = QCA::Base64().stringToArray(e.text()).toByteArray();
#ifdef XMPP_TEST
				TD::msg(QString("SASL IN: [%1]").arg(printArray(a)));
#endif
				sasl_step = a;
				need = NSASLNext;
				step = GetSASLNext;
				return false;
			}
			else if(e.tagName() == "success") {
				QString str = e.text();
				// "additional data with success" ?
				if(!str.isEmpty())
				{
					QByteArray a = QCA::Base64().stringToArray(str).toByteArray();
					sasl_step = a;
					sasl_authed = true;
					need = NSASLNext;
					step = GetSASLNext;
					return false;
				}

				sasl_authed = true;
				event = ESASLSuccess;
				step = HandleSASLSuccess;
				return true;
			}
			else if(e.tagName() == "failure") {
				QDomElement t = firstChildElement(e);
				if(t.isNull() || t.namespaceURI() != NS_SASL)
					errCond = -1;
				else
					errCond = stringToSASLCond(t.tagName());

				event = EError;
				errorCode = ErrAuth;
				return true;
			}
			else {
				event = EError;
				errorCode = ErrProtocol;
				return true;
			}
		}
	}
	else if(step == GetBindResponse) {
		if(e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
			QString type(e.attribute("type"));
			QString id(e.attribute("id"));

			if(id == "bind_1" && (type == "result" || type == "error")) {
				if(type == "result") {
					QDomElement b = e.elementsByTagNameNS(NS_BIND, "bind").item(0).toElement();
					Jid j;
					if(!b.isNull()) {
						QDomElement je = e.elementsByTagName("jid").item(0).toElement();
						j = je.text();
					}
					if(!j.isValid()) {
						event = EError;
						errorCode = ErrProtocol;
						return true;
					}
					jid_ = j;
					return loginComplete();
				}
				else {
					errCond = -1;

					QDomElement err = e.elementsByTagNameNS(NS_CLIENT, "error").item(0).toElement();
					if(!err.isNull()) {
						// get error condition
						QDomNodeList nl = err.childNodes();
						QDomElement t;
						for(int n = 0; n < nl.count(); ++n) {
							QDomNode i = nl.item(n);
							if(i.isElement()) {
								t = i.toElement();
								break;
							}
						}
						if(!t.isNull() && t.namespaceURI() == NS_STANZAS) {
							QString cond = t.tagName();
							if(cond == "not-allowed")
								errCond = BindNotAllowed;
							else if(cond == "conflict")
								errCond = BindConflict;
						}
					}

					event = EError;
					errorCode = ErrBind;
					return true;
				}
			}
			else {
				// ignore
			}
		}
		else {
			// ignore
		}
	}
	else if(step == GetAuthGetResponse) {
		// waiting for an iq
		if(e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
			Jid from(e.attribute("from"));
			QString type(e.attribute("type"));
			QString id(e.attribute("id"));

			bool okfrom = (from.isEmpty() || from.compare(Jid(to)));
			if(okfrom && id == "auth_1" && (type == "result" || type == "error")) {
				if(type == "result") {
					QDomElement q = e.elementsByTagNameNS("jabber:iq:auth", "query").item(0).toElement();
					if(q.isNull() || q.elementsByTagName("username").item(0).isNull() || q.elementsByTagName("resource").item(0).isNull()) {
						event = EError;
						errorCode = ErrProtocol;
						return true;
					}
					bool plain_supported = !q.elementsByTagName("password").item(0).isNull();
					bool digest_supported = !q.elementsByTagName("digest").item(0).isNull();

					if(!digest_supported && !plain_supported) {
						event = EError;
						errorCode = ErrProtocol;
						return true;
					}

					// plain text not allowed?
					if(!digest_supported && !allowPlain) {
						event = EError;
						errorCode = ErrPlain;
						return true;
					}

					digest = digest_supported;
					need = NPassword;
					step = HandleAuthSet;
					return false;
				}
				else {
					errCond = getOldErrorCode(e);

					event = EError;
					errorCode = ErrAuth;
					return true;
				}
			}
			else {
				// ignore
			}
		}
		else {
			// ignore
		}
	}
	else if(step == GetAuthSetResponse) {
		// waiting for an iq
		if(e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
			Jid from(e.attribute("from"));
			QString type(e.attribute("type"));
			QString id(e.attribute("id"));

			bool okfrom = (from.isEmpty() || from.compare(Jid(to)));
			if(okfrom && id == "auth_2" && (type == "result" || type == "error")) {
				if(type == "result") {
					return loginComplete();
				}
				else {
					errCond = getOldErrorCode(e);

					event = EError;
					errorCode = ErrAuth;
					return true;
				}
			}
			else {
				// ignore
			}
		}
		else {
			// ignore
		}
	}
	// server
	else if(step == GetRequest) {
		printf("get request: [%s], %s\n", e.namespaceURI().toLatin1().data(), e.tagName().toLatin1().data());
		if(e.namespaceURI() == NS_TLS && e.localName() == "starttls") {
			// TODO: don't let this be done twice

			QDomElement e = doc.createElementNS(NS_TLS, "proceed");
			writeElement(e, TypeElement, false, true);
			event = ESend;
			step = HandleTLS;
			return true;
		}
		if(e.namespaceURI() == NS_SASL) {
			if(e.localName() == "auth") {
				if(sasl_started) {
					// TODO
					printf("error\n");
					return false;
				}

				sasl_started = true;
				sasl_mech = e.attribute("mechanism");
				// TODO: if child text missing, don't pass it
				sasl_step = QCA::Base64().stringToArray(e.text()).toByteArray();
				need = NSASLFirst;
				step = GetSASLNext;
				return false;
			}
			else {
				// TODO
				printf("unknown sasl tag\n");
				return false;
			}
		}
		if(e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
			QDomElement b = e.elementsByTagNameNS(NS_BIND, "bind").item(0).toElement();
			if(!b.isNull()) {
				QDomElement res = b.elementsByTagName("resource").item(0).toElement();
				QString resource = res.text();

				QDomElement r = doc.createElement("iq");
				r.setAttribute("type", "result");
				r.setAttribute("id", e.attribute("id"));
				QDomElement bind = doc.createElementNS(NS_BIND, "bind");
				QDomElement jid = doc.createElement("jid");
				Jid j = QString(user + '@' + host + '/' + resource);
				jid.appendChild(doc.createTextNode(j.full()));
				bind.appendChild(jid);
				r.appendChild(bind);

				writeElement(r, TypeElement, false);
				event = ESend;
				// TODO
				return true;
			}
			else {
				// TODO
			}
		}
	}
	else if(step == GetSASLResponse) {
		if(e.namespaceURI() == NS_SASL && e.localName() == "response") {
			sasl_step = QCA::Base64().stringToArray(e.text()).toByteArray();
			need = NSASLNext;
			step = GetSASLNext;
			return false;
		}
	}
	else if(step == GetSMResponse) {
#ifdef IRIS_SM_DEBUG
		qWarning() << "HandleSM: step";
#endif
		if (e.namespaceURI() == NS_STREAM_MANAGEMENT) {
			if (e.localName() == "enabled") {
#ifdef IRIS_SM_DEBUG
				qWarning() << "Stream Management enabled";
#endif
				sm_started = true;
				if (e.attribute("resume", "false") == "true" || e.attribute("resume", "false") == "1") {
#ifdef IRIS_SM_DEBUG
					qDebug("\tResumption Supported");
#endif
					sm_resumption_supported = true;
					sm_resumption_id = e.attribute("id", "");

					if (!e.attribute("location").isEmpty()) {
						int port_off = 0;
						QStringRef sm_host;
						int sm_port = -1;
						QString location = e.attribute("location");
						if (location[0] == '[') { // ipv6
							port_off = location.indexOf(']');
							if (port_off != -1) { // looks valid
								sm_host = location.midRef(1, port_off - 1);
							}
						}
						if (port_off != -1) { // -1 means previous ipv6 parse failed
							port_off = location.indexOf(':');
							if (port_off != -1) {
								sm_host = location.leftRef(port_off);
								sm_port = location.mid(port_off + 1).toInt();
							} else {
								sm_host = location.midRef(0);
							}
						}
						if (!sm_host.isEmpty() && sm_port) {
							sm_resumption_location = QPair<QString,int>(sm_host.toString(), sm_port);
						}
					}

					startTimer(20);
					event = EReady;
					step = Done;
					return true;
				}
			} else if (e.localName() == "resumed") {
				processSMAcknowlegement(e.attribute("h").toULong());
				startTimer(20);
				event = EReady;
				step = Done;
				return true;
			} else if (e.localName() == "failed") {
				if (!sm_resumption_id.isEmpty()) { // tried to resume? ok, then try to just enable
					sm_resumption_id.clear();
					step = HandleFeatures;
					event = EFeatures;
					return true;
				}
			}
		}
	}

	if (isStreamManagementActive()) {
		if (sm_ack_last_requested.elapsed() >= 20000) {
			requestSMAcknowlegement();
		}
	}

	if(isReady() && !e.isNull()) {
		if(isValidStanza(e)) {
			stanzaToRecv = e;
			event = EStanzaReady;
			setIncomingAsExternal();
			return true;
		} else if (sm_started) {
			return streamManagementHandleStanza(e);
		}
	}

	need = NNotify;
	notify |= NRecv;
	return false;
}
