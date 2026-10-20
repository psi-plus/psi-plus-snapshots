/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "xmpp_bookmarkmanager.h"
#include "xmpp_pubsubreplica.h"

#include "xmpp_client.h"
#include "xmpp_features.h"
#include "xmpp_serverinfomanager.h"
#include "xmpp_xmlcommon.h"

#include <QDomDocument>
#include <QDomElement>
#include <QHash>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <optional>

namespace XMPP {
namespace {
    const QString legacyNs = QStringLiteral("storage:bookmarks");

    bool same(const NativeBookmark &a, const NativeBookmark &b) { return a.hasSamePayload(b); }

    bool same(const QList<NativeBookmark> &a, const QList<NativeBookmark> &b)
    {
        if (a.size() != b.size())
            return false;
        QHash<QString, NativeBookmark> indexed;
        for (const auto &item : a)
            indexed.insert(item.room().bare(), item);
        for (const auto &item : b) {
            auto it = indexed.constFind(item.room().bare());
            if (it == indexed.cend() || !same(it.value(), item))
                return false;
        }
        return true;
    }

    // Only the fields a client edits; an edit never asks to erase opaque
    // <extensions/> it did not know about.
    bool sameEditable(const NativeBookmark &a, const NativeBookmark &b)
    {
        return a.room().bare() == b.room().bare() && a.name() == b.name() && a.nick() == b.nick()
            && a.password() == b.password() && a.autoJoin() == b.autoJoin();
    }

    NativeBookmark withEditableFields(NativeBookmark base, const NativeBookmark &edit)
    {
        if (!base.isValid())
            return NativeBookmark::create(edit.room(), edit.name(), edit.autoJoin(), edit.nick(), edit.password());
        base.setName(edit.name());
        base.setAutoJoin(edit.autoJoin());
        base.setNick(edit.nick());
        base.setPassword(edit.password());
        return base;
    }
    QList<NativeBookmark> decodeConferences(const QDomElement &storage)
    {
        QList<NativeBookmark> rooms;
        for (auto el = storage.firstChildElement(); !el.isNull(); el = el.nextSiblingElement()) {
            if (el.tagName() != QLatin1String("conference"))
                continue;
            const Jid room(el.attribute(QStringLiteral("jid")));
            if (!room.isValid() || room.isEmpty() || !room.resource().isEmpty())
                continue;
            const auto join = el.attribute(QStringLiteral("autojoin"));
            rooms.append(NativeBookmark::create(room, el.attribute(QStringLiteral("name")),
                                                join == QLatin1String("true") || join == QLatin1String("1"),
                                                el.firstChildElement(QStringLiteral("nick")).text(),
                                                el.firstChildElement(QStringLiteral("password")).text()));
        }
        return rooms;
    }

    QList<BookmarkUrl> decodeUrls(const QDomElement &storage)
    {
        QList<BookmarkUrl> urls;
        for (auto el = storage.firstChildElement(); !el.isNull(); el = el.nextSiblingElement())
            if (el.tagName() == QLatin1String("url"))
                urls.append({ el.attribute(QStringLiteral("name")), el.attribute(QStringLiteral("url")) });
        return urls;
    }

    class PrivateBookmarksTask final : public Task {
    public:
        explicit PrivateBookmarksTask(Task *parent) : Task(parent) { }

