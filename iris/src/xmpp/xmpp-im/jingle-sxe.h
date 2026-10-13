/*
 * jingle-sxe.h - Shared XML Editing (XEP-0284) over Jingle
 * Copyright (C) 2026  Sergey Ilinykh
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

#ifndef JINGLE_SXE_H
#define JINGLE_SXE_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/jingle-application.h>
#include <iris/xmpp-im/jingle-sxe-document.h>
#include <iris/xmpp-im/jingle-transport.h>

namespace XMPP { namespace Jingle { namespace SXE {
    extern IRIS_EXPORT const QString NS_TRANSPORT;      // urn:xmpp:jingle:transports:sxe
    extern IRIS_EXPORT const QString NS_SVG_WHITEBOARD; // urn:xmpp:jingle:apps:svg-whiteboard:0
    /// Proposed disco feature: SXE payload on reliable, ordered message-oriented Jingle connections
    /// (e.g. an ICE SCTP data channel), one <sxe/> element per message.
    extern IRIS_EXPORT const QString NS_CONNECTIONS; // urn:xmpp:sxe:0#connections

    /// The initial document of a whiteboard session started without a user document.
    IRIS_EXPORT QDomDocument emptyWhiteboardDocument();

    class TransportManager;

    /**
     * @brief End-to-end protection of SXE payload in messages, e.g. SxeOmemo (OMEMO 2 with SCE).
     *
     * When both sides support the protector's method and the session prefers end-to-end encryption,
     * the SXE transport carries encryption='method'; every <sxe/> then travels inside protect()'s
     * element next to an empty <sxe session='SID'/> marker that routes the message, and plain payload is
     * dropped. docs/xsf-proposals/xep-0284-sxe-encryption.md describes the format.
     */
    class IRIS_EXPORT PayloadProtector {
    public:
        /// The protected element (e.g. OMEMO <encrypted/>), or the inner <sxe/>; null on failure.
        using Done = std::function<void(const QDomElement &result)>;

        virtual ~PayloadProtector();
        virtual QString     method() const                    = 0; // e.g. urn:xmpp:omemo:2
        virtual bool        canProtect(const Jid &peer) const = 0;
        virtual QStringList discoFeatures() const             = 0;
        /// Protects @a sxe for @a peer; @a done gets the element to send next to the marker.
        virtual void protect(const Jid &peer, const QDomElement &sxe, const Done &done) = 0;
        /// Recovers the <sxe/> of a message from @a from that carries a protected element.
        virtual void unprotect(const Jid &from, const QDomElement &message, const Done &done) = 0;
    };

    /**
     * @brief The XEP-0284 SXE Jingle transport.
     *
     * Signaling carries the <host/> entities. Payload is exchanged as <sxe/>
     * elements in <message/> stanzas; a one-to-one session is hosted by the
     * creator of the content and payload always goes to the session peer.
     * The transport provides no Connection channels.
     */
    class IRIS_EXPORT Transport : public XMPP::Jingle::Transport {
        Q_OBJECT
    public:
        Transport(TransportManagerPad *pad, Origin creator);
        ~Transport() override;

        void                        prepare() override;
        void                        start() override;
        PrepareUpdateResult         prepareUpdate(const QDomElement &transportEl) override;
        bool                        commitPreparedUpdate(PreparedUpdatePtr update) override;
        bool                        update(const QDomElement &transportEl) override;
        bool                        hasUpdates() const override;
        OutgoingTransportInfoUpdate takeOutgoingUpdate(bool ensureTransportElement = false) override;
        bool                        isValid() const override;
        TransportFeatures           features() const override;
        int                         maxSupportedChannelsPerComponent(TransportFeatures features) const override;
        Connection::Ptr addChannel(TransportFeatures features, const QString &id, int componentIndex = -1) override;
        QList<Connection::Ptr> channels() const override;

        QList<Jid> hosts() const;
        /// The method protecting the payload end to end (PayloadProtector::method()), or empty.
        QString encryption() const;
        /// Sends an <sxe/> element. 'session' and 'id' are filled in by the caller.
        void sendSxe(const QDomElement &sxe);
        /// Internal: a message of the session arrived (IncomingSxeTask).
        void receive(const QDomElement &message, const QDomElement &sxe, const Jid &from);

    signals:
        void sxeReceived(const QDomElement &sxe, const XMPP::Jid &from);

    private:
        friend class TransportManager;
        class Private;
        std::unique_ptr<Private> d;
    };

    class IRIS_EXPORT TransportPad : public TransportManagerPad {
        Q_OBJECT
    public:
        TransportPad(TransportManager *manager, Session *session);
        ~TransportPad() override;
        QString                         ns() const override;
        Session                        *session() const override;
        XMPP::Jingle::TransportManager *manager() const override;

    private:
        TransportManager *_manager;
        Session          *_session;
    };

    class IRIS_EXPORT TransportManager : public XMPP::Jingle::TransportManager {
        Q_OBJECT
    public:
        explicit TransportManager(QObject *parent = nullptr);
        ~TransportManager() override;

        TransportFeatures features() const override;
        bool              canMakeConnection(TransportFeatures desiredFeatures, const QString &ns) override;
        void              setJingleManager(XMPP::Jingle::Manager *jm) override;
        QPointer<XMPP::Jingle::Transport> newTransport(TransportManagerPad *pad, Origin creator) override;

        /// The SXE transport manager registered with @a manager, if any.
        static TransportManager *of(XMPP::Jingle::Manager *manager);
        /// Not owned; remove it before it is destroyed.
        void                 setPayloadProtector(PayloadProtector *protector);
        PayloadProtector    *payloadProtector() const;
        TransportManagerPad *pad(Session *session) override;
        QStringList          ns() const override;
        QStringList          discoFeatures() const override;

    private:
        class Private;
        std::unique_ptr<Private> d;
    };

    class Manager;

    class IRIS_EXPORT Pad : public ApplicationManagerPad {
        Q_OBJECT
    public:
        Pad(Manager *manager, Session *session);
        ~Pad() override;
        QString                           ns() const override;
        Session                          *session() const override;
        XMPP::Jingle::ApplicationManager *manager() const override;
        QString                           generateContentName(Origin senders) override;

    private:
        Manager *_manager;
        Session *_session;
    };

    /**
     * @brief One shared XML document edited over XEP-0284.
     *
     * The application type is given by the description namespace of its
     * manager, e.g. NS_SVG_WHITEBOARD. After Jingle acceptance the content
     * creator hosts the document: the joiner sends <connect/>, accepts the
     * <state-offer/> and imports the state before editing. Local edits are
     * made through document() and sent by Document::flush().
     *
     * Payload goes over a Jingle connection with reliable, ordered messages when
     * both sides support it (NS_CONNECTIONS), otherwise in <message/> stanzas
     * (NS_TRANSPORT). A failed connection is replaced by the message transport
     * with transport-replace, and the joiner synchronizes again.
     */
    class IRIS_EXPORT Application : public XMPP::Jingle::Application {
        Q_OBJECT
    public:
        enum class SyncState {
            Offline,      // the session is not established yet or has ended
            Connecting,   // the joiner waits for the state offer
            Receiving,    // the joiner imports the state
            Synchronized, // editing is possible
        };
        Q_ENUM(SyncState)

        Application(Pad *pad, const QString &contentName, Origin creator, Origin senders);
        ~Application() override;

        /// The shared document. It is a child of the application; a consumer may reparent
        /// it to keep the content after the session. Null if the consumer deleted it.
        Document *document() const;
        /// Replaces the hosted document. Only meaningful for the hosting side before the peer joins.
        void      setInitialDocument(const QDomDocument &doc);
        bool      isHost() const;
        SyncState syncState() const;
        /// True if the participant with @a role may edit: Jingle senders of an SXE content are
        /// the participants allowed to send edits. 'none' freezes the document for everybody.
        bool mayEdit(Origin role) const;

        void                                      setState(State state) override;
        const std::optional<XMPP::Stanza::Error> &lastError() const override;
        Reason                                    lastReason() const override;
        bool                                      isTransportReplaceEnabled() const override;
        /// A whiteboard that may use connections shares an ICE association (BUNDLE) with other contents.
        bool allowsSharedTransport() const override;
        bool supportsContentModify() const override { return true; }
        void remove(Reason::Condition cond = Reason::Success, const QString &comment = QString()) override;

        SetDescError setRemoteOffer(const QDomElement &description) override;
        SetDescError setRemoteAnswer(const QDomElement &description) override;
        void         prepare() override;
        void         start() override;

    protected:
        QDomElement makeLocalOffer() override;
        QDomElement makeLocalAnswer() override;
        void        incomingRemove(const Reason &r) override;
        void        prepareTransport() override;

    signals:
        void syncStateChanged(XMPP::Jingle::SXE::Application::SyncState state);

    private:
        class Private;
        std::unique_ptr<Private> d;
    };

    /// Payload of an XEP-0353 proposal for an SXE application type. The minimal whiteboard
    /// profile's description has no children.
    struct Proposal { };

    class IRIS_EXPORT Manager : public XMPP::Jingle::ApplicationManager {
        Q_OBJECT
    public:
        /// @a descriptionNs is the Jingle application type edited over SXE.
        explicit Manager(const QString &descriptionNs = NS_SVG_WHITEBOARD, QObject *parent = nullptr);
        ~Manager() override;

        void                    setJingleManager(XMPP::Jingle::Manager *jm) override;
        Application            *startApplication(ApplicationManagerPad *pad, const QString &contentName, Origin creator,
                                                 Origin senders) override;
        ApplicationManagerPad  *pad(Session *session) override;
        void                    closeAll(const QString &ns = QString()) override;
        QStringList             ns() const override;
        QStringList             discoFeatures() const override;
        std::optional<std::any> parseProposal(const QDomElement &description) const override;
        QDomElement             serializeProposal(const std::any &data, QDomDocument *document) const override;

        QString descriptionNamespace() const;

    private:
        QString                descriptionNs_;
        XMPP::Jingle::Manager *jingleManager_ = nullptr;
    };

}}}

#endif // JINGLE_SXE_H
