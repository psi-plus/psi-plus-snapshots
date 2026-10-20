// SPDX-License-Identifier: LGPL-2.1-or-later
#include "fixture.h"
#include <iris/xmpp_bookmarkmanager.h>
#include <QCoreApplication>
#include <QtCrypto>

using namespace XMPP;
using namespace MessageTest;

// Ensure the public protocol manager can be constructed independently of
// PsiAccount, and that disconnect resets availability without side effects.
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCA::Initializer qca;
    Endpoint ep;
    BookmarksManager bookmarks(&ep.client);
    check(!bookmarks.isAvailable(), "New bookmark manager should be offline");
    check(!bookmarks.usesNativeBookmarks(), "New bookmark manager unexpectedly using PEP");
    check(bookmarks.conferences().isEmpty() && bookmarks.urls().isEmpty(),
          "New bookmark manager was not empty");

    bookmarks.setConferences({ NativeBookmark::create(Jid("room@conference.example.test"), "A", true) });
    bookmarks.setUrls({ { QStringLiteral("Link"), QStringLiteral("https://example.test") } });
    check(bookmarks.conferences().isEmpty() && bookmarks.urls().isEmpty(),
          "Offline requests unexpectedly mutated shared bookmark state");
    bookmarks.setOnline(false); // idempotent, no network traffic.
    return 0;
}