        void get() { setup(QStringLiteral("get")); }
        void set(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences)
        {
            setup(QStringLiteral("set"));
            urls_        = urls;
            conferences_ = conferences;
            auto storage = iq_.firstChildElement(QStringLiteral("query")).firstChildElement(QStringLiteral("storage"));
            for (const auto &url : urls) {
                auto item = doc()->createElement(QStringLiteral("url"));
                item.setAttribute(QStringLiteral("name"), url.name);
                item.setAttribute(QStringLiteral("url"), url.url);
                storage.appendChild(item);
            }
            for (const auto &room : conferences) {
                if (!room.isValid())
                    continue;
                auto el = doc()->createElement(QStringLiteral("conference"));
                el.setAttribute(QStringLiteral("jid"), room.room().bare());
                el.setAttribute(QStringLiteral("name"), room.name());
                el.setAttribute(QStringLiteral("autojoin"),
                                room.autoJoin() ? QStringLiteral("true") : QStringLiteral("false"));
                const auto child = [this, &el](const QString &tag, const QString &text) {
                    if (text.isEmpty())
                        return;
                    auto node = doc()->createElement(tag);
                    node.appendChild(doc()->createTextNode(text));
                    el.appendChild(node);
                };
                child(QStringLiteral("nick"), room.nick());
                child(QStringLiteral("password"), room.password());
                storage.appendChild(el);
            }
        }
        const QList<BookmarkUrl>    &urls() const { return urls_; }
        const QList<NativeBookmark> &conferences() const { return conferences_; }

    protected:
        void onGo() override { send(iq_); }
        bool take(const QDomElement &stanza) override
        {
            if (!iqVerify(stanza, Jid(), id()))
                return false;
            if (stanza.attribute(QStringLiteral("type")) != QLatin1String("result")) {
                setError(stanza);
                return true;
            }
            if (get_) {
                const auto query = stanza.firstChildElement(QStringLiteral("query"));
                if (!query.isNull() && query.namespaceURI() == QLatin1String("jabber:iq:private")) {
                    auto storage = query.firstChildElement(QStringLiteral("storage"));
                    if (!storage.isNull() && storage.namespaceURI() == legacyNs) {
                        urls_        = decodeUrls(storage);
                        conferences_ = decodeConferences(storage);
                    }
                }
            }
            setSuccess();
            return true;
        }

