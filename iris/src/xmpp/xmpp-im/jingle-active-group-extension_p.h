/*
 * jingle-active-group-extension_p.h - active Jingle group extension validation
 * Copyright (C) 2026  Psi contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef JINGLE_ACTIVE_GROUP_EXTENSION_P_H
#define JINGLE_ACTIVE_GROUP_EXTENSION_P_H

#include "jingle-session.h"

#include <QSet>

namespace XMPP { namespace Jingle { namespace ActiveGroupExtension {

    enum class Kind { Unchanged, Extension, Invalid };

    struct Result {
        Kind      kind = Kind::Invalid;
        QString   addedName;
        qsizetype groupIndex = -1;
    };

    inline bool same(const QList<ContentGroup> &left, const QList<ContentGroup> &right)
    {
        if (left.size() != right.size())
            return false;
        for (qsizetype i = 0; i < left.size(); ++i) {
            if (left.at(i).semantics != right.at(i).semantics || left.at(i).contents != right.at(i).contents)
                return false;
        }
        return true;
    }

    // Each supplied group describes its complete membership. Unmentioned
    // groups retain their committed membership; group-list order is not a
    // transport identity. Only one new content may extend one existing BUNDLE.
    inline Result classify(const QList<ContentGroup> &committed, const QList<ContentGroup> &proposed,
                           const QSet<QString> &stanzaContents)
    {
        Result          result { Kind::Unchanged, {}, -1 };
        QSet<qsizetype> updated;
        for (const auto &after : proposed) {
            qsizetype index     = -1;
            bool      extension = false;
            for (qsizetype i = 0; i < committed.size(); ++i) {
                const auto &before = committed.at(i);
                if (before.semantics != after.semantics)
                    continue;
                bool sameMembers = before.contents == after.contents;
                bool append      = before.semantics == QLatin1String("BUNDLE") && !before.contents.isEmpty()
                    && after.contents.size() == before.contents.size() + 1;
                for (qsizetype member = 0; append && member < before.contents.size(); ++member)
                    append = before.contents.at(member) == after.contents.at(member);
                if (!sameMembers && !append)
                    continue;
                if (index >= 0)
                    return {}; // ambiguous group identity
                index     = i;
                extension = append;
            }
            if (index < 0 || updated.contains(index))
                return {};
            updated.insert(index);
            if (!extension)
                continue;
            const auto added = after.contents.last();
            if (result.kind == Kind::Extension || added.isEmpty() || !stanzaContents.contains(added))
                return {};
            for (const auto &before : committed) {
                if (before.semantics == QLatin1String("BUNDLE") && before.contents.contains(added))
                    return {};
            }
            result = { Kind::Extension, added, index };
        }
        return result;
    }

    inline QList<ContentGroup> extended(const QList<ContentGroup> &committed, const Result &change)
    {
        auto result = committed;
        if (change.kind == Kind::Extension)
            result[change.groupIndex].contents.append(change.addedName);
        return result;
    }

}}} // namespace XMPP::Jingle::ActiveGroupExtension

#endif // JINGLE_ACTIVE_GROUP_EXTENSION_P_H
