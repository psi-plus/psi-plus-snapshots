/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef XMPP_BOOKMARKS_H
#define XMPP_BOOKMARKS_H

#include <iris/xmpp-im/xmpp_pubsub.h>
#include <iris/xmpp_jid.h>

namespace XMPP {

// XEP-0402 PEP bookmark value. Serialize known fields only when publishing;
// retain just the optional opaque <extensions/> element for roundtripping.
class IRIS_EXPORT NativeBookmark {
public:
    static QString node();
    static PubSubOptions privatePublishOptions();

    static NativeBookmark fromItem(const PubSubItem &item);
    bool isValid() const;
    Jid room() const;
    QString name() const;
    QString nick() const;
    QString password() const;
    bool autoJoin() const;
    bool hasSamePayload(const NativeBookmark &other) const;
    void setName(const QString &);
    void setNick(const QString &);
    void setPassword(const QString &);
    void setAutoJoin(bool);
    PubSubItem toItem(QDomDocument &document) const;

    static NativeBookmark create(const Jid &room, const QString &name, bool autoJoin,
                                 const QString &nick = {}, const QString &password = {});
private:
    Jid          room_;
    QString      name_;
    QString      nick_;
    QString      password_;
    bool         autoJoin_ = false;
    QDomDocument extensions_; // Optional <extensions/> subtree; never the whole conference.
};
} // namespace XMPP
#endif // XMPP_BOOKMARKS_H