    private:
        void setup(const QString &type)
        {
            get_       = type == QLatin1String("get");
            iq_        = createIQ(doc(), type, QString(), id());
            auto query = doc()->createElementNS(QStringLiteral("jabber:iq:private"), QStringLiteral("query"));
            iq_.appendChild(query);
            query.appendChild(doc()->createElementNS(legacyNs, QStringLiteral("storage")));
        }
        QDomElement           iq_;
        bool                  get_ = false;
        QList<BookmarkUrl>    urls_;
        QList<NativeBookmark> conferences_;
    };
} // namespace

class BookmarksManager::Private {
public:
    Client                       *client          = nullptr;
    bool                          online          = false;
    bool                          available       = false;
    bool                          nativeActive    = false;
    bool                          nativeWasActive = false;
    bool                          nativeFallback  = false; // Initial empty PEP node while legacy data exists.
    bool                          batchingEdits   = false;
    bool                          legacyPending   = false;
    QString                       accountJid;
    bool                          legacyWriting    = false;
    bool                          pendingUrlsValid = false;
    bool                          legacyQueued     = false;
    QList<NativeBookmark>         queuedLegacyConferences;
    QList<BookmarkUrl>            queuedLegacyUrls;
    int                           retryDelayMs        = 0;
    bool                          nodeRepairAttempted = false;
    QHash<QString, int>           itemRetries;
    quint64                       generation = 1;
    PubSubReplica<NativeBookmark> replica;
    // Room edits made while XEP-0402 is not active (first activation, reload
    // after reconnect or failure). The replica has no authoritative snapshot
    // then, so the edits are kept relative to the displayed list and applied
    // once the node is loaded, or saved to Private XML on legacy fallback.
    QHash<QString, std::optional<NativeBookmark>> deferredEdits;
    QList<NativeBookmark>                         conferences;
    QList<BookmarkUrl>                            urls;
    QList<BookmarkUrl>                            pendingUrls;
};

BookmarksManager::BookmarksManager(Client *client, QObject *parent) :
    QObject(parent ? parent : client), d(std::make_unique<Private>())
{
    d->client    = client;
    auto *pubsub = client->pubSubManager();
    connect(client->serverInfoManager(), &ServerInfoManager::featuresChanged, this,
            &BookmarksManager::maybeEnableNative);
    connect(pubsub, &PubSubManager::itemPublished, this,
            [this](const Jid &service, const QString &node, const PubSubItem &item) {
                if (!d->online || (service.bare() != d->client->jid().bare() || !service.resource().isEmpty())
                    || node != NativeBookmark::node())
                    return;
                const auto native = NativeBookmark::fromItem(item);
                if (!native.isValid())
                    return;
                d->replica.published(native.room().bare(), native);
                if (d->nativeActive)
                    rebuildNativeConferences(ChangeSource::Push);
            });
    connect(pubsub, &PubSubManager::itemRetracted, this,
            [this](const Jid &service, const QString &node, const QString &id) {
                if (!d->online || (service.bare() != d->client->jid().bare() || !service.resource().isEmpty())
                    || node != NativeBookmark::node())
                    return;
                d->replica.retracted(id);
                if (d->nativeActive)
                    rebuildNativeConferences(ChangeSource::Push);
            });
    const auto invalidate = [this](const Jid &service, const QString &node) {
        if (!d->online || (service.bare() != d->client->jid().bare() || !service.resource().isEmpty())
            || node != NativeBookmark::node()
            || (!d->nativeActive && d->replica.state() != PubSubReplica<NativeBookmark>::State::Loading))
            return;
        d->replica.invalidate(true);
        rebuildNativeConferences(ChangeSource::Push);
        d->nativeActive = false;
        fetchNative();
    };
    connect(pubsub, &PubSubManager::nodePurged, this, invalidate);
    connect(pubsub, &PubSubManager::nodeDeleted, this, invalidate);
    connect(client, &Client::disconnected, this, [this] { setOnline(false); });
}

BookmarksManager::~BookmarksManager() = default;
bool                         BookmarksManager::isAvailable() const { return d->available; }
bool                         BookmarksManager::usesNativeBookmarks() const { return d->nativeActive; }
const QList<NativeBookmark> &BookmarksManager::conferences() const { return d->conferences; }
const QList<BookmarkUrl>    &BookmarksManager::urls() const { return d->urls; }

void BookmarksManager::setAvailable(bool value)
{
    if (d->available == value)
        return;
    d->available = value;
    emit availabilityChanged(value);
}

void BookmarksManager::setOnline(bool online)
{
    const auto jid            = d->client->jid().bare();
    const bool changedAccount = online && !d->accountJid.isEmpty() && jid != d->accountJid;
    if (d->online == online && !changedAccount)
        return;
    d->online = online;
    ++d->generation;
    if (changedAccount) {
        // A Client object can be reused with different credentials. Never
        // replay the previous identity's pending data on the new account.
        d->replica.reset();
        d->deferredEdits.clear();
        d->nativeWasActive  = false;
        d->nativeFallback   = false;
        d->nativeActive     = false;
        d->pendingUrlsValid = false;
        d->pendingUrls.clear();
        d->legacyQueued = false;
        d->queuedLegacyUrls.clear();
        d->queuedLegacyConferences.clear();
        d->retryDelayMs        = 0;
        d->nodeRepairAttempted = false;
        d->itemRetries.clear();
        if (!d->urls.isEmpty()) {
            d->urls.clear();
            emit urlsChanged();
        }
        if (!d->conferences.isEmpty()) {
            d->conferences.clear();
            emit conferencesChanged();
        }
        setAvailable(false);
    }
    if (online) {
        d->accountJid = jid;
        fetchLegacy();
    } else {
        d->replica.reset(d->nativeWasActive);
        d->nativeActive  = false;
        d->legacyPending = false;
        if (d->legacyWriting && !d->nativeWasActive) {
            d->legacyQueued            = true;
            d->queuedLegacyUrls        = d->urls;
            d->queuedLegacyConferences = d->conferences;
        }
        d->legacyWriting = false;
        setAvailable(false);
    }
}

void BookmarksManager::fetchLegacy()
{
    if (!d->online || d->legacyPending)
        return;
    d->legacyPending = true;
    const auto gen   = d->generation;
    auto      *task  = new PrivateBookmarksTask(d->client->rootTask());
    connect(task, &Task::finished, this, [this, task, gen] {
        if (gen != d->generation || !d->online)
            return;
        d->legacyPending = false;
        if (task->success()) {
            if (d->urls != task->urls()) {
                d->urls = task->urls();
                emit urlsChanged();
            }
            if (!d->nativeWasActive && !d->nativeActive && !d->legacyQueued
                && !same(d->conferences, task->conferences())) {
                d->conferences = task->conferences();
                emit conferencesChanged();
            }
            setAvailable(true);
        } else if (!d->nativeWasActive) {
            setAvailable(false);
        }
        if (d->legacyQueued && !d->nativeWasActive) {
            d->legacyQueued = false;
            saveLegacy(d->queuedLegacyUrls, d->queuedLegacyConferences);
        } else {
            maybeEnableNative();
        }
    });
    task->get();
    task->go(true);
}

bool BookmarksManager::nativeCandidate() const
{
    const auto *server          = d->client->serverInfoManager();
    const auto &accountFeatures = server->accountFeatures();
    const bool  publishOptions  = accountFeatures.test("http://jabber.org/protocol/pubsub#publish-options")
        || server->serverFeatures().test("http://jabber.org/protocol/pubsub#publish-options");
    return !d->nativeFallback && server->hasPEP() && accountFeatures.test("urn:xmpp:bookmarks:1#compat")
        && publishOptions;
}

void BookmarksManager::maybeEnableNative()
{
    if (!d->online || d->legacyPending || d->legacyWriting || d->nativeActive
        || d->replica.state() == PubSubReplica<NativeBookmark>::State::Loading)
        return;
    if (nativeCandidate())
        fetchNative();
}

void BookmarksManager::scheduleNativeRetry()
{
    d->retryDelayMs = d->retryDelayMs ? qMin(d->retryDelayMs * 2, 60000) : 2000;
    const auto gen  = d->generation;
    QTimer::singleShot(d->retryDelayMs, this, [this, gen] {
        if (gen == d->generation && d->online && d->nativeWasActive && !d->nativeActive
            && d->replica.state() == PubSubReplica<NativeBookmark>::State::ResyncRequired)
            fetchNative();
    });
}

void BookmarksManager::fetchNative()
{
    const auto token = d->replica.beginLoad();
    if (!token)
        return;
    const auto gen  = d->generation;
    auto      *task = d->client->pubSubManager()->items(Jid(), NativeBookmark::node());
    connect(task, &Task::finished, this, [this, task, gen, token] {
        if (gen != d->generation || !d->online)
            return;
        const bool noNode = !task->success() && task->error().condition == Stanza::Error::ErrorCond::ItemNotFound;
        if (!task->success() && !(d->nativeWasActive && noNode)) {
            if (d->replica.failLoad(token)) {
                if (d->nativeWasActive)
                    scheduleNativeRetry();
                else
                    fallBackToLegacy(); // Never worked here: keep Private XML.
            }
            return;
        }
        PubSubReplica<NativeBookmark>::Items snapshot;
        if (task->success())
            for (const auto &item : task->items()) {
                const auto native = NativeBookmark::fromItem(item);
                if (native.isValid())
                    snapshot.insert(native.room().bare(), native);
            }
        if (!d->replica.finishLoad(token, std::move(snapshot)))
            return;
        // Do not silently replace an existing legacy collection by an empty
        // initially-unmigrated PEP node. This is a policy decision, not a merge.
        if (!d->nativeWasActive && d->replica.items().isEmpty() && !d->conferences.isEmpty()) {
            // The server advertised #compat but hasn't migrated existing
            // Private XML rooms. Keep the established legacy backend instead
            // of presenting an empty list or trying unsafe PEP replacement.
            d->replica.invalidate();
            fallBackToLegacy();
            return;
        }
        d->nativeFallback  = false;
        d->nativeActive    = true;
        d->nativeWasActive = true;
        d->retryDelayMs    = 0;
        setAvailable(true);
        applyDeferredEdits();
        rebuildNativeConferences();
        flushNativeWrites();
    });
    task->go(true);
}

void BookmarksManager::rebuildNativeConferences(ChangeSource source)
{
    // Never expose the stale last IQ as the UI's authoritative state while
    // a user intent remains queued or in-flight.
    const auto            optimistic = d->replica.optimisticItems();
    QList<NativeBookmark> next;
    QSet<QString>         emitted;
    // Preserve the user's existing ordering when switching between legacy
    // and PEP; only newly discovered rooms need a deterministic position.
    for (const auto &existing : d->conferences) {
        const auto id = existing.room().bare();
        if (optimistic.contains(id)) {
            next.append(optimistic.value(id));
            emitted.insert(id);
        }
    }
    QStringList fresh = optimistic.keys();
    fresh.sort(Qt::CaseInsensitive);
    for (const auto &id : fresh)
        if (!emitted.contains(id))
            next.append(optimistic.value(id));
    if (same(d->conferences, next))
        return;

    QHash<QString, NativeBookmark> before, after;
    for (const auto &room : d->conferences)
        before.insert(room.room().bare(), room);
    for (const auto &room : next)
        after.insert(room.room().bare(), room);
    d->conferences = std::move(next);
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (!after.contains(it.key()))
            emit conferenceRemoved(it.value().room(), source);
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto previous = before.constFind(it.key());
        if (previous == before.cend() || !same(previous.value(), it.value()))
            emit conferenceUpdated(it.value(), source);
    }
    emit conferencesChanged();
}

