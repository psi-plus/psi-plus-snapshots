/*
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

#ifndef XMPP_MESSAGE_H
#define XMPP_MESSAGE_H

#include "iris/xmpp_stanza.h"
#include "xmpp_address.h"
#include "xmpp_chatstate.h"
#include "xmpp_muc.h"
#include "xmpp_receipts.h"
#include "xmpp_reference.h"
#include "xmpp_rosterx.h"
#include "xmpp_url.h"

#include <QExplicitlySharedDataPointer>

class QDateTime;
class QString;

namespace XMPP {
class BoBData;
class HTMLElement;
class HttpAuthRequest;
class IBBData;
class Jid;
class PubSubItem;
class PubSubRetraction;
class XData;

typedef QMap<QString, QString> StringMap;

typedef enum { OfflineEvent, DeliveredEvent, DisplayedEvent, ComposingEvent, CancelEvent } MsgEvent;

class Message {
public:
    enum CarbonDir : quint8 {
        NoCarbon,
        Received, // other party messages are sent to another own client
        Sent      // own messages are sent from other clients
    };

    // XEP-0334
    enum ProcessingHint { NoPermanentStore = 1, NoStore = 2, NoCopy = 4, Store = 8 };
    Q_DECLARE_FLAGS(ProcessingHints, ProcessingHint)

    struct StanzaId {
        Jid     by;
        QString id;
    };

    Message();
    Message(const Jid &to);
    Message(const Message &from);
    Message &operator=(const Message &from);
    ~Message();
    bool        operator==(const Message &from) const;
    inline bool isNull() const { return d == nullptr; }
    bool        isPureSubject() const;

    Jid           to() const;
    Jid           from() const;
    QString       id() const;
    QString       type() const;
    QString       lang() const;
    QString       subject(const QString &lang = QString()) const;
    QString       subject(const QLocale &lang) const;
    StringMap     subjectMap() const;
    QString       body(const QString &lang = "") const;
    QString       body(const QLocale &lang) const;
    QString       thread() const;
    Stanza::Error error() const;

    void setTo(const Jid &j);
    void setFrom(const Jid &j);
    void setId(const QString &s);
    void setType(const QString &s);
    void setLang(const QString &s);
    void setSubject(const QString &s, const QString &lang = "");
    void setBody(const QString &s, const QString &lang = "");
    void setThread(const QString &s, bool send = false);
    void setError(const Stanza::Error &err);

    // XEP-0060
    QString                 pubsubNode() const;
    QList<PubSubItem>       pubsubItems() const;
    QList<PubSubRetraction> pubsubRetractions() const;

    // XEP-0091
    QDateTime timeStamp() const;
    void      setTimeStamp(const QDateTime &ts, bool send = false);

    // XEP-0071
    HTMLElement html(const QString &lang = "") const;
    void        setHTML(const HTMLElement &s, const QString &lang = "");
    bool        containsHTML() const;

    // XEP-0066
    UrlList urlList() const;
    void    urlAdd(const Url &u);
    void    urlsClear();
    void    setUrlList(const UrlList &list);

    // XEP-0022
    QString eventId() const;
    void    setEventId(const QString &id);
    bool    containsEvents() const;
    bool    containsEvent(MsgEvent e) const;
    void    addEvent(MsgEvent e);

    // XEP-0085
    ChatState chatState() const;
    void      setChatState(ChatState);

    // XEP-0184
    MessageReceipt messageReceipt() const;
    void           setMessageReceipt(MessageReceipt);
    QString        messageReceiptId() const;
    void           setMessageReceiptId(const QString &s);

    // XEP-0027
    QString xsigned() const;
    void    setXSigned(const QString &s);
    QString xencrypted() const;
    void    setXEncrypted(const QString &s);

    // XEP-0033
    AddressList addresses() const;
    AddressList findAddresses(Address::Type t) const;
    void        addAddress(const Address &a);
    void        clearAddresses();
    void        setAddresses(const AddressList &list);

    // XEP-144
    RosterExchangeItems rosterExchangeItems() const;
    void                setRosterExchangeItems(const RosterExchangeItems &);

    // XEP-172
    void    setNick(const QString &);
    QString nick() const;

    // XEP-0070
    void            setHttpAuthRequest(const HttpAuthRequest &);
    HttpAuthRequest httpAuthRequest() const;

    // XEP-0004
    void  setForm(const XData &);
    XData getForm() const;

    // XEP-xxxx SXE
    void        setSxe(const QDomElement &);
    QDomElement sxe() const;

    // XEP-0231 bits of binary
    void           addBoBData(const BoBData &);
    QList<BoBData> bobDataList() const;

    // XEP-0047 ibb
    IBBData ibbData() const;

    // XEP-0280 Message Carbons
    void      setDisabledCarbons(bool disabled);
    bool      isDisabledCarbons() const;
    void      setCarbonDirection(CarbonDir);
    CarbonDir carbonDirection() const;

    // XEP-0297
    void setForwardedFrom(const Jid &jid);
    Jid  forwardedFrom() const;

    // XEP-0308
    QString replaceId() const;
    void    setReplaceId(const QString &id);

    // XEP-0334
    void            setProcessingHints(const ProcessingHints &hints);
    ProcessingHints processingHints() const;

    // MUC
    void             addMUCStatus(int);
    QList<int>       getMUCStatuses() const;
    void             addMUCInvite(const MUCInvite &);
    QList<MUCInvite> mucInvites() const;
    void             setMUCDecline(const MUCDecline &);
    MUCDecline       mucDecline() const;
    QString          mucPassword() const;
    void             setMUCPassword(const QString &);
    bool             hasMUCUser() const;

    // XEP-0359
    StanzaId stanzaId() const;
    void     setStanzaId(const StanzaId &id);
    QString  originId() const;
    void     setOriginId(const QString &id);

    // XEP-0380
    QString encryptionProtocol() const;
    void    setEncryptionProtocol(const QString &protocol);

    // XEP-0385 and XEP-0372
    QList<Reference> references() const;
    void             addReference(const Reference &r);
    void             setReferences(const QList<Reference> &r);

    // Obsolete invitation
    QString invite() const;
    void    setInvite(const QString &s);

    // for compatibility.  delete me later
    bool spooled() const;
    void setSpooled(bool);
    bool wasEncrypted() const;
    void setWasEncrypted(bool);

    Stanza toStanza(Stream *stream) const;
    bool   fromStanza(const Stanza &s);
    bool   fromStanza(const Stanza &s, int tzoffset);
    bool   fromStanza(const Stanza &s, bool useTimeZoneOffset, int timeZoneOffset);

private:
    class Private;
    QExplicitlySharedDataPointer<Private> d;
};
} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::Message::ProcessingHints)

#endif // XMPP_MESSAGE_H
