/*
 * psiencryptioncontroller.cpp - application-level encryption registry for Psi
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "psiencryptioncontroller.h"

#include "applicationinfo.h"

#include "iconset.h"
#include "iris/jingle-session.h"
#include "iris/xmpp_caps.h"
#include "iris/xmpp_client.h"
#include "psiaccount.h"
#include "psioptions.h"
#include "userlist.h"

#ifdef IRIS_ENABLE_OMEMO
#include "iris/xmpp_omemo.h"
#include "psiomemostorage.h"
#endif

#include <QAbstractItemView>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLoggingCategory>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <utility>

namespace {
constexpr auto OpenPgpMethodId = "openpgp";
#ifdef IRIS_ENABLE_OMEMO
Q_LOGGING_CATEGORY(omemoTrustLog, "psi.omemo.trust", QtWarningMsg)
#endif

XMPP::EncryptionJob::Error toIrisError(EncryptionMethodProvider::Error error)
{
    using PluginError = EncryptionMethodProvider::Error;
    using IrisError   = XMPP::EncryptionJob::Error;
    switch (error) {
    case PluginError::None:
        return IrisError::None;
    case PluginError::Unsupported:
        return IrisError::Unsupported;
    case PluginError::InvalidInput:
        return IrisError::InvalidInput;
    case PluginError::NoRecipients:
        return IrisError::NoRecipients;
    case PluginError::NoSession:
        return IrisError::NoSession;
    case PluginError::UntrustedIdentity:
        return IrisError::UntrustedIdentity;
    case PluginError::CryptoError:
        return IrisError::CryptoError;
    case PluginError::AuthenticationFailed:
        return IrisError::AuthenticationFailed;
    case PluginError::ProtocolError:
        return IrisError::ProtocolError;
    case PluginError::StorageError:
        return IrisError::StorageError;
    case PluginError::NetworkError:
        return IrisError::NetworkError;
    case PluginError::Cancelled:
        return IrisError::Cancelled;
    }
    return IrisError::ProtocolError;
}

EncryptionMethodProvider::Metadata toPluginMetadata(const XMPP::EncryptionMetadata &metadata)
{
    EncryptionMethodProvider::Metadata result;
    result.sender         = metadata.sender.full();
    result.senderDeviceId = metadata.senderDeviceId;
    result.senderKey      = metadata.senderKey;
    result.protocolOnly   = metadata.protocolOnly;
    result.details        = metadata.details;
    return result;
}

XMPP::EncryptionMetadata toIrisMetadata(const QString &methodId, const EncryptionMethodProvider::Metadata &metadata)
{
    XMPP::EncryptionMetadata result;
    result.methodId       = methodId;
    result.sender         = XMPP::Jid(metadata.sender);
    result.senderDeviceId = metadata.senderDeviceId;
    result.senderKey      = metadata.senderKey;
    result.protocolOnly   = metadata.protocolOnly;
    result.details        = metadata.details;
    return result;
}

QString decryptionRecoveryKey(const QString &methodId, const XMPP::EncryptionMetadata &metadata)
{
    return methodId + QLatin1Char('\n') + metadata.sender.bare() + QLatin1Char('\n')
        + QString::number(metadata.senderDeviceId) + QLatin1Char('\n')
        + metadata.details.value(QStringLiteral("omemoProtocol")).toString();
}

EncryptionMethodProvider::Context toPluginContext(const XMPP::EncryptionContext &context)
{
    EncryptionMethodProvider::Context result;
    result.options = context.options;
    result.recipients.reserve(context.recipients.size());
    for (const auto &recipient : context.recipients)
        result.recipients.append(recipient.full());
    if (context.replyTo) {
        result.hasReplyTo = true;
        result.replyTo    = toPluginMetadata(*context.replyTo);
    }
    return result;
}

XMPP::EncryptionMethod::Capabilities toIrisCapabilities(EncryptionMethodProvider::Capabilities capabilities)
{
    XMPP::EncryptionMethod::Capabilities result;
    if (capabilities.testFlag(EncryptionMethodProvider::XmppStanza))
        result |= XMPP::EncryptionMethod::XmppStanza;
    if (capabilities.testFlag(EncryptionMethodProvider::DataMessage))
        result |= XMPP::EncryptionMethod::DataMessage;
    if (capabilities.testFlag(EncryptionMethodProvider::DataStream))
        result |= XMPP::EncryptionMethod::DataStream;
    if (capabilities.testFlag(EncryptionMethodProvider::Groupchat))
        result |= XMPP::EncryptionMethod::Groupchat;
    return result;
}

#ifdef IRIS_ENABLE_OMEMO
using PsiOmemo::needsManualTrustDecision;

QString formatFingerprint(const QByteArray &fingerprint)
{
    const auto hex = fingerprint.toHex();
    QString    out;
    out.reserve(hex.size() + hex.size() / 8);
    for (qsizetype i = 0; i < hex.size(); i += 8) {
        if (!out.isEmpty())
            out += QLatin1Char(' ');
        out += QString::fromLatin1(hex.mid(i, 8));
    }
    return out;
}

QString omemoProtocolText(XMPP::OmemoProtocols protocols)
{
    QStringList profiles;
    if (protocols.testFlag(XMPP::OmemoProtocol::Omemo2))
        profiles.append(QStringLiteral("OMEMO 2"));
    if (protocols.testFlag(XMPP::OmemoProtocol::Legacy))
        profiles.append(QObject::tr("Legacy OMEMO"));
    return profiles.isEmpty() ? QObject::tr("Unknown") : profiles.join(QStringLiteral(", "));
}
#endif
} // namespace

#ifdef IRIS_ENABLE_OMEMO
// One modeless review per account, shared by incoming and outgoing operations.
// A row represents the exact identity, not merely a numeric device id.
class PsiEncryptionController::TrustReviewDialog final : public QDialog {
public:
    using Device = XMPP::OmemoDeviceInfo;
    using Purpose = PsiOmemo::DeviceDecision;
    using Collector = std::function<QList<Device>()>;
    struct Request {
        Collector collect;
        Purpose purpose;
        std::function<void(bool)> completion;
    };
    struct Row {
        Device device;
        bool repair = false;
        QTreeWidgetItem *item = nullptr;
    };
    enum class Action { None, Approve, Distrust };

    explicit TrustReviewDialog(PsiEncryptionController *controller)
        : controller_(controller)
    {
        setWindowTitle(tr("Review OMEMO devices — %1").arg(controller_->client()->jid().bare()));
        setModal(false);
        auto *layout = new QVBoxLayout(this);
        auto *intro = new QLabel(tr("Review new identity keys and session repair requests. "
                                   "Newly discovered devices are added to this list while it is open."), this);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        tree_ = new QTreeWidget(this);
        tree_->setColumnCount(5);
        tree_->setHeaderLabels({ tr("Device"), tr("Address"), tr("Profile"), tr("Fingerprint"), tr("Action") });
        tree_->setRootIsDecorated(false);
        tree_->setSelectionMode(QAbstractItemView::NoSelection);
        for (int column : { 0, 1, 2, 4 })
            tree_->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
        layout->addWidget(tree_);
        auto *hint = new QLabel(tr("Trust / approve selected verifies the checked identities and authorizes "
                                  "session repair only for rows marked for repair. Repair cannot recover "
                                  "the missed message. Not now leaves unresolved identities unchanged."), this);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        auto *buttons = new QDialogButtonBox(this);
        approve_ = buttons->addButton(tr("Trust / approve selected"), QDialogButtonBox::AcceptRole);
        distrust_ = buttons->addButton(tr("Distrust selected"), QDialogButtonBox::DestructiveRole);
        auto *later = buttons->addButton(tr("Not now"), QDialogButtonBox::RejectRole);
        connect(approve_, &QPushButton::clicked, this, [this] { action_ = Action::Approve; accept(); });
        connect(distrust_, &QPushButton::clicked, this, [this] { action_ = Action::Distrust; accept(); });
        connect(later, &QPushButton::clicked, this, &QDialog::reject);
        layout->addWidget(buttons);
        resize(900, 360);
        auto *omemo = controller_->omemoEncryption();
        connect(omemo, &XMPP::OmemoEncryption::deviceChanged, this, [this] { refresh(); });
        connect(omemo, &XMPP::OmemoEncryption::deviceRemoved, this, [this] { refresh(); });
        connect(omemo, &XMPP::OmemoEncryption::trustChanged, this, [this] { refresh(); });
    }

    void add(Request request)
    {
        requests_.append(std::move(request));
        refresh();
    }

    void refresh()
    {
        if (resolving_)
            return;
        QList<Row> desired;
        for (const auto &request : std::as_const(requests_)) {
            for (const auto &device : request.collect()) {
                auto found = std::find_if(desired.begin(), desired.end(), [&device](const Row &row) {
                    return sameIdentity(row.device, device);
                });
                const bool repair = request.purpose == Purpose::RepairSession;
                if (found == desired.end())
                    desired.append(Row { device, repair, nullptr });
                else {
                    found->repair |= repair;
                    found->device.protocols |= device.protocols;
                    if (found->device.label.isEmpty())
                        found->device.label = device.label;
                }
            }
        }

        // Never rebuild the tree: keep existing row objects, their check
        // states, and the scroll position while more bundles arrive.
        for (int i = rows_.size() - 1; i >= 0; --i) {
            if (std::none_of(desired.cbegin(), desired.cend(), [this, i](const Row &row) {
                    return sameIdentity(rows_.at(i).device, row.device);
                })) {
                delete rows_.at(i).item;
                rows_.removeAt(i);
            }
        }
        for (auto &row : desired) {
            auto existing = std::find_if(rows_.begin(), rows_.end(), [&row](const Row &old) {
                return sameIdentity(old.device, row.device);
            });
            if (existing == rows_.end()) {
                row.item = new QTreeWidgetItem(tree_);
                row.item->setFlags(row.item->flags() | Qt::ItemIsUserCheckable);
                row.item->setCheckState(0, Qt::Checked);
                rows_.append(row);
                updateRow(rows_.last());
            } else {
                row.item = existing->item;
                // A newly requested repair is a distinct operation; do not
                // inherit a checked box from a prior verification-only row.
                if (row.repair && !existing->repair)
                    row.item->setCheckState(0, Qt::Unchecked);
                *existing = row;
                updateRow(*existing);
            }
        }
        approve_->setEnabled(!rows_.isEmpty());
        distrust_->setEnabled(!rows_.isEmpty());
    }

    void resolve()
    {
        resolving_ = true;
        auto *omemo = controller_->omemoEncryption();
        const auto requests = std::exchange(requests_, {});
        QList<Row> selected;
        for (const auto &row : std::as_const(rows_)) {
            if (row.item->checkState(0) == Qt::Checked)
                selected.append(row);
        }
        // Capture the repair targets before trust changes alter the candidate
        // sets. Every completion is evaluated against its own request.
        QList<QList<Device>> repairTargets;
        for (const auto &request : requests)
            repairTargets.append(request.purpose == Purpose::RepairSession ? request.collect() : QList<Device>());
        if (action_ != Action::None) {
            for (const auto &row : selected) {
                const auto &device = row.device;
                const auto current = omemo->devices(device.owner);
                const bool stillActive = std::any_of(current.cbegin(), current.cend(), [&device](const Device &candidate) {
                    return candidate.active && sameIdentity(candidate, device);
                });
                if (!stillActive)
                    continue;
                if (action_ == Action::Approve && !needsManualTrustDecision(omemo->trustLevel(device.owner, device.identityKey)))
                    continue;
                const auto level = action_ == Action::Approve ? XMPP::EncryptionTrustLevel::ManuallyTrusted
                                                              : XMPP::EncryptionTrustLevel::Distrusted;
                if (!omemo->setTrustLevel(device.owner, device.identityKey, level))
                    controller_->reportError(device.owner, tr("Could not save the OMEMO trust decision."));
            }
        }

        // Compute the outcomes before invoking callbacks: those can start new
        // encryption operations and open a subsequent review.
        QList<bool> outcomes;
        for (int i = 0; i < requests.size(); ++i) {
            const auto &request = requests.at(i);
            bool retry = action_ != Action::None && request.collect().isEmpty();
            if (request.purpose == Purpose::RepairSession) {
                const auto accepted = omemo->acceptedSessionBuildingTrustLevels();
                const auto &targets = repairTargets.at(i);
                retry = action_ == Action::Approve && !targets.isEmpty()
                    && std::all_of(targets.cbegin(), targets.cend(), [&](const Device &device) {
                        return accepted.testFlag(omemo->trustLevel(device.owner, device.identityKey))
                            && std::any_of(selected.cbegin(), selected.cend(), [&device](const Row &row) {
                                return row.repair && sameIdentity(row.device, device);
                            });
                    });
            }
            outcomes.append(retry);
        }
        const QPointer<PsiEncryptionController> guard(controller_);
        for (int i = 0; i < requests.size() && guard; ++i)
            if (requests.at(i).completion)
                requests.at(i).completion(outcomes.at(i));
    }

private:
    static bool sameIdentity(const Device &a, const Device &b)
    {
        return a.owner.bare() == b.owner.bare() && a.id == b.id && a.identityKey == b.identityKey;
    }

    void updateRow(const Row &row)
    {
        const auto &device = row.device;
        row.item->setText(0, device.label.isEmpty() ? tr("Unnamed device (%1)").arg(device.id)
                                                    : tr("%1 (%2)").arg(device.label).arg(device.id));
        const auto address = device.owner.bare();
        row.item->setText(1, address == controller_->client()->jid().bare()
                               ? tr("Your account (%1)").arg(address) : address);
        row.item->setText(2, omemoProtocolText(device.protocols));
        row.item->setText(3, formatFingerprint(device.identityKey));
        row.item->setText(4, row.repair ? (needsManualTrustDecision(device.trust) ? tr("Trust and repair")
                                                                               : tr("Repair session"))
                                        : tr("Verify identity"));
        row.item->setToolTip(0, device.label.isEmpty() ? tr("This device did not publish a verifiable name.")
            : tr("The device name was verified against this OMEMO identity key."));
    }

    PsiEncryptionController *controller_;
    QTreeWidget *tree_ = nullptr;
    QPushButton *approve_ = nullptr;
    QPushButton *distrust_ = nullptr;
    QList<Request> requests_;
    QList<Row> rows_;
    Action action_ = Action::None;
    bool resolving_ = false;
};
#endif

class PsiEncryptionController::PluginMethodAdapter final : public XMPP::EncryptionMethod {
public:
    class Session final : public XMPP::EncryptedSession {
    public:
        Session(PluginMethodAdapter *method, const XMPP::EncryptionContext &context,
                EncryptionMethodProvider::Session *pluginSession, QObject *parent = nullptr) :
            XMPP::EncryptedSession(method ? method->id() : QString(), context, parent), method_(method),
            pluginSession_(pluginSession)
        {
            if (method)
                method->sessions_.insert(this);
        }

        ~Session() override
        {
            detachProvider();
            if (method_)
                method_->sessions_.remove(this);
        }

        XMPP::EncryptionJob *doEncrypt(const QDomElement &xml, const XMPP::EncryptionContext &context) override
        {
            return runStanza(true, xml, context);
        }

        XMPP::EncryptionJob *doDecrypt(const QDomElement &xml, const XMPP::EncryptionContext &context) override
        {
            return runStanza(false, xml, context);
        }

        XMPP::EncryptionJob *doEncrypt(const QByteArray &data) override { return runData(true, data); }

        XMPP::EncryptionJob *doDecrypt(const QByteArray &data) override { return runData(false, data); }

        XMPP::EncryptionJob *doEncryptChunk(const QByteArray &chunk, bool final) override
        {
            return runData(true, chunk, true, final);
        }

        XMPP::EncryptionJob *doDecryptChunk(const QByteArray &chunk, bool final) override
        {
            return runData(false, chunk, true, final);
        }

    private:
        void detachProvider()
        {
            const auto operations = activeOperations_;
            activeOperations_.clear();
            for (auto it = operations.cbegin(); it != operations.cend(); ++it) {
                delete it.value(); // asks the plugin to cancel queued/asynchronous work
                if (it.key() && !it.key()->isFinished())
                    it.key()->fail(XMPP::EncryptionJob::Error::Cancelled,
                                   QStringLiteral("Encryption plugin was unloaded"));
            }
            pluginSession_.reset();
        }

        XMPP::EncryptionJob *runStanza(bool encrypting, const QDomElement &xml, const XMPP::EncryptionContext &context)
        {
            auto *job = new XMPP::EncryptionJob(this);
            if (!method_ || !pluginSession_) {
                job->fail(XMPP::EncryptionJob::Error::Cancelled,
                          QStringLiteral("Encryption plugin is no longer available"));
                return job;
            }

            auto                         *guard = new QObject(job);
            QPointer<XMPP::EncryptionJob> weakJob(job);
            activeOperations_.insert(job, guard);
            connect(job, &XMPP::EncryptionJob::finished, this, [this, job]() { activeOperations_.remove(job); });
            connect(job, &QObject::destroyed, this, [this, job]() { activeOperations_.remove(job); });
            auto completion = [weakJob, methodId = methodId()](EncryptionMethodProvider::Result result) {
                if (!weakJob || weakJob->isFinished())
                    return;
                if (!result.success) {
                    weakJob->fail(toIrisError(result.error), result.errorString);
                    return;
                }
                weakJob->complete(result.stanza, toIrisMetadata(methodId, result.metadata));
            };

            if (encrypting)
                pluginSession_->encryptStanza(xml, toPluginContext(context), guard, std::move(completion));
            else
                pluginSession_->decryptStanza(xml, toPluginContext(context), guard, std::move(completion));
            return job;
        }

        XMPP::EncryptionJob *runData(bool encrypting, const QByteArray &data, bool streaming = false,
                                     bool final = false)
        {
            auto *job = new XMPP::EncryptionJob(this);
            if (!method_ || !pluginSession_) {
                job->fail(XMPP::EncryptionJob::Error::Cancelled,
                          QStringLiteral("Encryption plugin is no longer available"));
                return job;
            }

            auto                         *guard = new QObject(job);
            QPointer<XMPP::EncryptionJob> weakJob(job);
            activeOperations_.insert(job, guard);
            connect(job, &XMPP::EncryptionJob::finished, this, [this, job]() { activeOperations_.remove(job); });
            connect(job, &QObject::destroyed, this, [this, job]() { activeOperations_.remove(job); });
            auto completion = [weakJob, methodId = methodId()](EncryptionMethodProvider::Result result) {
                if (!weakJob || weakJob->isFinished())
                    return;
                if (!result.success) {
                    weakJob->fail(toIrisError(result.error), result.errorString);
                    return;
                }
                weakJob->complete(result.data, toIrisMetadata(methodId, result.metadata));
            };

            if (streaming) {
                if (encrypting)
                    pluginSession_->encryptChunk(data, final, guard, std::move(completion));
                else
                    pluginSession_->decryptChunk(data, final, guard, std::move(completion));
            } else if (encrypting) {
                pluginSession_->encryptData(data, guard, std::move(completion));
            } else {
                pluginSession_->decryptData(data, guard, std::move(completion));
            }
            return job;
        }

        QPointer<PluginMethodAdapter>                      method_;
        std::unique_ptr<EncryptionMethodProvider::Session> pluginSession_;
        QHash<XMPP::EncryptionJob *, QObject *>            activeOperations_;

        friend class PluginMethodAdapter;
    };

    PluginMethodAdapter(int accountId, EncryptionMethodProvider *provider, QObject *parent = nullptr) :
        XMPP::EncryptionMethod(parent), accountId_(accountId), provider_(provider),
        id_(provider ? provider->id() : QString())
    {
    }

    QString      id() const override { return id_; }
    QString      name() const override { return provider_ ? provider_->name() : id_; }
    Capabilities capabilities() const override
    {
        return provider_ ? toIrisCapabilities(provider_->capabilities()) : Capabilities {};
    }
    GroupchatRequirements groupchatRequirements() const override
    {
        GroupchatRequirements result;
        if (!provider_ || !capabilities().testFlag(Groupchat))
            return result;
        const auto requirements = provider_->groupchatRequirements();
        if (requirements.testFlag(EncryptionMethodProvider::MembersOnly))
            result |= MembersOnly;
        if (requirements.testFlag(EncryptionMethodProvider::NonAnonymous))
            result |= NonAnonymous;
        if (requirements.testFlag(EncryptionMethodProvider::MemberJids))
            result |= MemberJids;
        return result;
    }
    XMPP::EncryptedSession *startSession(Capabilities capabilities, const XMPP::EncryptionContext &context) override
    {
        if (!provider_ || (capabilities & this->capabilities()) != capabilities)
            return nullptr;
        auto *pluginSession = provider_->startSession(accountId_, toPluginContext(context));
        if (!pluginSession)
            return nullptr;
        return new Session(this, context, pluginSession);
    }
    XMPP::Features features() const override
    {
        return provider_ ? XMPP::Features(provider_->features()) : XMPP::Features();
    }
    bool canDecrypt(const QDomElement &stanza) const override
    {
        return provider_ && provider_->canDecrypt(accountId_, stanza);
    }
    Availability availability(const XMPP::Jid &peer) const override
    {
        if (!provider_)
            return Availability::Unavailable;
        switch (provider_->availability(accountId_, peer.full())) {
        case EncryptionMethodProvider::Availability::Available:
            return Availability::Available;
        case EncryptionMethodProvider::Availability::Unavailable:
            return Availability::Unavailable;
        case EncryptionMethodProvider::Availability::Unknown:
            return Availability::Unknown;
        }
        return Availability::Unknown;
    }

    void detachProvider()
    {
        const auto sessions = sessions_;
        sessions_.clear();
        for (auto *session : sessions) {
            if (session) {
                session->detachProvider();
                session->method_ = nullptr;
            }
        }
        provider_ = nullptr;
    }

    int                       accountId_ = -1;
    EncryptionMethodProvider *provider_  = nullptr;
    QString                   id_;
    QSet<Session *>           sessions_;
};

struct PsiEncryptionController::PluginRegistration {
    int                       accountId = -1;
    EncryptionMethodProvider *provider  = nullptr;
    PluginMethodAdapter      *adapter   = nullptr;
    QMetaObject::Connection   lifetimeConnection;
};

PsiEncryptionController::PsiEncryptionController(PsiAccount *account, XMPP::Client *client,
                                                 const QMap<QString, QString> &selectedMethods,
                                                 const QString &profileDataPath, const QString &accountId,
                                                 QObject *parent) :
    QObject(parent), account_(account), client_(client), selectedMethods_(selectedMethods)
{
    Q_ASSERT(account_);
    Q_ASSERT(client_);

    connect(client_->encryptionManager(), &XMPP::EncryptionManager::methodRegistered, this,
            [this](const QString &) { emit methodsChanged(); });
    connect(client_->encryptionManager(), &XMPP::EncryptionManager::methodUnregistered, this,
            [this](const QString &) { emit methodsChanged(); });
    connect(client_->capsManager(), &XMPP::CapsManager::capsChanged, this,
            [this](const XMPP::Jid &jid) { emit peerAvailabilityChanged(jid); });
    connect(client_, &XMPP::Client::resourceAvailable, this,
            [this](const XMPP::Jid &jid, const XMPP::Resource &) { emit peerAvailabilityChanged(jid); });
    connect(client_, &XMPP::Client::resourceUnavailable, this,
            [this](const XMPP::Jid &jid, const XMPP::Resource &) { emit peerAvailabilityChanged(jid); });
    connect(client_, &XMPP::Client::stanzaDecryptionFailed, this,
            [this](const QString &methodId, const XMPP::Jid &peer, XMPP::EncryptionJob::Error errorCode,
                   const QString &error, const XMPP::EncryptionMetadata &metadata) {
                qWarning().noquote() << QStringLiteral("%1 stanza decryption failed: %2").arg(methodId, error);
                // libomemo-c codes: a message received twice is ignored silently (XEP-0384 section 6); a message
                // no ratchet state accepts comes from a broken session, which the user may repair.
                constexpr int InvalidMac = -1004, InvalidMessage = -1005;
                const int signalError = metadata.details.value(QStringLiteral("signalError")).toInt();
                if (isDuplicateDecryptionFailure(methodId, metadata)
                    || metadata.details.value(QStringLiteral("archiveReplay")).toBool())
                    return; // Archived failures have their own timeline entry.
                const bool broken = methodId == QLatin1String("omemo")
                    && (signalError == InvalidMac || signalError == InvalidMessage);
                // One notice per sending device and kind of failure, not one per message.
                const QString key = QStringLiteral("%1|%2|%3|%4")
                                        .arg(methodId, metadata.sender.bare())
                                        .arg(metadata.senderDeviceId)
                                        .arg(int(errorCode));
                const qint64     now              = QDateTime::currentMSecsSinceEpoch();
                constexpr qint64 NoticeIntervalMs = 10 * 60 * 1000;
                if (now - decryptionFailureNotices_.value(key, -NoticeIntervalMs) >= NoticeIntervalMs) {
                    decryptionFailureNotices_.insert(key, now);
                    emit encryptionError(peer,
                                         broken ? tr("Could not decrypt a message: the encryption session with this "
                                                     "device is broken (%1)")
                                                      .arg(error)
                                                : tr("Decryption failed: %1").arg(error));
                }
                // The MUC stanza's 'from' is room/nick, not an OMEMO
                // account. Never fetch a bundle or request trust for the
                // conference JID. Once MUC sender resolution supplies a real
                // bare JID in the metadata, the normal repair flow applies.
                const bool isMuc = account_->groupchats().contains(peer.bare());
                const bool unresolvedMucSender = isMuc
                    && (metadata.sender.bare().isEmpty() || metadata.sender.bare() == peer.bare());
                // In a MUC a rejected/changed identity must also lead to an
                // explicit trust or repair decision, never silent dismissal.
                // This is only safe once Iris resolved the actual sender.
                const bool mucUntrusted = isMuc && !unresolvedMucSender
                    && errorCode == XMPP::EncryptionJob::Error::UntrustedIdentity;
                if ((errorCode == XMPP::EncryptionJob::Error::NoSession || broken || mucUntrusted)
                    && !unresolvedMucSender)
                    recoverDecryption(methodId, peer, metadata);
            });
    connect(account_, &PsiAccount::pgpKeyChanged, this, [this]() {
        emit methodStateChanged(QString::fromLatin1(OpenPgpMethodId));
        emit methodsChanged();
    });

    // A local-network identity has no server PEP service for OMEMO bundles.
    // Leave the normal plugin method registration available to direct transports.
    if (account_->isServerless())
        return;
#ifdef IRIS_ENABLE_OMEMO
    omemoStorage_ = std::make_unique<PsiOmemoStorage>(profileDataPath, accountId);
    if (omemoStorage_->isOpen()) {
        legacyOmemoEnabled_  = omemoStorage_->legacyEnabledJids();
        legacyOmemoDisabled_ = omemoStorage_->legacyDisabledJids();
        omemo_               = new XMPP::OmemoEncryption(client_, omemoStorage_.get(), omemoStorage_.get(), this);
        // Psi uses explicit device verification. Unknown identities are learned
        // so their fingerprints can be shown, but they cannot be used for a
        // newly built outgoing session until the user makes a decision.
        omemo_->setNewIdentityTrustLevel(XMPP::EncryptionTrustLevel::Undecided);
        omemo_->setAcceptedSessionBuildingTrustLevels(XMPP::EncryptionTrustLevel::ManuallyTrusted
                                                      | XMPP::EncryptionTrustLevel::Authenticated);

        // Preserve historical trust decisions from the old OMEMO plugin.
        for (const auto &device : omemo_->devices()) {
            if (device.identityKey.isEmpty() || !device.protocols.testFlag(XMPP::OmemoProtocol::Legacy))
                continue;
            const auto legacyTrust = omemoStorage_->legacyTrust(device.owner.bare(), device.id);
            if (!legacyTrust || device.trust != XMPP::EncryptionTrustLevel::Undecided)
                continue;
            switch (*legacyTrust) {
            case PsiOmemoStorage::LegacyTrust::Trusted:
                omemo_->setTrustLevel(device.owner, device.identityKey, XMPP::EncryptionTrustLevel::ManuallyTrusted);
                break;
            case PsiOmemoStorage::LegacyTrust::Untrusted:
                omemo_->setTrustLevel(device.owner, device.identityKey, XMPP::EncryptionTrustLevel::Distrusted);
                break;
            case PsiOmemoStorage::LegacyTrust::Undecided:
                break;
            }
        }

        connect(omemo_, &XMPP::OmemoEncryption::trustChanged, this,
                [this](const XMPP::Jid &owner, const QByteArray &identityKey, XMPP::EncryptionTrustLevel level) {
                    qCDebug(omemoTrustLog)
                        << "trust-saved owner=" << owner.bare() << "level=" << int(level) << "key-tag="
                        << QCryptographicHash::hash(identityKey, QCryptographicHash::Sha256).toHex().left(12);
                    if (!omemoStorage_ || !omemo_)
                        return;
                    for (const auto &device : omemo_->devices(owner)) {
                        if (device.identityKey != identityKey
                            || !device.protocols.testFlag(XMPP::OmemoProtocol::Legacy))
                            continue;
                        const auto legacy = level == XMPP::EncryptionTrustLevel::Distrusted
                            ? PsiOmemoStorage::LegacyTrust::Untrusted
                            : (level == XMPP::EncryptionTrustLevel::Undecided ? PsiOmemoStorage::LegacyTrust::Undecided
                                                                              : PsiOmemoStorage::LegacyTrust::Trusted);
                        omemoStorage_->setLegacyTrust(owner.bare(), device.id, legacy);
                    }
                    emit methodStateChanged(XMPP::OmemoEncryption::methodId());
                });
        connect(omemo_, &XMPP::OmemoEncryption::deviceChanged, this,
                [this](const XMPP::Jid &, uint32_t) { emit methodStateChanged(XMPP::OmemoEncryption::methodId()); });
        connect(omemo_, &XMPP::OmemoEncryption::deviceRemoved, this,
                [this](const XMPP::Jid &, uint32_t) { emit methodStateChanged(XMPP::OmemoEncryption::methodId()); });
        connect(omemo_, &XMPP::OmemoEncryption::readyChanged, this,
                [this](bool) { emit methodStateChanged(XMPP::OmemoEncryption::methodId()); });
        connect(omemo_, &XMPP::OmemoEncryption::warning, this,
                [this](const QString &message) { emit encryptionError({}, message); });
    } else {
        qWarning() << "Could not open OMEMO storage:" << omemoStorage_->errorString();
    }
#else
    Q_UNUSED(profileDataPath);
    Q_UNUSED(accountId);
#endif
}

PsiEncryptionController::~PsiEncryptionController()
{
    // OmemoEncryption unregisters itself from EncryptionManager in its
    // destructor. Do not turn that teardown notification into methodsChanged():
    // observers may already be going away during account/application shutdown.
    disconnect(client_->encryptionManager(), nullptr, this, nullptr);

    const auto providers = pluginMethods_.keys();
    for (auto *provider : providers)
        unregisterPluginMethod(provider);

#ifdef IRIS_ENABLE_OMEMO
    // OmemoEncryption is a QObject child while its storage is a C++ member.
    // Destroy it explicitly before member destruction so it cannot observe an
    // already-destroyed storage backend from QObject's base destructor.
    if (trustReview_) {
        disconnect(trustReview_, nullptr, this, nullptr);
        delete trustReview_.data();
    }
    delete omemo_;
    omemo_ = nullptr;
#endif
}

XMPP::Client *PsiEncryptionController::client() const { return client_; }

QList<PsiEncryptionController::MethodInfo>
PsiEncryptionController::methods(XMPP::EncryptionMethod::Capabilities capabilities) const
{
    QList<MethodInfo> result;

    if ((capabilities == XMPP::EncryptionMethod::Capabilities {}
         || capabilities == XMPP::EncryptionMethod::Capabilities(XMPP::EncryptionMethod::XmppStanza))
        && account_->hasPgp() && PsiOptions::instance()->getOption("plugins.auto-load.openpgp", false).toBool()) {
        MethodInfo pgp;
        pgp.id             = QString::fromLatin1(OpenPgpMethodId);
        pgp.name           = QStringLiteral("OpenPGP");
        pgp.icon           = IconsetFactory::icon("psi/cryptoYes").icon();
        pgp.capabilities   = XMPP::EncryptionMethod::XmppStanza;
        pgp.uiCapabilities = EncryptionMethodProvider::KeyManagement;
        result.append(pgp);
    }

    const auto nativeMethods = client_->encryptionManager()->methods(capabilities);
    for (const auto &[id, name] : nativeMethods) {
        if (account_->isServerless() && id == QLatin1String("omemo"))
            continue;
        MethodInfo info;
        info.id           = id;
        info.name         = name;
        info.capabilities = client_->encryptionManager()->method(id)->capabilities();

        if (auto *provider = pluginProvider(id)) {
            info.pluginProvided = true;
            info.icon           = provider->icon();
            info.uiCapabilities = provider->uiCapabilities();
        }
#ifdef IRIS_ENABLE_OMEMO
        else if (id == XMPP::OmemoEncryption::methodId()) {
            info.icon           = IconsetFactory::icon("psi/omemo").icon();
            info.uiCapabilities = EncryptionMethodProvider::KeyManagement | EncryptionMethodProvider::DeviceManagement
                | EncryptionMethodProvider::TrustManagement;
        }
#endif
        result.append(info);
    }

    return result;
}

bool PsiEncryptionController::hasMethod(const QString &methodId) const
{
    if (methodId.isEmpty())
        return true;
    // OMEMO device/bundle discovery requires server PEP. Direct transports
    // still support separately registered encryption methods such as OTR.
    if (account_->isServerless() && methodId == QLatin1String("omemo"))
        return false;
    if (methodId == QLatin1String(OpenPgpMethodId))
        return account_->hasPgp() && PsiOptions::instance()->getOption("plugins.auto-load.openpgp", false).toBool();
    return client_->encryptionManager()->method(methodId) != nullptr;
}

PsiEncryptionController::MethodInfo PsiEncryptionController::methodInfo(const QString &methodId) const
{
    for (const auto &info : methods(XMPP::EncryptionMethod::Capabilities {})) {
        if (info.id == methodId)
            return info;
    }
    return {};
}

XMPP::EncryptionMethod::GroupchatRequirements
PsiEncryptionController::groupchatRequirements(const QString &methodId) const
{
    auto *method = client_->encryptionManager()->method(methodId);
    return method && method->capabilities().testFlag(XMPP::EncryptionMethod::Groupchat)
        ? method->groupchatRequirements() : XMPP::EncryptionMethod::GroupchatRequirements {};
}

QString PsiEncryptionController::groupchatAvailabilityError(const QString &methodId, bool roomInfoKnown,
                                                             bool membersOnly, bool nonAnonymous) const
{
    using Method = XMPP::EncryptionMethod;
    auto *method = client_->encryptionManager()->method(methodId);
    const auto caps = Method::XmppStanza | Method::Groupchat;
    if (!method || (method->capabilities() & caps) != caps)
        return tr("The selected encryption method does not support groupchat");
    const auto requirements = method->groupchatRequirements();
    if (!roomInfoKnown && requirements != Method::GroupchatRequirements {})
        return tr("Checking room encryption requirements");
    if (requirements.testFlag(Method::MembersOnly) && !membersOnly)
        return tr("This encryption method requires a members-only room");
    if (requirements.testFlag(Method::NonAnonymous) && !nonAnonymous)
        return tr("This encryption method requires a non-anonymous room");
    return {};
}

void PsiEncryptionController::populateMethodMenu(
    QMenu *menu, XMPP::EncryptionMethod::Capabilities capabilities, const QString &current,
    const std::function<QString(const QString &)> &unavailableReason,
    const std::function<void(const QString &)> &select) const
{
    auto *plain = menu->addAction(tr("No encryption"));
    plain->setCheckable(true);
    plain->setChecked(current.isEmpty());
    connect(plain, &QAction::triggered, menu, [select]() { select({}); });

    bool found = current.isEmpty();
    for (const auto &info : methods(capabilities)) {
        auto *action = menu->addAction(info.icon, info.name);
        action->setCheckable(true);
        action->setData(info.id);
        action->setChecked(current == info.id);
        found = found || current == info.id;
        const auto reason = unavailableReason(info.id);
        action->setEnabled(reason.isEmpty());
        action->setToolTip(reason);
        connect(action, &QAction::triggered, menu, [select, id = info.id]() { select(id); });
    }
    if (!found) {
        const auto info = methodInfo(current);
        auto *action = menu->addAction(tr("%1 (unavailable)").arg(info.name.isEmpty() ? current : info.name));
        action->setCheckable(true);
        action->setChecked(true);
        action->setEnabled(false);
    }
}

bool PsiEncryptionController::isDuplicateDecryptionFailure(const QString &methodId,
                                                          const XMPP::EncryptionMetadata &metadata) const
{
    return methodId == QLatin1String("omemo")
        && metadata.details.value(QStringLiteral("signalError")).toInt() == -1001;
}

void PsiEncryptionController::requestGroupchatTrustDecision(const QString &methodId, const QList<XMPP::Jid> &members,
                                                            QWidget *parent, QObject *scope,
                                                            std::function<void(bool)> completion)
{
#ifdef IRIS_ENABLE_OMEMO
    if (omemo_ && methodId == XMPP::OmemoEncryption::methodId() && scope) {
        const QPointer<QObject> guard(scope);
        const auto collect = [this, members, guard]() {
            QList<XMPP::OmemoDeviceInfo> candidates;
            if (!guard)
                return candidates;
            for (const auto &member : members)
                candidates.append(omemo_->devices(member));
            return PsiOmemo::devicesForDecision(candidates, 0, XMPP::OmemoProtocol::Omemo2,
                                                PsiOmemo::DeviceDecision::VerifyIdentity,
                                                client_->jid(), omemo_->ownDeviceId());
        };
        if (enqueueTrustReview(collect, PsiOmemo::DeviceDecision::VerifyIdentity, parent,
                               [guard, completion](bool approved) {
                                   if (guard && completion)
                                       completion(approved);
                               }))
            return;
    }
#else
    Q_UNUSED(methodId)
    Q_UNUSED(members)
    Q_UNUSED(parent)
    Q_UNUSED(scope)
#endif
    if (completion)
        completion(false);
}

#ifdef IRIS_ENABLE_OMEMO
bool PsiEncryptionController::enqueueTrustReview(std::function<QList<XMPP::OmemoDeviceInfo>()> collect,
                                                 PsiOmemo::DeviceDecision purpose, QWidget *parent,
                                                 std::function<void(bool)> completion)
{
    if (collect().isEmpty())
        return false;
    if (!trustReview_) {
        auto *dialog = new TrustReviewDialog(this);
        trustReview_ = dialog;
        connect(dialog, &QDialog::finished, this, [this, dialog] {
            trustReview_.clear();
            dialog->resolve();
            dialog->deleteLater();
        });
        if (parent)
            dialog->move(parent->window()->geometry().center() - dialog->rect().center());
        dialog->add({ std::move(collect), purpose, std::move(completion) });
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    } else {
        trustReview_->add({ std::move(collect), purpose, std::move(completion) });
    }
    return true;
}
#endif

QString PsiEncryptionController::explicitlySelectedMethod(const XMPP::Jid &jid) const
{
    // Empty means either no explicit preference or explicitly unencrypted.
    // Both must keep a MUC on the original plaintext path.
    return selectedMethods_.value(jid.bare());
}

QString PsiEncryptionController::selectedMethod(const XMPP::Jid &jid) const
{
    const QString bare = jid.bare();
    if (bare.isEmpty())
        return {};

    const auto explicitMethod = selectedMethods_.constFind(bare);
    if (explicitMethod != selectedMethods_.cend())
        return *explicitMethod;

    // Migration policy: retain the old OpenPGP/OMEMO per-contact defaults
    // until the user explicitly chooses a method in the unified selector.
    if (account_->hasPgp() && PsiOptions::instance()->getOption("plugins.auto-load.openpgp", false).toBool()) {
        const bool alwaysPgp = PsiOptions::instance()->getOption("options.pgp.always-enabled", false).toBool();
        if (alwaysPgp || account_->isPgpEnabled(jid))
            return QString::fromLatin1(OpenPgpMethodId);
    }

#ifdef IRIS_ENABLE_OMEMO
    if (!account_->isServerless() && omemo_ && omemoStorage_) {
        const bool alwaysEnabled
            = PsiOptions::instance()->getOption("plugins.options.omemo.always-enabled", false).toBool();
        const bool enabledByDefault
            = PsiOptions::instance()->getOption("plugins.options.omemo.enabled-by-default", false).toBool();
        if (alwaysEnabled || (enabledByDefault && !legacyOmemoDisabled_.contains(bare))
            || legacyOmemoEnabled_.contains(bare)) {
            return XMPP::OmemoEncryption::methodId();
        }
    }
#endif
    return {};
}

void PsiEncryptionController::configureJingleSession(XMPP::Jingle::Session *session) const
{
    if (!session)
        return;
    const auto methodId = selectedMethod(session->peer());
    // The choice and local preference originate in Psi's UI. Iris binds the
    // method to an encryption session and validates every Jingle transport.
    session->setEncryptionMethod(methodId);
    using Policy = XMPP::Jingle::EncryptionPolicy;
    session->setEncryptionPolicy(jingleEndToEndRequired(session->peer()) ? Policy::Required
                                : methodId.isEmpty() ? Policy::Disabled : Policy::Preferred);
}

bool PsiEncryptionController::jingleEndToEndRequired(const XMPP::Jid &jid) const
{
    return !selectedMethod(jid).isEmpty()
        && PsiOptions::instance()->getOption(QStringLiteral("options.media.require-end-to-end"), false).toBool();
}

bool PsiEncryptionController::jingleEndToEndPreferred(const XMPP::Jid &jid) const
{
    return !selectedMethod(jid).isEmpty();
}

bool PsiEncryptionController::requestJingleTrustDecision(const XMPP::Jid &jid, QWidget *parent,
                                                         std::function<void()> completion)
{
#ifdef IRIS_ENABLE_OMEMO
    if (selectedMethod(jid) != XMPP::OmemoEncryption::methodId())
        return false;
    // The call goes on whatever the answer is; the end-to-end policy decides what it may use.
    return requestTrustDecision(XMPP::OmemoEncryption::methodId(), jid.withResource(QString()), false, parent,
                                [completion = std::move(completion)](bool) {
                                    if (completion)
                                        completion();
                                });
#else
    Q_UNUSED(jid)
    Q_UNUSED(parent)
    Q_UNUSED(completion)
    return false;
#endif
}

bool PsiEncryptionController::setSelectedMethod(const XMPP::Jid &jid, const QString &methodId)
{
    const QString bare = jid.bare();
    if (bare.isEmpty())
        return false;
    if (!methodId.isEmpty() && !hasMethod(methodId))
        return false;
    if (selectedMethods_.contains(bare) && selectedMethods_.value(bare) == methodId)
        return true;

    selectedMethods_.insert(bare, methodId); // empty means explicit plaintext
    syncLegacySelection(bare, methodId);
    emit selectedMethodChanged(XMPP::Jid(bare), methodId);
    return true;
}

void PsiEncryptionController::adoptIncomingGroupchatMethod(const XMPP::Jid &room, const QString &methodId)
{
    // Only authenticated live messages may establish an initial preference.
    // An explicit plaintext choice or another selected method is not replaced.
    if (selectedMethods_.contains(room.bare()))
        return;
    using Method = XMPP::EncryptionMethod;
    auto *method = client_->encryptionManager()->method(methodId);
    const auto required = Method::XmppStanza | Method::Groupchat;
    if (method && (method->capabilities() & required) == required)
        setSelectedMethod(room, methodId);
}

QMap<QString, QString> PsiEncryptionController::selectedMethods() const { return selectedMethods_; }

void PsiEncryptionController::setSelectedMethods(const QMap<QString, QString> &methods)
{
    if (selectedMethods_ == methods)
        return;
    selectedMethods_ = methods;
    emit methodsChanged();
}

XMPP::Jid PsiEncryptionController::capabilityJid(const XMPP::Jid &jid) const
{
    if (!jid.resource().isEmpty())
        return jid;

    auto *item = account_->findFirstRelevant(jid);
    if (!item || !item->isAvailable() || item->userResourceList().isEmpty())
        return jid;

    const auto priority = item->userResourceList().priority();
    if (priority == item->userResourceList().end())
        return jid;
    return jid.withResource(priority->name());
}

PsiEncryptionController::Availability PsiEncryptionController::availability(const QString   &methodId,
                                                                            const XMPP::Jid &jid) const
{
    if (methodId.isEmpty())
        return Availability::Available;
    if (!hasMethod(methodId))
        return Availability::Unavailable;

    // OpenPGP is a legacy Psi-only method; native and plugin methods are
    // registered with Iris and report their own availability.
    if (methodId == QLatin1String(OpenPgpMethodId))
        return Availability::Available;

    switch (client_->encryptionManager()->availability(methodId, capabilityJid(jid))) {
    case XMPP::EncryptionMethod::Availability::Available:
        return Availability::Available;
    case XMPP::EncryptionMethod::Availability::Unavailable:
        return Availability::Unavailable;
    case XMPP::EncryptionMethod::Availability::Unknown:
        return Availability::Unknown;
    }
    return Availability::Unknown;
}

void PsiEncryptionController::reportError(const XMPP::Jid &jid, const QString &message)
{
    emit encryptionError(jid, message);
}

void PsiEncryptionController::recoverDecryption(const QString &methodId, const XMPP::Jid &displayPeer,
                                                const XMPP::EncryptionMetadata &metadata)
{
    if (!client_ || metadata.methodId != methodId || !metadata.sender.isValid() || metadata.senderDeviceId == 0)
        return;

    const QString recoveryKey = decryptionRecoveryKey(methodId, metadata);
    if (decryptionRecoveriesActive_.contains(recoveryKey))
        return;

    constexpr qint64 RecoveryCooldownMs = 60 * 1000;
    const qint64     now                = QDateTime::currentMSecsSinceEpoch();
    for (auto it = decryptionRecoveryAttempts_.begin(); it != decryptionRecoveryAttempts_.end();) {
        if (now < it.value() || now - it.value() >= RecoveryCooldownMs)
            it = decryptionRecoveryAttempts_.erase(it);
        else
            ++it;
    }
    if (now - decryptionRecoveryAttempts_.value(recoveryKey, 0) < RecoveryCooldownMs)
        return;
    decryptionRecoveryAttempts_.insert(recoveryKey, now);
    decryptionRecoveriesActive_.insert(recoveryKey);

    // Resolving the public bundle is deliberately separate from creating the
    // session. XEP-0384 requires explicit user interaction before a client
    // replaces a session in response to a decryption error.
    auto *method = client_->encryptionManager()->method(methodId);
    auto *job    = method ? method->prepareDecryptionRecovery(metadata) : nullptr;
    if (!job) {
        decryptionRecoveriesActive_.remove(recoveryKey);
        return;
    }

    const auto finish = [this, job, methodId, displayPeer, recoveryKey]() {
        if (!job->success()) {
            decryptionRecoveriesActive_.remove(recoveryKey);
            emit encryptionError(displayPeer, tr("Could not prepare session recovery: %1").arg(job->errorString()));
            job->deleteLater();
            return;
        }

        const auto prepared = job->metadata();
        job->deleteLater();
        const bool requested = requestTrustDecision(
            methodId, prepared.sender, false, nullptr,
            [this, methodId, displayPeer, prepared, recoveryKey](bool approved) {
                decryptionRecoveriesActive_.remove(recoveryKey);
                if (approved)
                    performDecryptionRecovery(methodId, displayPeer, prepared);
            },
            prepared.senderDeviceId, prepared.details.value(QStringLiteral("omemoProtocol")).toString(),
            PsiOmemo::DeviceDecision::RepairSession);
        if (!requested) {
            decryptionRecoveriesActive_.remove(recoveryKey);
            emit encryptionError(displayPeer,
                                 tr("Session recovery was not offered because the sending device is unavailable or "
                                    "explicitly distrusted."));
        }
    };
    if (job->isFinished())
        finish();
    else
        connect(job, &XMPP::EncryptionJob::finished, this, finish);
}

void PsiEncryptionController::performDecryptionRecovery(const QString &methodId, const XMPP::Jid &displayPeer,
                                                        const XMPP::EncryptionMetadata &metadata)
{
    const QString recoveryKey = decryptionRecoveryKey(methodId, metadata);
    if (!client_ || decryptionRecoveriesActive_.contains(recoveryKey))
        return;
    decryptionRecoveriesActive_.insert(recoveryKey);

    auto *method = client_->encryptionManager()->method(methodId);
    auto *job    = method ? method->recoverDecryption(metadata) : nullptr;
    if (!job) {
        decryptionRecoveriesActive_.remove(recoveryKey);
        return;
    }

    const auto finish = [this, job, displayPeer, metadata, recoveryKey]() {
        decryptionRecoveriesActive_.remove(recoveryKey);
        if (job->success()) {
            emit encryptionError(
                displayPeer,
                tr("Sent a new key exchange to %1 device %2. Future messages from it can be decrypted, but the "
                   "failed message must be sent again.")
                    .arg(metadata.sender.bare())
                    .arg(metadata.senderDeviceId));
            job->deleteLater();
            return;
        }
        emit encryptionError(displayPeer, tr("Session recovery failed: %1").arg(job->errorString()));
        job->deleteLater();
    };
    if (job->isFinished())
        finish();
    else
        connect(job, &XMPP::EncryptionJob::finished, this, finish);
}

bool PsiEncryptionController::registerPluginMethod(int accountId, EncryptionMethodProvider *provider,
                                                   QObject *pluginLifetime)
{
    if (!provider || provider->id().isEmpty() || pluginMethods_.contains(provider))
        return false;
    if (provider->id() == QLatin1String(OpenPgpMethodId) || client_->encryptionManager()->method(provider->id()))
        return false;

    auto *registration      = new PluginRegistration;
    registration->accountId = accountId;
    registration->provider  = provider;
    registration->adapter   = new PluginMethodAdapter(accountId, provider, this);

    pluginMethodsById_.insert(provider->id(), provider);
    pluginMethods_.insert(provider, registration);

    auto *adapter = registration->adapter;
    if (!client_->encryptionManager()->registerMethod(adapter)) {
        pluginMethodsById_.remove(provider->id());
        pluginMethods_.remove(provider);
        delete adapter;
        delete registration;
        return false;
    }

    if (pluginLifetime) {
        pluginMethods_.value(provider)->lifetimeConnection = connect(
            pluginLifetime, &QObject::destroyed, this, [this, provider]() { unregisterPluginMethod(provider); });
    }
    return true;
}

void PsiEncryptionController::unregisterPluginMethod(EncryptionMethodProvider *provider)
{
    auto it = pluginMethods_.find(provider);
    if (it == pluginMethods_.end())
        return;

    auto *registration = it.value();
    pluginMethods_.erase(it);
    pluginMethodsById_.remove(registration->adapter->id());
    disconnect(registration->lifetimeConnection);

    registration->adapter->detachProvider();
    client_->encryptionManager()->unregisterMethod(registration->adapter);
    delete registration->adapter;
    delete registration;
}

void PsiEncryptionController::pluginMethodStateChanged(EncryptionMethodProvider *provider)
{
    const auto it = pluginMethods_.constFind(provider);
    if (it == pluginMethods_.cend())
        return;
    emit methodStateChanged(it.value()->adapter->id());
}

QWidget *PsiEncryptionController::createSettingsWidget(const QString &methodId, QWidget *parent) const
{
    auto *provider = pluginProvider(methodId);
    if (!provider)
        return nullptr;
    const auto &registration = pluginMethods_.value(provider);
    return provider->createSettingsWidget(registration->accountId, parent);
}

QWidget *PsiEncryptionController::createKeyManagementWidget(const QString &methodId, const XMPP::Jid &jid,
                                                            QWidget *parent) const
{
    auto *provider = pluginProvider(methodId);
    if (!provider)
        return nullptr;
    const auto &registration = pluginMethods_.value(provider);
    return provider->createKeyManagementWidget(registration->accountId, jid.full(), parent);
}

QWidget *PsiEncryptionController::createDeviceManagementWidget(const QString &methodId, const XMPP::Jid &jid,
                                                               QWidget *parent) const
{
    auto *provider = pluginProvider(methodId);
    if (!provider)
        return nullptr;
    const auto &registration = pluginMethods_.value(provider);
    return provider->createDeviceManagementWidget(registration->accountId, jid.full(), parent);
}

QWidget *PsiEncryptionController::createTrustManagementWidget(const QString &methodId, const XMPP::Jid &jid,
                                                              QWidget *parent) const
{
    auto *provider = pluginProvider(methodId);
    if (!provider)
        return nullptr;
    const auto &registration = pluginMethods_.value(provider);
    return provider->createTrustManagementWidget(registration->accountId, jid.full(), parent);
}

bool PsiEncryptionController::requestTrustDecision(const QString &methodId, const XMPP::Jid &peer,
                                                   bool includeOwnDevices, QWidget *parent,
                                                   std::function<void(bool)> completion, quint32 deviceId,
                                                   const QString &profile, PsiOmemo::DeviceDecision purpose)
{
#ifdef IRIS_ENABLE_OMEMO
    if (!omemo_ || methodId != XMPP::OmemoEncryption::methodId())
        return false;

    const QString peerBare = peer.bare();
    if (peerBare.isEmpty())
        return false;
    const bool isRecovery = purpose == PsiOmemo::DeviceDecision::RepairSession;

    const QString                      ownBare = client_->jid().bare();
    std::optional<XMPP::OmemoProtocol> profileFilter;
    if (profile == QLatin1String("legacy"))
        profileFilter = XMPP::OmemoProtocol::Legacy;
    else if (profile == QLatin1String("omemo2"))
        profileFilter = XMPP::OmemoProtocol::Omemo2;

    const auto collectDevices = [this, peerBare, ownBare, includeOwnDevices, deviceId, profileFilter, purpose]() {
        auto candidates = omemo_->devices(XMPP::Jid(peerBare));
        if (includeOwnDevices && !ownBare.isEmpty() && ownBare != peerBare)
            candidates.append(omemo_->devices(XMPP::Jid(ownBare)));
        return PsiOmemo::devicesForDecision(candidates, deviceId, profileFilter, purpose,
                                            client_->jid(), omemo_->ownDeviceId());
    };

    const auto initialPending = collectDevices();
    qCDebug(omemoTrustLog) << "decision account=" << ownBare << "peer=" << peerBare
                           << "reason=" << (isRecovery ? "session-repair" : "identity-verification")
                           << "device=" << deviceId << "local-device=" << omemo_->ownDeviceId()
                           << "profile=" << profile << "own-devices=" << includeOwnDevices
                           << "pending=" << initialPending.size();
    for (const auto &device : initialPending) {
        qCDebug(omemoTrustLog)
            << "pending owner=" << device.owner.bare() << "device=" << device.id << "profile=" << int(device.protocol)
            << "trust=" << int(device.trust) << "has-session=" << device.hasSession
            << "key-tag=" << QCryptographicHash::hash(device.identityKey, QCryptographicHash::Sha256).toHex().left(12);
    }
    if (initialPending.isEmpty())
        return false;

    return enqueueTrustReview(collectDevices, purpose, parent, std::move(completion));
#else
    Q_UNUSED(methodId);
    Q_UNUSED(peer);
    Q_UNUSED(includeOwnDevices);
    Q_UNUSED(parent);
    Q_UNUSED(completion);
    Q_UNUSED(deviceId);
    Q_UNUSED(profile);
    Q_UNUSED(purpose);
    return false;
#endif
}

#ifdef IRIS_ENABLE_OMEMO
XMPP::OmemoEncryption *PsiEncryptionController::omemoEncryption() const { return omemo_; }

void PsiEncryptionController::setUpOmemo(const QString &deviceLabel)
{
    if (!omemo_)
        return;
    // A null label on reconnect preserves the existing device name. Only a
    // new identity needs a default; a non-null label is an explicit UI edit.
    const auto label = deviceLabel.isNull() && !omemo_->ownDeviceId() ? ApplicationInfo::name() : deviceLabel;
    auto      *job   = omemo_->setUp(label);
    const auto report = [this, job]() {
        if (!job->success()) {
            qWarning().noquote() << "OMEMO setup failed for" << client_->jid().bare() << ":" << job->errorString();
            emit encryptionError({}, tr("OMEMO setup failed: %1").arg(job->errorString()));
            return;
        }
        // Own devices that publish no bundle at all cannot receive anything; they are retired from the device
        // lists so contacts stop seeing them. A device that is alive announces itself again.
        auto      *sanitize = omemo_->sanitizeOwnPep();
        const auto done     = [sanitize]() {
            if (!sanitize->success())
                qWarning("OMEMO own device check failed: %s", qUtf8Printable(sanitize->errorString()));
            sanitize->deleteLater();
        };
        if (sanitize->isFinished())
            done();
        else
            connect(sanitize, &XMPP::EncryptionJob::finished, this, done);
    };
    if (job->isFinished()) {
        report();
        job->deleteLater();
    } else {
        connect(job, &XMPP::EncryptionJob::finished, this, report);
        connect(job, &XMPP::EncryptionJob::finished, job, &QObject::deleteLater);
    }
}
#endif

EncryptionMethodProvider *PsiEncryptionController::pluginProvider(const QString &methodId) const
{
    return pluginMethodsById_.value(methodId, nullptr);
}

void PsiEncryptionController::syncLegacySelection(const QString &bareJid, const QString &methodId)
{
    account_->setPgpEnabled(XMPP::Jid(bareJid), methodId == QLatin1String(OpenPgpMethodId));

#ifdef IRIS_ENABLE_OMEMO
    if (omemoStorage_ && omemoStorage_->isOpen()) {
        const bool omemoSelected = methodId == XMPP::OmemoEncryption::methodId();
        const bool enabledByDefault
            = PsiOptions::instance()->getOption("plugins.options.omemo.enabled-by-default", false).toBool();
        if (enabledByDefault) {
            omemoStorage_->setLegacyDisabled(bareJid, !omemoSelected);
            if (omemoSelected)
                legacyOmemoDisabled_.remove(bareJid);
            else
                legacyOmemoDisabled_.insert(bareJid);
        } else {
            omemoStorage_->setLegacyEnabled(bareJid, omemoSelected);
            if (omemoSelected)
                legacyOmemoEnabled_.insert(bareJid);
            else
                legacyOmemoEnabled_.remove(bareJid);
        }
    }
#else
    Q_UNUSED(bareJid);
#endif
}