bool BookmarksManager::setBookmarks(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences)
{
    if (!d->online) {
        emit syncError(QStringLiteral("Cannot edit bookmarks while offline"));
        return false;
    }
    if (d->nativeWasActive || d->nativeActive || nativeCandidate()) {
        if (urls != d->urls) {
            emit syncError(QStringLiteral("URL bookmark editing is read-only with XEP-0402"));
            return false;
        }
        return setConferences(conferences);
    }
    // Validate before handing a full legacy IQ to the transport.
    QSet<QString> ids;
    for (const auto &room : conferences) {
        if (!room.isValid() || ids.contains(room.room().bare())) {
            emit syncError(QStringLiteral("Invalid conference bookmark list"));
            return false;
        }
        ids.insert(room.room().bare());
    }
    saveLegacy(urls, conferences);
    return true;
}

bool BookmarksManager::setConferences(const QList<NativeBookmark> &conferences)
{
    if (!d->online) {
        emit syncError(QStringLiteral("Cannot edit bookmarks while offline"));
        return false;
    }
    QSet<QString> ids;
    for (const auto &room : conferences) {
        if (!room.isValid() || ids.contains(room.room().bare())) {
            emit syncError(QStringLiteral("Invalid or duplicate conference bookmark"));
            return false;
        }
        ids.insert(room.room().bare());
    }

    if (!d->nativeWasActive && !d->nativeActive && !nativeCandidate()) {
        if (!same(d->conferences, conferences))
            saveLegacy(d->urls, conferences);
        return true;
    }

    if (!d->nativeActive) {
        deferEdits(conferences);
        if (d->replica.state() != PubSubReplica<NativeBookmark>::State::Loading)
            fetchNative();
        return true;
    }

    const auto current = d->replica.optimisticItems();
    for (const auto &room : conferences) {
        const QString id  = room.room().bare();
        const auto    old = current.constFind(id);
        // A UI caller may not know the opaque XEP-0402 <extensions/> subtree.
        // Compare known editable fields when deciding whether to publish: a
        // native value without extension data is NOT a request to erase it.
        if (old != current.cend() && sameEditable(old.value(), room))
            continue; // Keep pending edits and any foreign extension data.
        // Preserve opaque <extensions/> when the caller has only the public
        // conference fields (as the Psi GUI does).
        d->replica.put(id, withEditableFields(old != current.cend() ? old.value() : NativeBookmark(), room));
    }
    for (auto it = current.cbegin(); it != current.cend(); ++it) {
        if (ids.contains(it.key()))
            continue;
        if (!d->replica.items().contains(it.key()) && !d->replica.hasInFlightIntent(it.key()))
            d->replica.cancelPending(it.key()); // Undo of unsent addition.
        else
            d->replica.remove(it.key()); // Existing or in-flight addition.
    }
    rebuildNativeConferences(ChangeSource::LocalEdit);
    if (!d->batchingEdits) {
        flushNativeWrites();
        if (!d->nativeActive && d->replica.state() != PubSubReplica<NativeBookmark>::State::Loading)
            fetchNative();
    }
    return true;
}

