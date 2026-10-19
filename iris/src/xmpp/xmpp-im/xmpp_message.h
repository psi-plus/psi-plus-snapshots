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

#include <iris/xmpp-im/xmpp_address.h>
#include <iris/xmpp-im/xmpp_chatstate.h>
#include <iris/xmpp-im/xmpp_file-sharing.h>
#include <iris/xmpp-im/xmpp_jinglemessage.h>
#include <iris/xmpp-im/xmpp_muc.h>
#include <iris/xmpp-im/xmpp_receipts.h>
#include <iris/xmpp-im/xmpp_reference.h>
#include <iris/xmpp-im/xmpp_rosterx.h>
#include <iris/xmpp-im/xmpp_url.h>

#include <QDomElement>
#include <QExplicitlySharedDataPointer>
#include <QList>
#include <QSet>

class QDateTime;
class QString;

namespace XMPP {
class BoBData;
class Forwarding;
class HTMLElement;
class HttpAuthRequest;
class IBBData;
class Jid;
class JT_Message;
class JT_PushMessage;
class PubSubEvent;
class PubSubItem;
class PubSubRetraction;
class XData;
namespace Jingle {
    class Manager;
}

typedef QMap<QString, QString> StringMap;

typedef enum { OfflineEvent, DeliveredEvent, DisplayedEvent, ComposingEvent, CancelEvent } MsgEvent;

class Message {
public:
    enum class Type { Chat, Error, Groupchat, Headline, Normal };

    // XEP-0334
    enum ProcessingHint { NoPermanentStore = 1, NoStore = 2, NoCopy = 4, Store = 8 };
    Q_DECLARE_FLAGS(ProcessingHints, ProcessingHint)

    struct StanzaId {
        Jid     by;
        QString id;

        bool isValid() const { return by.isBare() && !id.isEmpty(); }
    };

    // XEP-0461. The 'to' JID is optional; the referenced message ID is not.
    struct Reply {
        Jid     to;
        QString id;
        bool    isValid() const { return !id.isEmpty() && (to.isEmpty() || to.isValid()); }
    };

    // XEP-0428. Ranges count Unicode code points (XEP-0426) and 'end' is
    // exclusive. A range without start/end covers the whole element.
    struct FallbackRange {
        int  start = -1;
        int  end   = -1;
        bool isWhole() const { return start < 0 && end < 0; }
        // UTF-16 [from, to) for this range in text; false when out of bounds.
        bool toUtf16(const QString &text, int &from, int &to) const;
    };
    struct Fallback {
        QString              for_; // namespace of the replaced specification; may be empty
        QList<FallbackRange> body;
        QList<FallbackRange> subject;
    };

    struct Reactions {
        QString       targetId;
        QSet<QString> reactions;
    };

    // XEP-0425: received moderation metadata (not proof of room support).
    // For Tombstone, the original message UID is supplied by the MAM result.
    struct ModeratedRetraction {
        enum class Kind { None, Action, Tombstone };
        Kind    kind = Kind::None;
        Jid     by;
        QString moderatorOccupantId;
        QString reason;
        QString stamp; // XML datetime, only set for a validated tombstone stamp
    };

    // XEP-0313 result envelope carried by an ordinary message stanza.
    // Keep its original XML for callers that consume the legacy MAMTask::archive().
    struct MamResult {
        QString     queryId;
        QString     id;
        QDomElement xml;
        bool        isValid() const { return !xml.isNull(); }
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
    Type          type() const;
    QString       typeStr() const;
    QString       lang() const;
    QString       subject(const QString &lang = {}) const;
    QString       subject(const QLocale &lang) const;
    StringMap     subjectMap() const;
    QString       body(const QString &lang = "") const;
    QString       body(const QLocale &lang) const;
    QString       thread() const;
    Stanza::Error error() const;

    void setTo(const Jid &j);
    void setFrom(const Jid &j);
    void setId(const QString &s);
    void setType(Type type);
    void setLang(const QString &s);
    void setSubject(const QString &s, const QString &lang = {});
    void setBody(const QString &s, const QString &lang = {});
    void setThread(const QString &s, bool send = false);
    void setError(const Stanza::Error &err);

    // XEP-0060
    const QList<PubSubEvent>                                            &pubSubEvents() const;
    [[deprecated("Use pubSubEvents() instead")]] QString                 pubsubNode() const;
    [[deprecated("Use pubSubEvents() instead")]] QList<PubSubItem>       pubsubItems() const;
    [[deprecated("Use pubSubEvents() instead")]] QList<PubSubRetraction> pubsubRetractions() const;

