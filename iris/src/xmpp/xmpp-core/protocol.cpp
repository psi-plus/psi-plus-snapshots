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
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

// TODO: let the app know if tls is required
//       require mutual auth for server out/in
//       report ErrProtocol if server uses wrong NS

#include "protocol.h"

#ifdef XMPP_TEST
#include "td.h"
#endif

#include <QByteArray>
#include <QList>
#include <QtCrypto>
#include <qca.h>

using namespace XMPP;

// printArray
//
// This function prints out an array of bytes as latin characters, converting
// non-printable bytes into hex values as necessary.  Useful for displaying
// QByteArrays for debugging purposes.
static QString printArray(const QByteArray &a)
{
    QString s;
    for (int n = 0; n < a.size(); ++n) {
        unsigned char c = (unsigned char)a[(int)n];
        if (c < 32 || c >= 127) {
            QString str = QString::asprintf("[%02x]", c);
            s += str;
        } else
            s += QChar::fromLatin1(c);
    }
    return s;
}

// firstChildElement
//
// Get an element's first child element
static QDomElement firstChildElement(const QDomElement &e)
{
    for (QDomNode n = e.firstChild(); !n.isNull(); n = n.nextSibling()) {
        if (n.isElement())
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
    tls_supported      = false;
    sasl_supported     = false;
    bind_supported     = false;
    tls_required       = false;
    compress_supported = false;
    sm_supported       = false;
    session_supported  = false;
    session_required   = false;
}

//----------------------------------------------------------------------------
// BasicProtocol
//----------------------------------------------------------------------------
BasicProtocol::SASLCondEntry BasicProtocol::saslCondTable[] = {
    { "aborted", Aborted },
    { "account-disabled", AccountDisabled },
    { "credentials-expired", CredentialsExpired },
    { "encryption-required", EncryptionRequired },
    { "incorrect-encoding", IncorrectEncoding },
    { "invalid-authzid", InvalidAuthzid },
    { "invalid-mechanism", InvalidMech },
    { "malformed-request", MalformedRequest },
    { "mechanism-too-weak", MechTooWeak },
    { "not-authorized", NotAuthorized },
    { "temporary-auth-failure", TemporaryAuthFailure },
    { nullptr, 0 },
};

BasicProtocol::StreamCondEntry BasicProtocol::streamCondTable[] = {
    { "bad-format", BadFormat },
    { "bad-namespace-prefix", BadNamespacePrefix },
    { "conflict", Conflict },
    { "connection-timeout", ConnectionTimeout },
    { "host-gone", HostGone },
    { "host-unknown", HostUnknown },
    { "improper-addressing", ImproperAddressing },
    { "internal-server-error", InternalServerError },
    { "invalid-from", InvalidFrom },
    { "invalid-namespace", InvalidNamespace },
    { "invalid-xml", InvalidXml },
    { "not-authorized", StreamNotAuthorized },
    { "not-well-formed", NotWellFormed },
    { "policy-violation", PolicyViolation },
    { "remote-connection-failed", RemoteConnectionFailed },
    { "reset", StreamReset },
    { "resource-constraint", ResourceConstraint },
    { "restricted-xml", RestrictedXml },
    { "see-other-host", SeeOtherHost },
    { "system-shutdown", SystemShutdown },
    { "undefined-condition", UndefinedCondition },
    { "unsupported-encoding", UnsupportedEncoding },
    { "unsupported-stanza-type", UnsupportedStanzaType },
    { "unsupported-version", UnsupportedVersion },
    { nullptr, 0 },
};

BasicProtocol::BasicProtocol() : XmlProtocol() { init(); }

BasicProtocol::~BasicProtocol() { }

void BasicProtocol::init()
{
    errCond        = -1;
    sasl_authed    = false;
    doShutdown     = false;
    delayedError   = false;
    closeError     = false;
    ready          = false;
    stanzasPending = 0;
    stanzasWritten = 0;
}

void BasicProtocol::reset()
{
    XmlProtocol::reset();
    init();

    to         = QString();
    from       = QString();
    id         = QString();
    lang       = QString();
    version    = Version(1, 0);
    errText    = QString();
    errAppSpec = QDomElement();
    otherHost  = QString();
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

void BasicProtocol::clearSendQueue()
{
    sendList.clear();
    XmlProtocol::clearSendQueue();
}

QDomElement BasicProtocol::recvStanza()
{
    QDomElement e = stanzaToRecv;
    stanzaToRecv  = QDomElement();
    return e;
}

void BasicProtocol::shutdown() { doShutdown = true; }

void BasicProtocol::shutdownWithError(int cond, const QString &str)
{
    otherHost = str;
    delayErrorAndClose(cond);
}

bool BasicProtocol::isReady() const { return ready; }

void BasicProtocol::setReady(bool b) { ready = b; }

QString BasicProtocol::saslMech() const { return sasl_mech; }

QByteArray BasicProtocol::saslStep() const { return sasl_step; }

void BasicProtocol::setSASLMechList(const QStringList &list) { sasl_mechlist = list; }

void BasicProtocol::setSASLFirst(const QString &mech, const QByteArray &step)
{
    sasl_mech = mech;
    sasl_step = step;
}

void BasicProtocol::setSASLNext(const QByteArray &step) { sasl_step = step; }

void BasicProtocol::setSASLAuthed() { sasl_authed = true; }

int BasicProtocol::stringToSASLCond(const QString &s)
{
    for (int n = 0; saslCondTable[n].str; ++n) {
        if (s == saslCondTable[n].str)
            return saslCondTable[n].cond;
    }
    return -1;
}

int BasicProtocol::stringToStreamCond(const QString &s)
{
    for (int n = 0; streamCondTable[n].str; ++n) {
        if (s == streamCondTable[n].str)
            return streamCondTable[n].cond;
    }
    return -1;
}

QString BasicProtocol::saslCondToString(int x)
{
    for (int n = 0; saslCondTable[n].str; ++n) {
        if (x == saslCondTable[n].cond)
            return saslCondTable[n].str;
    }
    return QString();
}

QString BasicProtocol::streamCondToString(int x)
{
    for (int n = 0; streamCondTable[n].str; ++n) {
        if (x == streamCondTable[n].cond)
            return streamCondTable[n].str;
    }
    return QString();
}

void BasicProtocol::extractStreamError(const QDomElement &e)
{
    QString                 text;
    QHash<QString, QString> langText;
    QDomElement             appSpec;

    QDomElement t = firstChildElement(e);
    if (t.isNull() || t.namespaceURI() != NS_STREAMS) {
        // probably old-style error
        errCond = -1;
        errText = e.text();
    } else
        errCond = stringToStreamCond(t.tagName());

    if (errCond != -1) {
        if (errCond == SeeOtherHost)
            otherHost = t.text();

        auto nodes = e.elementsByTagNameNS(NS_STREAMS, "text");
        if (nodes.count()) {
            for (int i = 0; i < nodes.count(); i++) {
                auto    e    = nodes.item(i).toElement();
                QString lang = e.attributeNS(NS_STREAMS, "lang", "");
                langText.insert(lang, e.text());
            }
        } else
            text = t.text();

        // find first non-standard namespaced element
        QDomNodeList nl = e.childNodes();
        for (int n = 0; n < nl.count(); ++n) {
            QDomNode i = nl.item(n);
            if (i.isElement() && i.namespaceURI() != NS_STREAMS) {
                appSpec = i.toElement();
                break;
            }
        }

        errText     = text;
        errLangText = langText;
        errAppSpec  = appSpec;
    }
}

void BasicProtocol::send(const QDomElement &e, bool clip) { writeElement(e, TypeElement, false, clip, false); }

void BasicProtocol::sendUrgent(const QDomElement &e, bool clip) { writeElement(e, TypeElement, false, clip, true); }

void BasicProtocol::sendStreamError(int cond, const QString &text, const QDomElement &appSpec)
{
    QDomElement se  = doc.createElementNS(NS_ETHERX, "stream:error");
    QDomElement err = doc.createElementNS(NS_STREAMS, streamCondToString(cond));
    if (!otherHost.isEmpty())
        err.appendChild(doc.createTextNode(otherHost));
    se.appendChild(err);
    if (!text.isEmpty()) {
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
    errCond    = cond;
    errText    = text;
    errAppSpec = appSpec;
    sendStreamError(cond, text, appSpec);
    return close();
}

bool BasicProtocol::error(int code)
{
    event     = EError;
    errorCode = code;
    return true;
}

void BasicProtocol::delayErrorAndClose(int cond, const QString &text, const QDomElement &appSpec)
{
    errorCode    = ErrStream;
    errCond      = cond;
    errText      = text;
    errAppSpec   = appSpec;
    delayedError = true;
}

void BasicProtocol::delayError(int code)
{
    errorCode    = code;
    delayedError = true;
}

QDomElement BasicProtocol::docElement()
{
    // create the root element
    QDomElement e = doc.createElementNS(NS_ETHERX, "stream:stream");

    QString           defns = defaultNamespace();
    const QStringList list  = extraNamespaces();

    // HACK: using attributes seems to be the only way to get additional namespaces in here
    if (!defns.isEmpty())
        e.setAttribute(QString::fromLatin1("xmlns"), defns);
    for (QStringList::ConstIterator it = list.begin(); it != list.end();) {
        QString prefix = *(it++);
        QString uri    = *(it++);
        e.setAttribute(QString::fromLatin1("xmlns:") + prefix, uri);
    }

    // additional attributes
    if (!isIncoming() && !to.isEmpty())
        e.setAttribute(QString::fromLatin1("to"), to);
    if (isIncoming() && !from.isEmpty())
        e.setAttribute(QString::fromLatin1("from"), from);
    if (!id.isEmpty())
        e.setAttribute(QString::fromLatin1("id"), id);
    if (!lang.isEmpty())
        e.setAttributeNS(QString::fromLatin1(NS_XML), QString::fromLatin1("xml:lang"), lang);
    if (version.major > 0 || version.minor > 0)
        e.setAttribute(QString::fromLatin1("version"),
                       QString::number(version.major) + '.' + QString::number(version.minor));

    return e;
}

void BasicProtocol::handleDocOpen(const Parser::Event &pe)
{
    if (isIncoming()) {
        if (xmlEncoding() != "UTF-8") {
            delayErrorAndClose(UnsupportedEncoding);
            return;
        }
    }

    if (pe.namespaceURI() == NS_ETHERX && pe.localName() == "stream") {
        auto atts = pe.atts();

        // grab the version
        int  major  = 0;
        int  minor  = 0;
        auto verstr = atts.value("version");
        if (!verstr.isEmpty()) {
            int n = verstr.indexOf('.');
            if (n != -1) {
                major = verstr.mid(0, n).toInt();
                minor = verstr.mid(n + 1).toInt();
            } else {
                major = verstr.toInt();
                minor = 0;
            }
        }
        version = Version(major, minor);

        if (isIncoming()) {
            to            = atts.value("to").toString();
            auto peerLang = atts.value(NS_XML, "lang");
            if (!peerLang.isEmpty())
                lang = peerLang.toString();
        }
        // outgoing
        else {
            from = atts.value("from").toString();
            lang = atts.value(NS_XML, "lang").toString();
            id   = atts.value("id").toString();
        }

        handleStreamOpen(pe);
    } else {
        if (isIncoming())
            delayErrorAndClose(BadFormat);
        else
            delayError(ErrProtocol);
    }
}

bool BasicProtocol::handleError()
{
    if (isIncoming())
        return errorAndClose(NotWellFormed);
    else
        return error(ErrParse);
}

bool BasicProtocol::handleCloseFinished()
{
    if (closeError) {
        event     = EError;
        errorCode = ErrStream;
        // note: errCond and friends are already set at this point
    } else
        event = EClosed;
    return true;
}

bool BasicProtocol::doStep(const QDomElement &e)
{
    // handle pending error
    if (delayedError) {
        if (isIncoming())
            return errorAndClose(errCond, errText, errAppSpec);
        else
            return error(errorCode);
    }

    // shutdown?
    if (doShutdown) {
        doShutdown = false;
        return close();
    }

    if (!e.isNull()) {
        // check for error
        if (e.namespaceURI() == NS_ETHERX && e.tagName() == "error") {
            extractStreamError(e);
            return error(ErrStream);
        }
    }

    if (ready) {
        // stanzas written?
        if (stanzasWritten > 0) {
            --stanzasWritten;
            event = EStanzaSent;
            return true;
        }
        // send items?
        if (!sendList.isEmpty()) {
            SendItem i;
            {
                QList<SendItem>::Iterator it = sendList.begin();
                i                            = (*it);
                sendList.erase(it);
            }

            // outgoing stanza?
            if (!i.stanzaToSend.isNull()) {
                ++stanzasPending;
                writeElement(i.stanzaToSend, TypeStanza, true);
                event = ESend;
            }
            // direct send?
            else if (!i.stringToSend.isEmpty()) {
                writeString(i.stringToSend, TypeDirect, true);
                event = ESend;
            }
            // whitespace keepalive?
            else if (i.doWhitespace) {
                writeString("\n", TypePing, false);
                event = ESend;
            }
            return true;
        } else {
            // if we have pending outgoing stanzas, ask for write notification
            if (stanzasPending)
                notify |= NSend;
        }
    }

    return doStep2(e);
}

void BasicProtocol::itemWritten(int id, int)
{
    if (id == TypeStanza) {
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
CoreProtocol::CoreProtocol() : BasicProtocol() { init(); }

CoreProtocol::~CoreProtocol()
{
    // fprintf(stderr, "\tCoreProtocol::~CoreProtocol()\n");
}

void CoreProtocol::init()
{
    step = Start;

    // ??
    server          = false;
    dialback        = false;
    dialback_verify = false;

    // settings
    jid_       = Jid();
    password   = QString();
    oldOnly    = false;
    allowPlain = false;
    doTLS      = true;
    doAuth     = true;
    doCompress = true;
    doBinding  = true;

    // input
    user = QString();
    host = QString();

    // status
    old              = false;
    digest           = false;
    tls_started      = false;
    sasl_started     = false;
    compress_started = false;

    sm.reset();
}

void CoreProtocol::reset()
{
    BasicProtocol::reset();
    init();
}

void CoreProtocol::needTimer(int seconds)
{
    notify |= NTimeout;
    need        = NNotify;
    timeout_sec = seconds;
}

void CoreProtocol::sendStanza(const QDomElement &e)
{
    if (sm.isActive()) {
        int len = sm.addUnacknowledgedStanza(e);
        if (len > 5 && len % 4 == 0)
            if (needSMRequest())
                event = ESend;
    }
    BasicProtocol::sendStanza(e);
}

void CoreProtocol::startClientOut(const Jid &_jid, bool _oldOnly, bool tlsActive, bool _doAuth, bool _doCompress)
{
    jid_        = _jid;
    to          = _jid.domain();
    oldOnly     = _oldOnly;
    doAuth      = _doAuth;
    doCompress  = _doCompress;
    tls_started = tlsActive;

    if (oldOnly)
        version = Version(0, 0);
    startConnect();
}

void CoreProtocol::startServerOut(const QString &_to)
{
    server = true;
    to     = _to;
    startConnect();
}

void CoreProtocol::startDialbackOut(const QString &_to, const QString &_from)
{
    server    = true;
    dialback  = true;
    to        = _to;
    self_from = _from;
    startConnect();
}

void CoreProtocol::startDialbackVerifyOut(const QString &_to, const QString &_from, const QString &id,
                                          const QString &key)
{
    server          = true;
    dialback        = true;
    dialback_verify = true;
    to              = _to;
    self_from       = _from;
    dialback_id     = id;
    dialback_key    = key;
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
    id     = _id;
    startAccept();
}

void CoreProtocol::setLang(const QString &s) { lang = s; }

void CoreProtocol::setAllowTLS(bool b) { doTLS = b; }

void CoreProtocol::setAllowBind(bool b) { doBinding = b; }

void CoreProtocol::setAllowPlain(bool b) { allowPlain = b; }

const Jid &CoreProtocol::jid() const { return jid_; }

void CoreProtocol::setPassword(const QString &s) { password = s; }

void CoreProtocol::setFrom(const QString &s) { from = s; }

void CoreProtocol::setDialbackKey(const QString &s) { dialback_key = s; }

bool CoreProtocol::loginComplete()
{
    setReady(true);

    // deal with stream management
    if (features.sm_supported && sm.state().isEnabled() && !sm.isActive()) {
        if (sm.state().isResumption()) {
            QDomElement e = doc.createElementNS(NS_STREAM_MANAGEMENT, "resume");
            e.setAttribute("previd", sm.state().resumption_id);
            e.setAttribute("h", sm.state().received_count);
            send(e);
        } else {
            QDomElement e = doc.createElementNS(NS_STREAM_MANAGEMENT, "enable");
            e.setAttribute("resume", "true");
            send(e);
        }
        event = ESend;
        step  = GetSMResponse;
    } else {
        event = EReady;
        step  = Done;
    }
    return true;
}

int CoreProtocol::getOldErrorCode(const QDomElement &e)
{
    QDomElement err = e.elementsByTagNameNS(NS_CLIENT, "error").item(0).toElement();
    if (err.isNull() || !err.hasAttribute("code"))
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
    if (stepRequiresElement())
        return true;
    else if (isReady())
        return true;
    return false;
}

// all element-needing steps need to be registered here
bool CoreProtocol::stepRequiresElement() const
{
    switch (step) {
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
    if (server)
        return NS_SERVER;
    else
        return NS_CLIENT;
}

QStringList CoreProtocol::extraNamespaces()
{
    QStringList list;
    if (dialback) {
        list += "db";
        list += NS_DIALBACK;
    }
    return list;
}

void CoreProtocol::handleStreamOpen(const Parser::Event &pe)
{
    if (isIncoming()) {
        QString ns = pe.nsprefix();
        QString db;
        if (server) {
            db = pe.nsprefix("db");
            if (!db.isEmpty())
                dialback = true;
        }

        // verify namespace
        if ((!server && ns != NS_CLIENT) || (server && ns != NS_SERVER) || (dialback && db != NS_DIALBACK)) {
            delayErrorAndClose(InvalidNamespace);
            return;
        }

        // verify version
        if (version.major < 1 && !dialback) {
            delayErrorAndClose(UnsupportedVersion);
            return;
        }
    } else {
        if (!dialback) {
            old = version.major < 1 || oldOnly;
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
    if (dialback)
        return dialbackStep(e);
    else
        return normalStep(e);
}

bool CoreProtocol::isValidStanza(const QDomElement &e) const
{
    QString      s    = e.tagName();
    Stanza::Kind kind = Stanza::kind(s);
    return e.namespaceURI() == (server ? NS_SERVER : NS_CLIENT)
        && (kind == Stanza::Message || kind == Stanza::Presence || kind == Stanza::IQ);
}

bool CoreProtocol::streamManagementHandleStanza(const QDomElement &e)
{
    QString s = e.tagName();
    if (s == "r") {
#ifdef IRIS_SM_DEBUG
        qDebug() << "Stream Management: [<-?] Received request from server";
#endif
        sendUrgent(sm.makeResponseStanza(doc));
        event = ESend;
        return true;
    } else if (s == "a") {
        quint32 last_id = e.attribute("h").toUInt();
#ifdef IRIS_SM_DEBUG
        qDebug() << "Stream Management: [<--] Received ack response from server with h =" << last_id;
#endif
        sm.processAcknowledgement(last_id);
        needTimer(SM_TIMER_INTERVAL_SECS);
        event = EAck;
        return true;
    } else {
        need = NNotify;
        notify |= NRecv;
        return false;
    }
}

bool CoreProtocol::needSMRequest()
{
    QDomElement e = sm.generateRequestStanza(doc);
    if (!e.isNull()) {
        send(e);
        needTimer(SM_TIMER_INTERVAL_SECS);
        return true;
    }
    return false;
}

bool CoreProtocol::grabPendingItem(const Jid &to, const Jid &from, int type, DBItem *item)
{
    for (QList<DBItem>::Iterator it = dbpending.begin(); it != dbpending.end(); ++it) {
        const DBItem &i = *it;
        if (i.type == type && i.to.compare(to) && i.from.compare(from)) {
            const DBItem &i = (*it);
            *item           = i;
            dbpending.erase(it);
            return true;
        }
    }
    return false;
}

bool CoreProtocol::dialbackStep(const QDomElement &e)
{
    if (step == Start) {
        setReady(true);
        step  = Done;
        event = EReady;
        return true;
    }

    if (!dbrequests.isEmpty()) {
        // process a request
        DBItem i;
        {
            QList<DBItem>::Iterator it = dbrequests.begin();
            i                          = (*it);
            dbrequests.erase(it);
        }

        QDomElement r;
        if (i.type == DBItem::ResultRequest) {
            r = doc.createElementNS(NS_DIALBACK, "db:result");
            r.setAttribute("to", i.to.full());
            r.setAttribute("from", i.from.full());
            r.appendChild(doc.createTextNode(i.key));
            dbpending += i;
        } else if (i.type == DBItem::ResultGrant) {
            r = doc.createElementNS(NS_DIALBACK, "db:result");
            r.setAttribute("to", i.to.full());
            r.setAttribute("from", i.from.full());
            r.setAttribute("type", i.ok ? "valid" : "invalid");
            if (i.ok) {
                i.type = DBItem::Validated;
                dbvalidated += i;
            } else {
                // TODO: disconnect after writing element
            }
        } else if (i.type == DBItem::VerifyRequest) {
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

        send(r);
        event = ESend;
        return true;
    }

    if (!e.isNull()) {
        if (e.namespaceURI() == NS_DIALBACK) {
            if (e.tagName() == "result") {
                Jid to(Jid(e.attribute("to")).domain());
                Jid from(Jid(e.attribute("from")).domain());
                if (isIncoming()) {
                    [[maybe_unused]] QString key = e.text();
                    // TODO: report event
                } else {
                    bool   ok = e.attribute("type") == "valid";
                    DBItem i;
                    if (grabPendingItem(from, to, DBItem::ResultRequest, &i)) {
                        if (ok) {
                            i.type = DBItem::Validated;
                            i.ok   = true;
                            dbvalidated += i;
                            // TODO: report event
                        } else {
                            // TODO: report event
                        }
                    }
                }
            } else if (e.tagName() == "verify") {
                Jid                      to(Jid(e.attribute("to")).domain());
                Jid                      from(Jid(e.attribute("from")).domain());
                [[maybe_unused]] QString id = e.attribute("id");
                if (isIncoming()) {
                    [[maybe_unused]] QString key = e.text();
                    // TODO: report event
                } else {
                    bool   ok = e.attribute("type") == "valid";
                    DBItem i;
                    if (grabPendingItem(from, to, DBItem::VerifyRequest, &i)) {
                        if (ok) {
                            // TODO: report event
                        } else {
                            // TODO: report event
                        }
                    }
                }
            }
        } else {
            if (isReady()) {
                if (isValidStanza(e)) {
                    // TODO: disconnect if stanza is from unverified sender
                    // TODO: ignore packets from receiving servers
                    stanzaToRecv = e;
                    event        = EStanzaReady;
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
    if (step == Start) {
        if (isIncoming()) {
            need = NSASLMechs;
            step = SendFeatures;
            return false;
        } else {
            if (old) {
                if (doAuth)
                    step = HandleAuthGet;
                else
                    return loginComplete();
            } else
                step = GetFeatures;

            return processStep();
        }
    } else if (step == HandleFeatures) {
        // deal with TLS?
        if (doTLS && !tls_started && !sasl_authed && features.tls_supported) {
            QDomElement e = doc.createElementNS(NS_TLS, "starttls");

            send(e, true);
            event = ESend;
            step  = GetTLSProceed;
            return true;
        }

        // Should we go further ?
        if (!doAuth)
            return loginComplete();

        // Deal with compression
        if (doCompress && !compress_started && features.compress_supported
            && features.compression_mechs.contains("zlib")) {
            QDomElement e = doc.createElementNS(NS_COMPRESS_PROTOCOL, "compress");
            QDomElement m = doc.createElementNS(NS_COMPRESS_PROTOCOL, "method");
            m.appendChild(doc.createTextNode("zlib"));
            e.appendChild(m);
            send(e, true);
            event = ESend;
            step  = GetCompressProceed;
            return true;
        }

        // deal with SASL?
        if (!sasl_authed) {
            if (!features.sasl_supported) {
                // SASL MUST be supported
                // event = EError;
                // errorCode = ErrProtocol;
                // return true;

                // Fall back on auth for non-compliant servers
                step = HandleAuthGet;
                old  = true;
                return true;
            }

#ifdef XMPP_TEST
            TD::msg("starting SASL authentication...");
#endif
            need = NSASLFirst;
            step = GetSASLFirst;
            return false;
        }

        if (server) {
            return loginComplete();
        } else {
            if (!doBinding)
                return loginComplete();
        }

        // deal with bind
        if (!features.bind_supported) {
            // bind MUST be supported
            event     = EError;
            errorCode = ErrProtocol;
            return true;
        }

        if (sm.state().isResumption()) {
            // try to resume;
            return loginComplete();
        } else {
            QDomElement e = doc.createElement("iq");
            e.setAttribute("type", "set");
            e.setAttribute("id", "bind_1");
            QDomElement b = doc.createElementNS(NS_BIND, "bind");

            // request specific resource?
            QString resource = jid_.resource();
            if (!resource.isEmpty()) {
                QDomElement r = doc.createElement("resource");
                r.appendChild(doc.createTextNode(jid_.resource()));
                b.appendChild(r);
            }

            e.appendChild(b);

            send(e);
            event = ESend;
            step  = GetBindResponse;
            return true;
        }
    } else if (step == GetSASLFirst) {
        QDomElement e = doc.createElementNS(NS_SASL, "auth");
        e.setAttribute("mechanism", sasl_mech);
        if (!sasl_step.isEmpty()) {
#ifdef XMPP_TEST
            TD::msg(QString("SASL OUT: [%1]").arg(printArray(sasl_step)));
#endif
            e.appendChild(doc.createTextNode(QCA::Base64().arrayToString(sasl_step)));
        }

        send(e, true);
        event = ESend;
        step  = GetSASLChallenge;
        return true;
    } else if (step == GetSASLNext) {
        if (isIncoming()) {
            if (sasl_authed) {
                QDomElement e = doc.createElementNS(NS_SASL, "success");
                send(e, true);
                event = ESend;
                step  = IncHandleSASLSuccess;
                return true;
            } else {
                QByteArray  stepData = sasl_step;
                QDomElement e        = doc.createElementNS(NS_SASL, "challenge");
                if (!stepData.isEmpty())
                    e.appendChild(doc.createTextNode(QCA::Base64().arrayToString(stepData)));

                send(e, true);
                event = ESend;
                step  = GetSASLResponse;
                return true;
            }
        } else {
            // already authed?  then ignore last client step
            //   (this happens if "additional data with success"
            //   is used)
            if (sasl_authed) {
                event = ESASLSuccess;
                step  = HandleSASLSuccess;
                return true;
            }

            QByteArray stepData = sasl_step;
#ifdef XMPP_TEST
            TD::msg(QString("SASL OUT: [%1]").arg(printArray(sasl_step)));
#endif
            QDomElement e = doc.createElementNS(NS_SASL, "response");
            if (!stepData.isEmpty())
                e.appendChild(doc.createTextNode(QCA::Base64().arrayToString(stepData)));

            send(e, true);
            event = ESend;
            step  = GetSASLChallenge;
            return true;
        }
    } else if (step == HandleSASLSuccess) {
        need  = NSASLLayer;
        spare = resetStream();
        step  = Start;
        return false;
    } else if (step == HandleAuthGet) {
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
        step  = GetAuthGetResponse;
        return true;
    } else if (step == HandleAuthSet) {
        QDomElement e = doc.createElement("iq");
        e.setAttribute("to", to);
        e.setAttribute("type", "set");
        e.setAttribute("id", "auth_2");
        QDomElement q = doc.createElementNS("jabber:iq:auth", "query");
        QDomElement u = doc.createElement("username");
        u.appendChild(doc.createTextNode(jid_.node()));
        q.appendChild(u);
        QDomElement p;
        if (digest) {
            // need SHA1 here
            // if(!QCA::isSupported(QCA::CAP_SHA1))
            //    QCA::insertProvider(createProviderHash());

            p             = doc.createElement("digest");
            QByteArray cs = id.toUtf8() + password.toUtf8();
            p.appendChild(doc.createTextNode(QCA::Hash("sha1").hashToString(cs)));
        } else {
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
        step  = GetAuthSetResponse;
        return true;
    }
    // server
    else if (step == SendFeatures) {
        QDomElement f = doc.createElementNS(NS_ETHERX, "stream:features");
        if (!tls_started && !sasl_authed) { // don't offer tls if we are already sasl'd
            QDomElement tls = doc.createElementNS(NS_TLS, "starttls");
            f.appendChild(tls);
        }

        if (sasl_authed) {
            if (!server) {
                QDomElement bind = doc.createElementNS(NS_BIND, "bind");
                f.appendChild(bind);
            }
        } else {
            QDomElement mechs = doc.createElementNS(NS_SASL, "mechanisms");
            for (const QString &it : std::as_const(sasl_mechlist)) {
                QDomElement m = doc.createElement("mechanism");
                m.appendChild(doc.createTextNode(it));
                mechs.appendChild(m);
            }
            f.appendChild(mechs);
        }

        send(f);
        event = ESend;
        step  = GetRequest;
        return true;
    }
    // server
    else if (step == HandleTLS) {
        tls_started = true;
        need        = NStartTLS;
        spare       = resetStream();
        step        = Start;
        return false;
    }
    // server
    else if (step == IncHandleSASLSuccess) {
        event = ESASLSuccess;
        spare = resetStream();
        step  = Start;
        printf("sasl success\n");
        return true;
    } else if (step == GetFeatures) {
        // we are waiting for stream features
        if (e.namespaceURI() == NS_ETHERX && e.tagName() == QLatin1String("features")) {
            // extract features
            StreamFeatures     f;
            QDomNodeList       nl = e.childNodes();
            QList<QDomElement> unhandled;
            for (int i = 0; i < nl.size(); i++) {
                QDomElement c = nl.item(i).toElement();
                if (c.isNull()) {
                    continue;
                }
                if (c.localName() == QLatin1String("starttls") && c.namespaceURI() == NS_TLS) {
                    f.tls_supported = true;
                    f.tls_required  = c.elementsByTagNameNS(NS_TLS, QLatin1String("required")).count() > 0;

                } else if (c.localName() == QLatin1String("mechanisms") && c.namespaceURI() == NS_SASL) {
                    f.sasl_supported = true;
                    QDomNodeList l   = c.elementsByTagNameNS(NS_SASL, QLatin1String("mechanism"));
                    for (int n = 0; n < l.count(); ++n)
                        f.sasl_mechs += l.item(n).toElement().text();

                } else if (c.localName() == QLatin1String("compression") && c.namespaceURI() == NS_COMPRESS_FEATURE) {
                    f.compress_supported = true;
                    QDomNodeList l       = c.elementsByTagNameNS(NS_COMPRESS_FEATURE, QLatin1String("method"));
                    for (int n = 0; n < l.count(); ++n)
                        f.compression_mechs += l.item(n).toElement().text();

                } else if (c.localName() == QLatin1String("bind") && c.namespaceURI() == NS_BIND) {
                    f.bind_supported = true;

                } else if (c.localName() == QLatin1String("hosts") && c.namespaceURI() == NS_HOSTS) {
                    QDomNodeList l = c.elementsByTagNameNS(NS_HOSTS, QLatin1String("host"));
                    for (int n = 0; n < l.count(); ++n)
                        f.hosts += l.item(n).toElement().text();
                    hosts += f.hosts;

                } else if (c.localName() == QLatin1String("sm") && c.namespaceURI() == NS_STREAM_MANAGEMENT) {
                    f.sm_supported = true;
                    // REVIEW: previously we checked for sasl_authed as well. why?

                } else if (c.localName() == QLatin1String("session") && c.namespaceURI() == NS_SESSION) {
                    f.session_supported = true;
                    f.session_required  = c.elementsByTagName(QLatin1String("optional")).count() == 0;
                    // more details https://tools.ietf.org/html/draft-cridland-xmpp-session-01

                } else {
                    unhandled.append(c);
                }
            }

            if (f.tls_supported) {
#ifdef XMPP_TEST
                QString s = "STARTTLS is available";
                if (f.tls_required)
                    s += " (required)";
                TD::msg(s);
#endif
            }
            if (f.sasl_supported) {
#ifdef XMPP_TEST
                QString s = "SASL mechs:";
                for (const auto &saslMech : std::as_const(f.sasl_mechs))
                    s += QString(" [%1]").arg(saslMech);
                TD::msg(s);
#endif
            }
            if (f.compress_supported) {
#ifdef XMPP_TEST
                QString s = "Compression mechs:";
                for (const auto &comprMech : std::as_const(f.compression_mechs))
                    s += QString(" [%1]").arg(comprMech);
                TD::msg(s);
#endif
            }

            event             = EFeatures;
            features          = f;
            unhandledFeatures = unhandled;
            step              = HandleFeatures;
            return true;
        } else {
            // ignore
        }
    } else if (step == GetTLSProceed) {
        // waiting for proceed to starttls
        if (e.namespaceURI() == NS_TLS) {
            if (e.tagName() == "proceed") {
#ifdef XMPP_TEST
                TD::msg("Server wants us to proceed with ssl handshake");
#endif
                tls_started = true;
                need        = NStartTLS;
                spare       = resetStream();
                step        = Start;
                return false;
            } else if (e.tagName() == "failure") {
                event     = EError;
                errorCode = ErrStartTLS;
                return true;
            } else {
                event     = EError;
                errorCode = ErrProtocol;
                return true;
            }
        } else {
            // ignore
        }
    } else if (step == GetCompressProceed) {
        // waiting for proceed to compression
        if (e.namespaceURI() == NS_COMPRESS_PROTOCOL) {
            if (e.tagName() == "compressed") {
#ifdef XMPP_TEST
                TD::msg("Server wants us to proceed with compression");
#endif
                compress_started = true;
                need             = NCompress;
                spare            = resetStream();
                step             = Start;
                return false;
            } else if (e.tagName() == "failure") {
                event     = EError;
                errorCode = ErrCompress;
                return true;
            } else {
                event     = EError;
                errorCode = ErrProtocol;
                return true;
            }
        } else {
            // ignore
        }
    } else if (step == GetSASLChallenge) {
        // waiting for sasl challenge/success/fail
        if (e.namespaceURI() == NS_SASL) {
            if (e.tagName() == "challenge") {
                QByteArray a = QCA::Base64().stringToArray(e.text()).toByteArray();
#ifdef XMPP_TEST
                TD::msg(QString("SASL IN: [%1]").arg(printArray(a)));
#endif
                sasl_step = a;
                need      = NSASLNext;
                step      = GetSASLNext;
                return false;
            } else if (e.tagName() == "success") {
                QString str = e.text();
                // "additional data with success" ?
                if (!str.isEmpty()) {
                    QByteArray a = QCA::Base64().stringToArray(str).toByteArray();
                    sasl_step    = a;
                    sasl_authed  = true;
                    need         = NSASLNext;
                    step         = GetSASLNext;
                    return false;
                }

                sasl_authed = true;
                event       = ESASLSuccess;
                step        = HandleSASLSuccess;
                return true;
            } else if (e.tagName() == "failure") {
                QDomElement t = firstChildElement(e);
                if (t.isNull() || t.namespaceURI() != NS_SASL)
                    errCond = -1;
                else
                    errCond = stringToSASLCond(t.tagName());

                // handle text elements
                auto                  nodes = e.elementsByTagNameNS(NS_SASL, QLatin1String("text"));
                decltype(errLangText) lt;
                for (int i = 0; i < nodes.count(); i++) {
                    auto    e    = nodes.item(i).toElement();
                    QString lang = e.attributeNS(NS_SASL, "lang", "");
                    lt.insert(lang, e.text());
                }

                errLangText = lt;
                event       = EError;
                errorCode   = ErrAuth;
                return true;
            } else {
                event     = EError;
                errorCode = ErrProtocol;
                return true;
            }
        }
    } else if (step == GetBindResponse) {
        if (e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
            QString type(e.attribute("type"));
            QString id(e.attribute("id"));

            if (id == "bind_1" && (type == "result" || type == "error")) {
                if (type == "result") {
                    QDomElement b = e.elementsByTagNameNS(NS_BIND, "bind").item(0).toElement();
                    Jid         j;
                    if (!b.isNull()) {
                        QDomElement je = e.elementsByTagName("jid").item(0).toElement();
                        j              = je.text();
                    }
                    if (!j.isValid()) {
                        event     = EError;
                        errorCode = ErrProtocol;
                        return true;
                    }
                    jid_ = j;
                    return loginComplete();
                } else {
                    errCond = -1;

                    QDomElement err = e.elementsByTagNameNS(NS_CLIENT, "error").item(0).toElement();
                    if (!err.isNull()) {
                        // get error condition
                        QDomNodeList nl = err.childNodes();
                        QDomElement  t;
                        for (int n = 0; n < nl.count(); ++n) {
                            QDomNode i = nl.item(n);
                            if (i.isElement()) {
                                t = i.toElement();
                                break;
                            }
                        }
                        if (!t.isNull() && t.namespaceURI() == NS_STANZAS) {
                            QString cond = t.tagName();
                            if (cond == "not-allowed")
                                errCond = BindNotAllowed;
                            else if (cond == "conflict")
                                errCond = BindConflict;
                        }
                    }

                    event     = EError;
                    errorCode = ErrBind;
                    return true;
                }
            } else {
                // ignore
            }
        } else {
            // ignore
        }
    } else if (step == GetAuthGetResponse) {
        // waiting for an iq
        if (e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
            Jid     from(e.attribute("from"));
            QString type(e.attribute("type"));
            QString id(e.attribute("id"));

            bool okfrom = (from.isEmpty() || from.compare(Jid(to)));
            if (okfrom && id == "auth_1" && (type == "result" || type == "error")) {
                if (type == "result") {
                    QDomElement q = e.elementsByTagNameNS("jabber:iq:auth", "query").item(0).toElement();
                    if (q.isNull() || q.elementsByTagName("username").item(0).isNull()
                        || q.elementsByTagName("resource").item(0).isNull()) {
                        event     = EError;
                        errorCode = ErrProtocol;
                        return true;
                    }
                    bool plain_supported  = !q.elementsByTagName("password").item(0).isNull();
                    bool digest_supported = !q.elementsByTagName("digest").item(0).isNull();

                    if (!digest_supported && !plain_supported) {
                        event     = EError;
                        errorCode = ErrProtocol;
                        return true;
                    }

                    // plain text not allowed?
                    if (!digest_supported && !allowPlain) {
                        event     = EError;
                        errorCode = ErrPlain;
                        return true;
                    }

                    digest = digest_supported;
                    need   = NPassword;
                    step   = HandleAuthSet;
                    return false;
                } else {
                    errCond = getOldErrorCode(e);

                    event     = EError;
                    errorCode = ErrAuth;
                    return true;
                }
            } else {
                // ignore
            }
        } else {
            // ignore
        }
    } else if (step == GetAuthSetResponse) {
        // waiting for an iq
        if (e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
            Jid     from(e.attribute("from"));
            QString type(e.attribute("type"));
            QString id(e.attribute("id"));

            bool okfrom = (from.isEmpty() || from.compare(Jid(to)));
            if (okfrom && id == "auth_2" && (type == "result" || type == "error")) {
                if (type == "result") {
                    return loginComplete();
                } else {
                    errCond = getOldErrorCode(e);

                    event     = EError;
                    errorCode = ErrAuth;
                    return true;
                }
            } else {
                // ignore
            }
        } else {
            // ignore
        }
    }
    // server
    else if (step == GetRequest) {
        printf("get request: [%s], %s\n", e.namespaceURI().toLatin1().data(), e.tagName().toLatin1().data());
        if (e.namespaceURI() == NS_TLS && e.localName() == "starttls") {
            // TODO: don't let this be done twice

            QDomElement e = doc.createElementNS(NS_TLS, "proceed");
            send(e, true);
            event = ESend;
            step  = HandleTLS;
            return true;
        }
        if (e.namespaceURI() == NS_SASL) {
            if (e.localName() == "auth") {
                if (sasl_started) {
                    // TODO
                    printf("error\n");
                    return false;
                }

                sasl_started = true;
                sasl_mech    = e.attribute("mechanism");
                // TODO: if child text missing, don't pass it
                sasl_step = QCA::Base64().stringToArray(e.text()).toByteArray();
                need      = NSASLFirst;
                step      = GetSASLNext;
                return false;
            } else {
                // TODO
                printf("unknown sasl tag\n");
                return false;
            }
        }
        if (e.namespaceURI() == NS_CLIENT && e.tagName() == "iq") {
            QDomElement b = e.elementsByTagNameNS(NS_BIND, "bind").item(0).toElement();
            if (!b.isNull()) {
                QDomElement res      = b.elementsByTagName("resource").item(0).toElement();
                QString     resource = res.text();

                QDomElement r = doc.createElement("iq");
                r.setAttribute("type", "result");
                r.setAttribute("id", e.attribute("id"));
                QDomElement bind = doc.createElementNS(NS_BIND, "bind");
                QDomElement jid  = doc.createElement("jid");
                Jid         j    = QString(user + '@' + host + '/' + resource);
                jid.appendChild(doc.createTextNode(j.full()));
                bind.appendChild(jid);
                r.appendChild(bind);

                send(r);
                event = ESend;
                // TODO
                return true;
            } else {
                // TODO
            }
        }
    } else if (step == GetSASLResponse) {
        if (e.namespaceURI() == NS_SASL && e.localName() == "response") {
            sasl_step = QCA::Base64().stringToArray(e.text()).toByteArray();
            need      = NSASLNext;
            step      = GetSASLNext;
            return false;
        }
    } else if (step == GetSMResponse) {
#ifdef IRIS_SM_DEBUG
        qWarning() << "HandleSM: step";
#endif
        if (e.namespaceURI() == NS_STREAM_MANAGEMENT) {
            if (e.localName() == "enabled") {
#ifdef IRIS_SM_DEBUG
                qDebug() << "Stream Management: [INF] Enabled";
#endif
                QString rs = e.attribute("resume");
                QString id = (rs == "true" || rs == "1") ? e.attribute("id") : QString();
                sm.start(id);
                if (!id.isEmpty()) {
#ifdef IRIS_SM_DEBUG
                    qDebug() << "Stream Management: [INF] Resumption Supported";
#endif
                    QString location = e.attribute("location").trimmed();
                    if (!location.isEmpty()) {
                        int         port_off = 0;
                        QStringView sm_host;
                        int         sm_port       = 0;
                        auto        location_view = QStringView { location };
                        if (location.startsWith('[')) { // ipv6
                            port_off = location.indexOf(']');
                            if (port_off != -1) { // looks valid
                                sm_host = location_view.mid(1, port_off - 1);
                                if (location.length() > port_off + 2 && location.at(port_off + 1) == ':')
                                    sm_port = location_view.mid(port_off + 2).toUInt();
                            }
                        }
                        if (port_off == 0) {
                            port_off = location.indexOf(':');
                            if (port_off != -1) {
                                sm_host = location_view.left(port_off);
                                sm_port = location_view.mid(port_off + 1).toUInt();
                            } else {
                                sm_host = location_view.mid(0);
                            }
                        }
                        sm.setLocation(sm_host.toString(), sm_port);
                    }
                } // else resumption is not supported on this server
                needTimer(SM_TIMER_INTERVAL_SECS);
                event = EReady;
                step  = Done;
                return true;
            } else if (e.localName() == "resumed") {
                sm.resume(e.attribute("h").toUInt());
                while (true) {
                    QDomElement st = sm.getUnacknowledgedStanza();
                    if (st.isNull())
                        break;
                    send(st);
                }
                needTimer(SM_TIMER_INTERVAL_SECS);
                event = EReady;
                step  = Done;
                return true;
            } else if (e.localName() == "failed") {
                if (sm.state().isResumption()) { // tried to resume? ok, then try to just enable
                    sm.state().resumption_id.clear();
                    // step = HandleFeatures;
                    event = ESMResumeFailed;
                    return true;
                }
            }
        }
    }

    if (isReady()) {
        if (!e.isNull()) {
            if (isValidStanza(e)) {
                stanzaToRecv = e;
                event        = EStanzaReady;
                setIncomingAsExternal();
                return true;
            } else if (sm.isActive()) {
                return streamManagementHandleStanza(e);
            }
        }
        if (sm.isActive()) {
            if (sm.lastAckElapsed() >= SM_TIMER_INTERVAL_SECS) {
                if (needSMRequest())
                    event = ESend;
                else
                    event = ESMConnTimeout;
                return true;
            }
        }
    }

    need = NNotify;
    notify |= NRecv;
    return false;
}