void BookmarksManager::deferEdits(const QList<NativeBookmark> &desired)
{
    QHash<QString, NativeBookmark> shown;
    for (const auto &room : d->conferences)
        shown.insert(room.room().bare(), room);
    QSet<QString>         ids;
    QList<NativeBookmark> next;
    for (const auto &room : desired) {
        const QString id = room.room().bare();
        ids.insert(id);
        const auto old = shown.constFind(id);
        if (old != shown.cend() && sameEditable(old.value(), room)) {
            next.append(old.value());
            continue;
        }
        const auto updated = withEditableFields(old != shown.cend() ? old.value() : NativeBookmark(), room);
        d->deferredEdits.insert(id, updated);
        next.append(updated);
        emit conferenceUpdated(updated, ChangeSource::LocalEdit);
    }
    for (auto it = shown.cbegin(); it != shown.cend(); ++it)
        if (!ids.contains(it.key())) {
            d->deferredEdits.insert(it.key(), std::nullopt);
            emit conferenceRemoved(it.value().room(), ChangeSource::LocalEdit);
        }
    if (!same(d->conferences, next)) {
        d->conferences = std::move(next);
        emit conferencesChanged();
    }
}

void BookmarksManager::applyDeferredEdits()
{
    if (d->deferredEdits.isEmpty())
        return;
    const auto current = d->replica.optimisticItems();
    for (auto it = d->deferredEdits.cbegin(); it != d->deferredEdits.cend(); ++it) {
        const auto old = current.constFind(it.key());
        if (it.value()) {
            // Publish only a real change against the loaded node, keeping its
            // <extensions/>; a stale legacy copy must not overwrite it.
            if (old == current.cend() || !sameEditable(old.value(), *it.value()))
                d->replica.put(it.key(),
                               withEditableFields(old != current.cend() ? old.value() : NativeBookmark(), *it.value()));
        } else if (old != current.cend()) {
            d->replica.remove(it.key());
        }
    }
    d->deferredEdits.clear();
}

