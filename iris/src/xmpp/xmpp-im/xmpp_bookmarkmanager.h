/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef XMPP_BOOKMARKMANAGER_H
#define XMPP_BOOKMARKMANAGER_H

#include <iris/iris_export.h>
#include <iris/xmpp-im/xmpp_bookmarks.h>

#include <QList>
#include <QObject>
#include <QString>
#include <memory>

namespace XMPP {
class Client;

/** XEP-0048 private-XML URL bookmark (not expressible in XEP-0402). */
struct BookmarkUrl {
    QString name;
    QString url;
    bool    operator==(const BookmarkUrl &o) const { return name == o.name && url == o.url; }
};

/**
 * Shared bookmark protocol manager. Owns all network traffic, server feature
 * selection, PEP item reconciliation, and legacy private-XML compatibility.
 * Does not know about Qt widgets or client-local MUC auto-join policies.
 *
 * A host explicitly reports its authenticated session availability via
 * setOnline(). That makes the lifecycle unambiguous across reconnects.
 */
class IRIS_EXPORT BookmarksManager : public QObject {
    Q_OBJECT
public:
    explicit BookmarksManager(Client *client, QObject *parent = nullptr);
    ~BookmarksManager() override;

    bool                         isAvailable() const;
    bool                         usesNativeBookmarks() const;
    const QList<NativeBookmark> &conferences() const;
    const QList<BookmarkUrl>    &urls() const;

    void setOnline(bool online);
    // Replace both legacy collections with one IQ; native PEP conferences
    // remain item-scoped while URLs still use Private XML.
    bool setBookmarks(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences);
    bool setConferences(const QList<NativeBookmark> &conferences);
    bool upsertConference(const NativeBookmark &conference);
    bool removeConference(const Jid &room);
    // Intent-based edits: unrelated PEP changes made after baseline are kept.
    bool applyConferenceEdits(const QList<NativeBookmark> &baseline, const QList<NativeBookmark> &edited);
    bool setUrls(const QList<BookmarkUrl> &urls);

public:
    enum class ChangeSource { Snapshot, Push, LocalEdit, Acknowledged };
    Q_ENUM(ChangeSource)

signals:
    void availabilityChanged(bool available);
    void conferencesChanged();
    void conferenceUpdated(const XMPP::NativeBookmark &bookmark, ChangeSource source);
    void conferenceRemoved(const XMPP::Jid &room, ChangeSource source);
    void urlsChanged();
    void bookmarksSaved();
    void syncError(const QString &message);

private:
    void fetchLegacy();
    void maybeEnableNative();
    bool nativeCandidate() const;
    void fetchNative();
    void rebuildNativeConferences(ChangeSource source = ChangeSource::Snapshot);
    void flushNativeWrites();
    void saveLegacy(const QList<BookmarkUrl> &urls, const QList<NativeBookmark> &conferences);
    void deferEdits(const QList<NativeBookmark> &desired);
    void applyDeferredEdits();
    void fallBackToLegacy();
    void scheduleNativeRetry();
    void setAvailable(bool available);

    class Private;
    std::unique_ptr<Private> d;
};
} // namespace XMPP
#endif // XMPP_BOOKMARKMANAGER_H
