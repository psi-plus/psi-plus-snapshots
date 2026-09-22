// Internal ICE resource ownership. Not a negotiated BUNDLE or public transport API.
#ifndef JINGLE_ICE_CONNECTION_P_H
#define JINGLE_ICE_CONNECTION_P_H

#include "jingle.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QVector>
#include <QWeakPointer>

#include <memory>
#include <utility>

namespace XMPP {
class Dtls;
class Ice176;
class UdpPortReserver;
namespace Jingle {
    namespace RTP {
        class SecureRtpAssociation;
    }
    namespace SCTP {
        class Association;
    }
    namespace ICE {
        class RawConnection;

        struct Component {
            int                           componentIndex  = 0;
            bool                          initialized     = false;
            bool                          lowOverhead     = false;
            bool                          needDatachannel = false;
            Dtls                         *dtls            = nullptr;
            RTP::SecureRtpAssociation    *secureRtp       = nullptr;
            SCTP::Association            *sctp            = nullptr;
            QSharedPointer<RawConnection> rawConnection;
        };

        // These counters are deliberately independent. ICE restart, DTLS rekey
        // and membership changes invalidate different classes of callbacks.
        struct ConnectionGeneration {
            quint64 iceGeneration      = 0;
            quint64 dtlsEpoch          = 0;
            quint64 membershipRevision = 0;

            bool operator==(const ConnectionGeneration &other) const
            {
                return iceGeneration == other.iceGeneration && dtlsEpoch == other.dtlsEpoch
                    && membershipRevision == other.membershipRevision;
            }
            bool operator!=(const ConnectionGeneration &other) const { return !(*this == other); }
        };

        // No QObject parent: the last strong membership owns destruction. All access,
        // including releasing memberships, must happen on the connection's thread.
        class IceConnection : public QObject {
        public:
            class Runtime;

            QVector<Component>   components;
            UdpPortReserver     *portReserver = nullptr;
            Ice176              *ice          = nullptr;
            QByteArray           secureRtpAssociationId;
            ConnectionGeneration generation;
            std::unique_ptr<Runtime> runtime;

            IceConnection();
            ~IceConnection() override;
        };

        struct ConnectionAssociationState {
            quint64                       id = 0;
            QSharedPointer<IceConnection> connection;
            QSet<ContentKey>              members;
        };

        class ConnectionRegistry;
        class ConnectionGroupTransaction;

        // A logical content's strong share in one session-local network association.
        // Move-only so copying a convenience handle cannot accidentally extend the
        // association lifetime as if another content had joined it.
        class ConnectionMembership {
        public:
            ConnectionMembership()                                        = default;
            ConnectionMembership(const ConnectionMembership &)            = delete;
            ConnectionMembership &operator=(const ConnectionMembership &) = delete;
            ConnectionMembership(ConnectionMembership &&other) :
                state_(std::move(other.state_)), content_(std::move(other.content_))
            {
                other.content_ = {};
            }
            ConnectionMembership &operator=(ConnectionMembership &&other)
            {
                if (this != &other) {
                    reset();
                    state_         = std::move(other.state_);
                    content_       = std::move(other.content_);
                    other.content_ = {};
                }
                return *this;
            }
            ~ConnectionMembership() { reset(); }

            explicit             operator bool() const { return state_ && state_->connection; }
            IceConnection       *connection() const { return state_ ? state_->connection.data() : nullptr; }
            quint64              associationId() const { return state_ ? state_->id : 0; }
            const ContentKey    &content() const { return content_; }
            qsizetype            membershipCount() const { return state_ ? state_->members.size() : 0; }
            ConnectionGeneration generation() const
            {
                return state_ && state_->connection ? state_->connection->generation : ConnectionGeneration {};
            }
            void reset()
            {
                if (!state_)
                    return;
                auto state = std::move(state_);
                if (state->members.remove(content_) && state->connection)
                    ++state->connection->generation.membershipRevision;
                content_ = {};
            }

        private:
            friend class ConnectionRegistry;
            friend class ConnectionGroupTransaction;
            ConnectionMembership(QSharedPointer<ConnectionAssociationState> state, ContentKey content) :
                state_(std::move(state)), content_(std::move(content))
            {
            }

            QSharedPointer<ConnectionAssociationState> state_;
            ContentKey                                 content_;
        };

        // Intended to live in one ICE Pad, hence one Session. It deliberately holds
        // only weak association references: memberships, not the registry, own network
        // lifetime. Association ids therefore have no meaning across sessions.
        class ConnectionRegistry {
        public:
            ConnectionMembership create(const ContentKey &content)
            {
                if (containsContent(content))
                    return {};
                auto state        = QSharedPointer<ConnectionAssociationState>::create();
                state->id         = nextAssociationId_++;
                state->connection = QSharedPointer<IceConnection>::create();
                state->members.insert(content);
                ++state->connection->generation.membershipRevision;
                associations_.insert(state->id, state.toWeakRef());
                return ConnectionMembership(std::move(state), content);
            }

            ConnectionMembership attach(quint64 associationId, const ContentKey &content)
            {
                if (containsContent(content))
                    return {};
                auto state = associations_.value(associationId).toStrongRef();
                if (!state)
                    return {};
                state->members.insert(content);
                ++state->connection->generation.membershipRevision;
                return ConnectionMembership(std::move(state), content);
            }

            bool contains(quint64 associationId) const
            {
                return !associations_.value(associationId).toStrongRef().isNull();
            }

            qsizetype liveAssociationCount() const
            {
                qsizetype count = 0;
                for (auto it = associations_.cbegin(); it != associations_.cend(); ++it) {
                    if (!it.value().toStrongRef().isNull())
                        ++count;
                }
                return count;
            }

            void prune()
            {
                for (auto it = associations_.begin(); it != associations_.end();) {
                    if (it.value().toStrongRef().isNull())
                        it = associations_.erase(it);
                    else
                        ++it;
                }
            }

        private:
            friend class ConnectionGroupTransaction;

            bool containsContent(const ContentKey &content) const
            {
                for (auto it = associations_.cbegin(); it != associations_.cend(); ++it) {
                    auto state = it.value().toStrongRef();
                    if (state && state->members.contains(content))
                        return true;
                }
                return false;
            }

            QHash<quint64, QWeakPointer<ConnectionAssociationState>> associations_;
            quint64                                                  nextAssociationId_ = 1;
        };
    }
}
}
#endif