void BookmarksManager::fallBackToLegacy()
{
    d->nativeFallback = true;
    if (d->deferredEdits.isEmpty())
        return;
    // The displayed list already includes the deferred edits.
    d->deferredEdits.clear();
    saveLegacy(d->urls, d->conferences);
}

bool BookmarksManager::upsertConference(const NativeBookmark &conference)
{
    if (!conference.isValid()) {
        emit syncError(QStringLiteral("Invalid conference bookmark"));
        return false;
    }
    QList<NativeBookmark> desired = d->conferences; // Optimistic projection.
    auto                  it = std::find_if(desired.begin(), desired.end(), [&conference](const NativeBookmark &room) {
        return room.room().bare() == conference.room().bare();
    });
    if (it == desired.end())
        desired.append(conference);
    else
        *it = conference;
    return setConferences(desired);
}

bool BookmarksManager::removeConference(const Jid &jid)
{
    if (!jid.isValid() || jid.isEmpty() || !jid.resource().isEmpty()) {
        emit syncError(QStringLiteral("Invalid conference JID"));
        return false;
    }
    auto desired = d->conferences; // Optimistic projection.
    desired.erase(std::remove_if(desired.begin(), desired.end(),
                                 [&jid](const NativeBookmark &room) { return room.room().bare() == jid.bare(); }),
                  desired.end());
    return setConferences(desired);
}