    // XEP-0091
    QDateTime timeStamp() const;
    void      setTimeStamp(const QDateTime &ts, bool send = false);

    // XEP-0071
    HTMLElement html(const QString &lang = {}) const;
    void        setHTML(const HTMLElement &s, const QString &lang = {});
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

    // XEP-0231 bits of binary
    void           addBoBData(const BoBData &);
    QList<BoBData> bobDataList() const;

    // XEP-0047 ibb
    IBBData ibbData() const;

    // XEP-0280 Message Carbons
    Jid     displayJid() const;
    Message displayMessage() const;
    void    setCarbonsPrivate(bool enable);
    bool    carbonsPrivate() const;

    // XEP-0297
    void setForwarded(const Forwarding &frw);
    // note, the next method has to be called only on not-null message
    const Forwarding &forwarded() const;

    // XEP-0313: returned metadata does not imply that this message is
    // addressed to a chat; routing belongs to message subscribers.
    MamResult mamResult() const;

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

    // XEP-0353 Jingle Message Initiation
    Jingle::MessageInitiation jingleMessageInitiation() const;
    void                      setJingleMessageInitiation(const Jingle::MessageInitiation &initiation);

    // XEP-0421: raw/untrusted MUC identifier. Consumers MUST verify the
    // room advertises urn:xmpp:occupant-id:0 before using it as an identity.
    QString occupantId() const;
    void    setOccupantId(const QString &id);

    // XEP-0359. Consumers must verify the assigning entity via disco before
    // trusting stanza IDs for deduplication or MAM catch-up.
    StanzaId        stanzaId(const Jid &by) const;
    QList<StanzaId> stanzaIds() const;
    void            setStanzaIds(const QList<StanzaId> &ids);
    void            addStanzaId(const StanzaId &id);
    Reply           reply() const;
    void            setReply(const Reply &reply);
    // XEP-0428
    QList<Fallback> fallbacks() const;
    void            setFallbacks(const QList<Fallback> &fallbacks);
    // Body ranges marked as fallback for the given specification namespace.
    QList<FallbackRange> bodyFallbacks(const QString &forNamespace) const;
    QString              originId() const;
    void                 setOriginId(const QString &id);

    // XEP-0380
    QString encryptionProtocol() const;
    void    setEncryptionProtocol(const QString &protocol);

    // XEP-0385 and XEP-0372
    QList<Reference> references() const;
    void             addReference(const Reference &r);
    void             setReferences(const QList<Reference> &r);

    // XEP-0447 Stateless File Sharing
    QList<StatelessFileSharing::FileSharing> fileSharings() const;
    void                                     addFileSharing(const StatelessFileSharing::FileSharing &sharing);
    void                                     setFileSharings(const QList<StatelessFileSharing::FileSharing> &sharings);
    QList<StatelessFileSharing::Sources>     attachedFileSources() const;
    void                                     addAttachedFileSources(const StatelessFileSharing::Sources &sources);
    void setAttachedFileSources(const QList<StatelessFileSharing::Sources> &sources);

    // XEP-0367 Message Attaching (used by XEP-0447 source attachment)
    QString attachToId() const;
    void    setAttachToId(const QString &id);

    // XEP-0358 Published Jingle Sessions sent directly in a message
    QList<Jingle::JinglePub> jinglePublications() const;
    void                     addJinglePublication(const Jingle::JinglePub &publication);
    void                     setJinglePublications(const QList<Jingle::JinglePub> &publications);

    // XEP-0444 message reaction
    void      setReactions(const Reactions &reactions);
    Reactions reactions() const;

    void                setRetraction(const QString &retractedMessageId);
    QString             retraction() const;
    ModeratedRetraction moderatedRetraction() const;

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
    friend class Forwarding;
    friend class JT_Message;
    friend class JT_PushMessage;

    Stanza toStanza(Stream *stream, Jingle::Manager *jingleManager) const;
    bool   fromStanza(const Stanza &s, bool useTimeZoneOffset, int timeZoneOffset, Jingle::Manager *jingleManager);

    class Private;
    QExplicitlySharedDataPointer<Private> d;
};
} // namespace XMPP

Q_DECLARE_OPERATORS_FOR_FLAGS(XMPP::Message::ProcessingHints)

#endif // XMPP_MESSAGE_H