bool BookmarksManager::applyConferenceEdits(const QList<NativeBookmark> &baseline, const QList<NativeBookmark> &edited)
{
    QHash<QString, NativeBookmark> before, after;
    for (const auto &room : baseline) {
        if (!room.isValid() || before.contains(room.room().bare())) {
            emit syncError(QStringLiteral("Invalid original bookmark list"));
            return false;
        }
        before.insert(room.room().bare(), room);
    }
    for (const auto &room : edited) {
        if (!room.isValid() || after.contains(room.room().bare())) {
            emit syncError(QStringLiteral("Invalid edited bookmark list"));
            return false;
        }
        after.insert(room.room().bare(), room);
    }
    auto desired = d->conferences; // Keep newer events and all local intents.
    for (const auto &old : baseline)
        if (!after.contains(old.room().bare()))
            desired.erase(
                std::remove_if(desired.begin(), desired.end(),
                               [&old](const NativeBookmark &b) { return b.room().bare() == old.room().bare(); }),
                desired.end());
    for (const auto &next : edited) {
        const auto old = before.constFind(next.room().bare());
        if (old != before.cend() && same(old.value(), next))
            continue;
        desired.erase(
            std::remove_if(desired.begin(), desired.end(),
                           [&next](const NativeBookmark &b) { return b.room().bare() == next.room().bare(); }),
            desired.end());
        desired.append(next);
    }
    return setConferences(desired);
}

bool BookmarksManager::setUrls(const QList<BookmarkUrl> &urls)
{
    if (!d->online) {
        emit syncError(QStringLiteral("Cannot edit URL bookmarks while offline"));
        return false;
    }
    if (d->nativeWasActive || d->nativeActive || nativeCandidate()) {
        // Native bookmarks and Private XML may be bridged by the server.
        // A Private XML URL-only update can overwrite the entire PEP node,
        // deleting concurrent rooms and extensions. XEP-0402 has no URL
        // items: expose URLs read-only rather than risk data loss.
        emit syncError(QStringLiteral("URL bookmark editing is read-only with XEP-0402"));
        return false;
    }
    if (urls != d->urls)
        saveLegacy(urls, d->conferences);
    return true;
}

void BookmarksManager::saveLegacy(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences)
{
    if (!d->online)
        return;
    if (d->nativeWasActive || d->nativeActive || nativeCandidate()) {
        emit syncError(QStringLiteral("Private XML writes are prohibited with native bookmarks"));
        return;
    }
    // Update the optimistic projection even if an earlier legacy IQ is still
    // outstanding: every subsequent UI action must include the last intent.
    if (!same(d->conferences, conferences)) {
        d->conferences = conferences;
        emit conferencesChanged();
    }
    if (d->urls != urls) {
        d->urls = urls;
        emit urlsChanged();
    }
    if (d->legacyWriting) {
        d->legacyQueued            = true;
        d->queuedLegacyUrls        = urls;
        d->queuedLegacyConferences = conferences;
        return;
    }
    d->legacyWriting = true;
    const auto gen   = d->generation;
    auto      *task  = new PrivateBookmarksTask(d->client->rootTask());
    connect(task, &Task::finished, this, [this, task, gen] {
        if (gen != d->generation || !d->online)
            return;
        d->legacyWriting = false;
        if (!task->success())
            emit syncError(QStringLiteral("Private XML bookmark save failed"));
        if (d->legacyQueued) {
            // A newer full snapshot was already displayed and queued. Do not
            // overwrite it with an acknowledgement of the previous IQ.
            d->legacyQueued         = false;
            const auto pendingUrls  = d->queuedLegacyUrls;
            const auto pendingRooms = d->queuedLegacyConferences;
            saveLegacy(pendingUrls, pendingRooms);
        } else if (task->success()) {
            emit bookmarksSaved();
            maybeEnableNative();
        } else {
            fetchLegacy(); // Read authoritative data again after failure.
        }
    });
    task->set(urls, conferences);
    task->go(true);
}

void BookmarksManager::flushNativeWrites()
{
    if (!d->nativeActive || d->replica.state() != PubSubReplica<NativeBookmark>::State::Ready)
        return;
    const auto batch = d->replica.beginWrite();
    if (!batch.isValid())
        return;
    using Result            = PubSubReplica<NativeBookmark>::WriteResult;
    using Results           = PubSubReplica<NativeBookmark>::WriteResults;
    auto       remaining    = std::make_shared<int>(batch.changes.size());
    auto       results      = std::make_shared<Results>();
    auto       repairNeeded = std::make_shared<bool>(false);
    auto       repairIds    = std::make_shared<QStringList>();
    const auto generation   = d->generation;

    for (auto it = batch.changes.cbegin(); it != batch.changes.cend(); ++it) {
        const QString id   = it.key();
        Task         *task = nullptr;
        if (it.value()) {
            QDomDocument xml;
            task = d->client->pubSubManager()->publish(Jid(), NativeBookmark::node(), it.value()->toItem(xml),
                                                       NativeBookmark::privatePublishOptions());
        } else {
            task = d->client->pubSubManager()->retract(Jid(), NativeBookmark::node(), id);
        }
        connect(
            task, &Task::finished, this,
            [this, task, id, batch, generation, remaining, results, repairNeeded, repairIds] {
                if (generation != d->generation)
                    return;
                if (task->success()) {
                    results->insert(id, Result::Success);
                    d->itemRetries.remove(id);
                } else {
                    const auto &error        = task->error();
                    const bool  precondition = error.appSpec.tagName() == QLatin1String("precondition-not-met")
                        && error.appSpec.namespaceURI() == QLatin1String("http://jabber.org/protocol/pubsub#errors");
                    const bool retryable = task->statusCode() == Task::ErrTimeout || task->statusCode() == Task::ErrDisc
                        || error.isWait() || error.condition == Stanza::Error::ErrorCond::RemoteServerTimeout
                        || error.condition == Stanza::Error::ErrorCond::ServiceUnavailable;
                    if (precondition && !d->nodeRepairAttempted) {
                        *repairNeeded = true;
                        repairIds->append(id);
                        results->insert(id, Result::Retry);
                    } else if (retryable && ++d->itemRetries[id] <= 3) {
                        results->insert(id, Result::Retry);
                    } else {
                        results->insert(id, Result::Rejected);
                        d->itemRetries.remove(id);
                        emit syncError(QStringLiteral("Bookmark %1 was rejected: %2").arg(id, error.toString()));
                    }
                }
                if (--*remaining != 0)
                    return;
                if (!d->replica.finishWrite(batch, *results))
                    return; // Discard callbacks invalidated by reconnect/purge.

                rebuildNativeConferences(ChangeSource::Acknowledged);
                const bool hasRetry = d->replica.state() == PubSubReplica<NativeBookmark>::State::ResyncRequired;
                if (*repairNeeded) {
                    d->nodeRepairAttempted = true;
                    d->nativeActive        = false;
                    // XEP-0060 publish-options precondition failed. Attempt a
                    // single owner configuration repair; never loop indefinitely.
                    auto *config = d->client->pubSubManager()->configureNode(Jid(), NativeBookmark::node(),
                                                                             NativeBookmark::privatePublishOptions());
                    connect(config, &Task::finished, this, [this, config, generation, repairIds] {
                        if (generation != d->generation)
                            return;
                        if (!config->success()) {
                            for (const auto &id : *repairIds)
                                d->replica.cancelPending(id);
                            emit syncError(QStringLiteral("Bookmark PEP node configuration could not be repaired"));
                        }
                        // Re-fetch after changing the node config or discovering
                        // that the pending item must be discarded.
                        scheduleNativeRetry();
                    });
                    config->go(true);
                    return;
                }
                if (hasRetry) {
                    d->nativeActive = false;
                    scheduleNativeRetry(); // Bounded exponential delay, not busy-loop.
                    return;
                }
                // Rejected items have been isolated; successful unrelated edits
                // are no longer blocked and may proceed normally.
                if (results->size() == batch.changes.size()
                    && std::all_of(results->cbegin(), results->cend(),
                                   [](Result result) { return result == Result::Success; }))
                    emit bookmarksSaved();
                flushNativeWrites();
            });
        task->go(true);
    }
}

} // namespace XMPP
