/*
 * jingle-sxe-document.cpp - Shared XML Editing (XEP-0284) document model
 * Copyright (C) 2006  Joonas Govenius
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

#include "jingle-sxe-document.h"

#include <QHash>
#include <QLocale>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QTextStream>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <optional>

namespace XMPP { namespace Jingle { namespace SXE {
    const QString NS(QStringLiteral("urn:xmpp:sxe:0"));

    namespace {
        enum class RecordType { Element, Attr, Text, Comment, ProcessingInstruction };

        std::optional<RecordType> typeFromString(const QString &type)
        {
            if (type == QLatin1String("element"))
                return RecordType::Element;
            if (type == QLatin1String("attr"))
                return RecordType::Attr;
            if (type == QLatin1String("text"))
                return RecordType::Text;
            if (type == QLatin1String("comment"))
                return RecordType::Comment;
            if (type == QLatin1String("processinginstruction"))
                return RecordType::ProcessingInstruction;
            return std::nullopt;
        }

        QString typeToString(RecordType type)
        {
            switch (type) {
            case RecordType::Element:
                return QStringLiteral("element");
            case RecordType::Attr:
                return QStringLiteral("attr");
            case RecordType::Text:
                return QStringLiteral("text");
            case RecordType::Comment:
                return QStringLiteral("comment");
            case RecordType::ProcessingInstruction:
                return QStringLiteral("processinginstruction");
            }
            return {};
        }

        std::optional<RecordType> typeOfNode(const QDomNode &node)
        {
            switch (node.nodeType()) {
            case QDomNode::ElementNode:
                return RecordType::Element;
            case QDomNode::AttributeNode:
                return RecordType::Attr;
            case QDomNode::TextNode:
            case QDomNode::CDATASectionNode:
                return RecordType::Text;
            case QDomNode::CommentNode:
                return RecordType::Comment;
            case QDomNode::ProcessingInstructionNode:
                return RecordType::ProcessingInstruction;
            default:
                return std::nullopt;
            }
        }

        bool hasName(RecordType t) { return t == RecordType::Element || t == RecordType::Attr; }
        bool hasChdata(RecordType t)
        {
            return t == RecordType::Attr || t == RecordType::Text || t == RecordType::Comment;
        }

        // Conservative XML QName check. QDom accepts invalid names by default and
        // would serialize malformed XML from untrusted record names.
        bool isNameStart(QChar c) { return c.isLetter() || c == QLatin1Char('_'); }
        bool isNameChar(QChar c)
        {
            return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.')
                || c.category() == QChar::Mark_NonSpacing || c.category() == QChar::Mark_SpacingCombining;
        }
        bool isValidNCName(QStringView name)
        {
            if (name.isEmpty() || !isNameStart(name.front()))
                return false;
            return std::all_of(name.begin() + 1, name.end(), isNameChar);
        }
        bool isValidQName(const QString &name)
        {
            const auto colon = name.indexOf(QLatin1Char(':'));
            if (colon < 0)
                return isValidNCName(name);
            return isValidNCName(QStringView(name).left(colon)) && isValidNCName(QStringView(name).mid(colon + 1));
        }
        QString localName(const QString &qname)
        {
            const auto colon = qname.indexOf(QLatin1Char(':'));
            return colon < 0 ? qname : qname.mid(colon + 1);
        }

        double parseWeight(const QString &value)
        {
            bool       ok     = false;
            const auto weight = value.toDouble(&ok);
            return ok && std::isfinite(weight) ? weight : 0.0;
        }

        QString weightToString(double weight) { return QString::number(weight, 'g', QLocale::FloatingPointShortest); }

        // XEP-0284 secondary weight: the first differing character of the rids decides.
        bool secondaryLess(const QString &a, const QString &b)
        {
            const auto n = std::min(a.size(), b.size());
            for (std::decay_t<decltype(n)> i = 0; i < n; ++i) {
                if (a[i] != b[i])
                    return a[i].unicode() < b[i].unicode();
            }
            return a.size() < b.size();
        }

        struct Fields {
            QString parent;
            double  primaryWeight = 0.0;
            QString ns;
            QString name;   // element/attr name or processing instruction target
            QString chdata; // character data or processing instruction data
        };

        struct Record {
            QString    rid;
            RecordType type = RecordType::Element;
            QDomNode   node;
            int        version = 0;
            // Field values keyed by the version they were set at. The fields of
            // version v are those of the greatest key <= v.
            QMap<int, Fields> states;

            const Fields &fields() const { return states.last(); }
            Fields        fieldsAt(int version) const
            {
                auto it = states.upperBound(version);
                if (it == states.begin())
                    return states.first();
                return (--it).value();
            }
        };

        bool weightLess(const Record &a, const Record &b)
        {
            if (a.fields().primaryWeight != b.fields().primaryWeight)
                return a.fields().primaryWeight < b.fields().primaryWeight;
            return secondaryLess(a.rid, b.rid);
        }

        // QDomAttr::ownerElement() dereferences a null parent for a detached attribute.
        QDomElement ownerOf(const QDomNode &attr)
        {
            const auto parent = attr.parentNode();
            return parent.isElement() ? parent.toElement() : QDomElement();
        }

        void detachNode(QDomNode node)
        {
            if (node.isAttr()) {
                auto owner = ownerOf(node);
                if (!owner.isNull())
                    owner.removeAttributeNode(node.toAttr());
                return;
            }
            auto parent = node.parentNode();
            if (!parent.isNull())
                parent.removeChild(node);
        }
    } // namespace

    class Document::Private {
    public:
        Document *q;

        QDomDocument                  doc;
        QString                       prolog;
        QHash<QString, Record *>      records;
        QHash<QString, QSet<QString>> children; // parent rid -> child rids (attributes included)
        QSet<QString>                 removed;  // rids must never be reused within a session

        QDomDocument       editsDoc;
        QList<QDomElement> outgoing;
        bool               importing     = false;
        bool               readOnly      = false;
        bool               suppressQueue = false;
        bool               changed       = false;

        QString ridPrefix;
        quint64 ridCounter = 0;

        explicit Private(Document *q) : q(q), ridPrefix(Document::generateUUID().remove(QLatin1Char('-')).left(12)) { }
        ~Private() { qDeleteAll(records); }

        void clear()
        {
            qDeleteAll(records);
            records.clear();
            children.clear();
            removed.clear();
            outgoing.clear();
            doc = QDomDocument();
        }

        QString newRid()
        {
            QString rid;
            do {
                rid = QStringLiteral("%1.%2").arg(ridPrefix, QString::number(++ridCounter, 36));
            } while (records.contains(rid) || removed.contains(rid));
            return rid;
        }

        Record *recordOf(const QDomNode &node) const
        {
            if (node.isNull())
                return nullptr;
            for (auto r : records) {
                if (r->node == node)
                    return r;
            }
            return nullptr;
        }

        void queue(const QDomElement &edit)
        {
            if (!importing && !suppressQueue && !readOnly)
                outgoing.append(edit);
        }

        bool isAncestorOrSelf(const QString &ancestor, QString rid) const
        {
            for (int guard = 0; guard <= records.size() && !rid.isEmpty(); ++guard) {
                if (rid == ancestor)
                    return true;
                auto r = records.value(rid);
                if (!r)
                    return false;
                rid = r->fields().parent;
            }
            return false;
        }

        QDomNode createNode(RecordType type, const Fields &f)
        {
            switch (type) {
            case RecordType::Element:
                return f.ns.isEmpty() ? doc.createElement(f.name) : doc.createElementNS(f.ns, f.name);
            case RecordType::Attr: {
                auto attr = f.ns.isEmpty() ? doc.createAttribute(f.name) : doc.createAttributeNS(f.ns, f.name);
                attr.setValue(f.chdata);
                return attr;
            }
            case RecordType::Text:
                return doc.createTextNode(f.chdata);
            case RecordType::Comment:
                return doc.createComment(f.chdata);
            case RecordType::ProcessingInstruction:
                return doc.createProcessingInstruction(f.name, f.chdata);
            }
            return {};
        }

        // Removes the record, its descendants and their DOM nodes. Children are
        // removed first; optionally <remove/> edits are queued for them (and self).
        void deleteRecord(const QString &rid, bool remote, bool queueChildren, bool queueSelf)
        {
            auto r = records.value(rid);
            if (!r)
                return;
            const auto childRids = children.value(rid);
            for (const auto &child : childRids)
                deleteRecord(child, remote, queueChildren, queueChildren);

            if (queueSelf) {
                auto edit = editsDoc.createElementNS(NS, QStringLiteral("remove"));
                edit.setAttribute(QStringLiteral("target"), rid);
                queue(edit);
            }

            const bool inDom
                = !r->node.isNull() && (r->node.isAttr() ? !ownerOf(r->node).isNull() : !r->node.parentNode().isNull());
            if (inDom)
                emit q->nodeToBeRemoved(r->node, remote);
            detachNode(r->node);
            records.remove(rid);
            removed.insert(rid);
            children.remove(rid);
            auto pit = children.find(r->fields().parent);
            if (pit != children.end()) {
                pit->remove(rid);
                if (pit->isEmpty())
                    children.erase(pit);
            }
            const auto node = r->node;
            delete r;
            if (inDom)
                emit q->nodeRemoved(node, remote);
            changed = true;
        }

        enum class Placement { Placed, Deleted };

        // Puts the record's node where its fields say. Records violating the
        // XEP-0284 DOM mapping rules (orphans, duplicate roots or attributes)
        // are removed deterministically on every endpoint without sending edits.
        Placement place(Record *r, bool remote, bool adding)
        {
            const Fields &f   = r->fields();
            const QString rid = r->rid;
            QDomNode      container;
            if (f.parent.isEmpty()) {
                if (r->type == RecordType::Attr || r->type == RecordType::Text) {
                    deleteRecord(rid, remote, false, false);
                    return Placement::Deleted;
                }
                container = doc;
                if (r->type == RecordType::Element) {
                    auto root = doc.documentElement();
                    if (!root.isNull() && root != r->node) {
                        auto other = recordOf(root);
                        if (other && !secondaryLess(rid, other->rid)) {
                            deleteRecord(other->rid, remote, false, false);
                        } else {
                            deleteRecord(rid, remote, false, false);
                            return Placement::Deleted;
                        }
                    }
                }
            } else {
                auto parent = records.value(f.parent);
                if (!parent || parent->type != RecordType::Element) {
                    deleteRecord(rid, remote, false, false);
                    return Placement::Deleted;
                }
                container = parent->node;
            }

            if (r->type == RecordType::Attr) {
                auto element = container.toElement();
                auto existing
                    = f.ns.isEmpty() ? element.attributeNode(f.name) : element.attributeNodeNS(f.ns, localName(f.name));
                if (!existing.isNull() && existing != r->node) {
                    auto other = recordOf(existing);
                    if (other && secondaryLess(rid, other->rid)) {
                        deleteRecord(rid, remote, false, false);
                        return Placement::Deleted;
                    }
                    if (other)
                        deleteRecord(other->rid, remote, false, false);
                    else
                        element.removeAttributeNode(existing);
                }
                emit adding ? q->nodeToBeAdded(r->node, remote) : q->nodeToBeMoved(r->node, remote);
                if (!records.contains(rid))
                    return Placement::Deleted; // a signal handler removed it
                auto attr  = r->node.toAttr();
                auto owner = ownerOf(attr);
                if (!owner.isNull() && owner != element)
                    owner.removeAttributeNode(attr);
                if (f.ns.isEmpty())
                    element.setAttributeNode(attr);
                else
                    element.setAttributeNodeNS(attr);
                emit adding ? q->nodeAdded(r->node, remote) : q->nodeMoved(r->node, remote);
                changed = true;
                return Placement::Placed;
            }

            emit adding ? q->nodeToBeAdded(r->node, remote) : q->nodeToBeMoved(r->node, remote);
            if (!records.contains(rid))
                return Placement::Deleted;
            QDomNode before;
            for (auto child = container.firstChild(); !child.isNull(); child = child.nextSibling()) {
                if (child == r->node)
                    continue;
                auto sibling = recordOf(child);
                if (sibling && weightLess(*r, *sibling)) {
                    before = child;
                    break;
                }
            }
            if (before.isNull())
                container.appendChild(r->node);
            else
                container.insertBefore(r->node, before);
            emit adding ? q->nodeAdded(r->node, remote) : q->nodeMoved(r->node, remote);
            changed = true;
            return Placement::Placed;
        }

        void applyNew(const QDomElement &e, bool remote)
        {
            const auto rid  = e.attribute(QStringLiteral("rid"));
            const auto type = typeFromString(e.attribute(QStringLiteral("type")));
            if (rid.isEmpty() || !type || records.contains(rid) || removed.contains(rid))
                return; // duplicates and reused rids are ignored

            Fields f;
            f.parent        = e.attribute(QStringLiteral("parent"));
            f.primaryWeight = parseWeight(e.attribute(QStringLiteral("primary-weight")));
            if (hasName(*type)) {
                f.ns   = e.attribute(QStringLiteral("ns"));
                f.name = e.attribute(QStringLiteral("name"));
                if (!isValidQName(f.name))
                    return;
            } else if (*type == RecordType::ProcessingInstruction) {
                f.name   = e.attribute(QStringLiteral("pitarget"));
                f.chdata = e.attribute(QStringLiteral("pidata"));
                if (!isValidNCName(f.name) || f.name.compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0)
                    return;
            }
            if (hasChdata(*type))
                f.chdata = e.attribute(QStringLiteral("chdata"));

            bool ok      = false;
            int  version = e.attribute(QStringLiteral("version")).toInt(&ok);
            if (!ok || version < 0)
                version = 0;

            auto r     = new Record;
            r->rid     = rid;
            r->type    = *type;
            r->version = version;
            r->states.insert(version, f);
            r->node = createNode(*type, f);
            records.insert(rid, r);
            children[f.parent].insert(rid);
            place(r, remote, true);
        }

        void applySet(const QDomElement &e, bool remote)
        {
            auto r = records.value(e.attribute(QStringLiteral("target")));
            if (!r)
                return;
            bool ok      = false;
            int  version = e.attribute(QStringLiteral("version")).toInt(&ok);
            if (!ok || version < 0)
                return;

            const Fields previous = r->fields();
            if (importing)
                r->version = version;
            else
                ++r->version;

            Fields next;
            if (r->version == version) {
                next = previous;
                if (e.hasAttribute(QStringLiteral("parent")))
                    next.parent = e.attribute(QStringLiteral("parent"));
                if (e.hasAttribute(QStringLiteral("primary-weight")))
                    next.primaryWeight = parseWeight(e.attribute(QStringLiteral("primary-weight")));
                if (hasName(r->type)) {
                    if (e.hasAttribute(QStringLiteral("ns")))
                        next.ns = e.attribute(QStringLiteral("ns"));
                    if (e.hasAttribute(QStringLiteral("name")) && isValidQName(e.attribute(QStringLiteral("name"))))
                        next.name = e.attribute(QStringLiteral("name"));
                } else if (r->type == RecordType::ProcessingInstruction) {
                    if (e.hasAttribute(QStringLiteral("pitarget"))
                        && isValidNCName(e.attribute(QStringLiteral("pitarget"))))
                        next.name = e.attribute(QStringLiteral("pitarget"));
                    if (e.hasAttribute(QStringLiteral("pidata")))
                        next.chdata = e.attribute(QStringLiteral("pidata"));
                }
                if (hasChdata(r->type) && e.hasAttribute(QStringLiteral("chdata"))) {
                    const auto chdata = e.attribute(QStringLiteral("chdata"));
                    if (e.hasAttribute(QStringLiteral("replacefrom")) && e.hasAttribute(QStringLiteral("replacen"))) {
                        bool       okFrom = false, okN = false;
                        const auto from = e.attribute(QStringLiteral("replacefrom")).toUInt(&okFrom);
                        const auto n    = e.attribute(QStringLiteral("replacen")).toUInt(&okN);
                        if (okFrom && okN && quint64(from) + n <= quint64(next.chdata.size()))
                            next.chdata.replace(from, n, chdata);
                    } else {
                        next.chdata = chdata;
                    }
                }
            } else {
                // Conflicting edit: every field reverts to version (v - 1).
                next = r->fieldsAt(version - 1);
            }

            // A record can't become its own ancestor; keep the old parent then.
            if (next.parent != previous.parent && !next.parent.isEmpty() && isAncestorOrSelf(r->rid, next.parent))
                next.parent = previous.parent;

            // History may rewind the version while importing; later states are void then.
            while (!r->states.isEmpty() && r->states.lastKey() > r->version)
                r->states.erase(std::prev(r->states.end()));
            r->states.insert(r->version, next);
            updateNode(r, previous, remote);
        }

        void updateNode(Record *r, const Fields &previous, bool remote)
        {
            const Fields  next = r->fields();
            const QString rid  = r->rid;

            if (next.parent != previous.parent) {
                auto pit = children.find(previous.parent);
                if (pit != children.end()) {
                    pit->remove(rid);
                    if (pit->isEmpty())
                        children.erase(pit);
                }
                children[next.parent].insert(rid);
            }

            if (next.name != previous.name || next.ns != previous.ns) {
                // DOM names are immutable: rebuild the node, keeping its subtree.
                const auto old   = r->node;
                const bool inDom = old.isAttr() ? !ownerOf(old).isNull() : !old.parentNode().isNull();
                if (inDom)
                    emit q->nodeToBeRemoved(old, remote);
                if (!records.contains(rid))
                    return;
                detachNode(old);
                if (inDom)
                    emit q->nodeRemoved(old, remote);
                if (!records.contains(rid))
                    return;
                auto fresh = createNode(r->type, next);
                if (r->type == RecordType::Element) {
                    while (!old.firstChild().isNull())
                        fresh.appendChild(old.firstChild());
                    auto attrs = old.attributes();
                    while (attrs.count() > 0) {
                        auto attr = attrs.item(0).toAttr();
                        old.toElement().removeAttributeNode(attr);
                        if (attr.namespaceURI().isEmpty())
                            fresh.toElement().setAttributeNode(attr);
                        else
                            fresh.toElement().setAttributeNodeNS(attr);
                    }
                }
                r->node = fresh;
                place(r, remote, true);
                return;
            }

            if (next.parent != previous.parent || next.primaryWeight != previous.primaryWeight) {
                if (place(r, remote, false) == Placement::Deleted)
                    return;
            }

            if (next.chdata != previous.chdata && records.contains(rid)) {
                emit q->chdataToBeChanged(r->node, remote);
                if (!records.contains(rid))
                    return;
                if (r->type == RecordType::ProcessingInstruction)
                    r->node.toProcessingInstruction().setData(next.chdata);
                else
                    r->node.setNodeValue(next.chdata);
                changed = true;
                emit q->chdataChanged(r->node, remote);
            }
        }

        void applyRemove(const QDomElement &e, bool remote)
        {
            const auto rid = e.attribute(QStringLiteral("target"));
            if (!records.contains(rid))
                return;
            // XEP-0284: the receiver of a <remove/> for a node with children sends
            // <remove/> elements for them. Nothing is sent while importing a state.
            deleteRecord(rid, remote, true, false);
        }

        QDomElement makeNew(QDomDocument &target, const Record *r, const Fields &f, const QString &chdata,
                            int version) const
        {
            auto edit = target.createElementNS(NS, QStringLiteral("new"));
            edit.setAttribute(QStringLiteral("rid"), r->rid);
            edit.setAttribute(QStringLiteral("type"), typeToString(r->type));
            if (version > 0)
                edit.setAttribute(QStringLiteral("version"), version);
            if (!f.parent.isEmpty())
                edit.setAttribute(QStringLiteral("parent"), f.parent);
            if (f.primaryWeight != 0.0)
                edit.setAttribute(QStringLiteral("primary-weight"), weightToString(f.primaryWeight));
            if (hasName(r->type)) {
                if (!f.ns.isEmpty())
                    edit.setAttribute(QStringLiteral("ns"), f.ns);
                edit.setAttribute(QStringLiteral("name"), f.name);
            }
            if (hasChdata(r->type))
                edit.setAttribute(QStringLiteral("chdata"), chdata);
            if (r->type == RecordType::ProcessingInstruction) {
                edit.setAttribute(QStringLiteral("pitarget"), f.name);
                edit.setAttribute(QStringLiteral("pidata"), chdata);
            }
            return edit;
        }

        QDomElement makeAppend(QDomDocument &target, const QString &rid, int version, int from, const QString &chunk)
        {
            auto edit = target.createElementNS(NS, QStringLiteral("set"));
            edit.setAttribute(QStringLiteral("target"), rid);
            edit.setAttribute(QStringLiteral("version"), version);
            edit.setAttribute(QStringLiteral("replacefrom"), from);
            edit.setAttribute(QStringLiteral("replacen"), 0);
            edit.setAttribute(QStringLiteral("chdata"), chunk);
            return edit;
        }

        // Creates records for a (possibly foreign) node and its subtree as local edits.
        QDomNode generateNew(const QDomNode &node, const QString &parentRid, double weight)
        {
            const auto type = typeOfNode(node);
            if (!type)
                return {};
            Fields f;
            f.parent        = parentRid;
            f.primaryWeight = weight;
            if (hasName(*type)) {
                f.ns   = node.namespaceURI();
                f.name = node.nodeName();
            }
            if (hasChdata(*type))
                f.chdata = node.nodeValue();
            if (*type == RecordType::ProcessingInstruction) {
                f.name   = node.toProcessingInstruction().target();
                f.chdata = node.toProcessingInstruction().data();
            }

            Record probe;
            probe.rid        = newRid();
            probe.type       = *type;
            const bool split = hasChdata(*type) && f.chdata.size() > Document::MaxChdataPerEdit;
            auto edit = makeNew(editsDoc, &probe, f, split ? f.chdata.left(Document::MaxChdataPerEdit) : f.chdata, 0);
            applyNew(edit, false);
            if (!records.contains(probe.rid))
                return {};
            queue(edit);
            if (split) {
                for (int from = Document::MaxChdataPerEdit; from < f.chdata.size();
                     from += Document::MaxChdataPerEdit) {
                    auto r      = records.value(probe.rid);
                    auto append = makeAppend(editsDoc, probe.rid, r->version + 1, from,
                                             f.chdata.mid(from, Document::MaxChdataPerEdit));
                    applySet(append, false);
                    queue(append);
                    if (!records.contains(probe.rid))
                        return {};
                }
            }

            if (*type == RecordType::Element) {
                const auto attrs = node.attributes();
                for (int i = 0; i < attrs.count(); ++i)
                    generateNew(attrs.item(i), probe.rid, i);
                int i = 0;
                for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling())
                    generateNew(child, probe.rid, i++);
            }
            auto r = records.value(probe.rid);
            return r ? r->node : QDomNode();
        }

        void localSet(Record *r, const QList<std::pair<QString, QString>> &changes)
        {
            auto edit = editsDoc.createElementNS(NS, QStringLiteral("set"));
            edit.setAttribute(QStringLiteral("target"), r->rid);
            edit.setAttribute(QStringLiteral("version"), r->version + 1);
            for (const auto &[key, value] : changes)
                edit.setAttribute(key, value);
            queue(edit);
            applySet(edit, false);
        }

        QDomNode insertNode(const QDomNode &node, const QString &parentRid, double weight)
        {
            if (auto r = recordOf(node)) {
                QList<std::pair<QString, QString>> changes;
                if (r->fields().parent != parentRid)
                    changes.append({ QStringLiteral("parent"), parentRid });
                if (r->fields().primaryWeight != weight)
                    changes.append({ QStringLiteral("primary-weight"), weightToString(weight) });
                if (!changes.isEmpty())
                    localSet(r, changes);
                return node;
            }
            return generateNew(node, parentRid, weight);
        }

        void collectSnapshot(QDomDocument &target, const QString &parent, QList<QDomElement> &out) const
        {
            QList<const Record *> ordered;
            for (const auto &rid : children.value(parent)) {
                if (auto r = records.value(rid))
                    ordered.append(r);
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const Record *a, const Record *b) { return weightLess(*a, *b); });
            for (auto r : ordered) {
                const auto &f     = r->fields();
                const bool  split = hasChdata(r->type) && f.chdata.size() > Document::MaxChdataPerEdit;
                out.append(
                    makeNew(target, r, f, split ? f.chdata.left(Document::MaxChdataPerEdit) : f.chdata, r->version));
                if (split) {
                    // History <set/> elements carry the final version, so chunks
                    // reproduce the sender's version instead of advancing it.
                    for (int from = Document::MaxChdataPerEdit; from < f.chdata.size();
                         from += Document::MaxChdataPerEdit) {
                        auto edit = target.createElementNS(NS, QStringLiteral("set"));
                        edit.setAttribute(QStringLiteral("target"), r->rid);
                        edit.setAttribute(QStringLiteral("version"), r->version);
                        edit.setAttribute(QStringLiteral("replacefrom"), from);
                        edit.setAttribute(QStringLiteral("replacen"), 0);
                        edit.setAttribute(QStringLiteral("chdata"), f.chdata.mid(from, Document::MaxChdataPerEdit));
                        out.append(edit);
                    }
                }
                collectSnapshot(target, r->rid, out);
            }
        }

        void finishLocalChange()
        {
            if (changed) {
                changed = false;
                emit q->documentUpdated(false);
            }
        }
    };

    Document::Document(QObject *parent) : QObject(parent), d(std::make_unique<Private>(this)) { }

    Document::~Document() = default;

    void Document::reset(const QDomDocument &source)
    {
        d->clear();
        d->prolog        = prologOf(source);
        d->suppressQueue = true;
        int i            = 0;
        for (auto child = source.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (child.isProcessingInstruction()
                && child.toProcessingInstruction().target().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0)
                continue; // the XML declaration is part of the prolog
            if (child.isDocumentType())
                continue;
            d->generateNew(child, QString(), i++);
        }
        d->suppressQueue = false;
        d->changed       = false;
        emit documentUpdated(false);
    }

    const QDomDocument &Document::document() const { return d->doc; }

    QString Document::prolog() const { return d->prolog; }

    QDomNode Document::insertNodeBefore(const QDomNode &node, const QDomNode &parent, const QDomNode &referenceNode)
    {
        if (d->readOnly)
            return {};
        if (!referenceNode.isNull() && !referenceNode.previousSibling().isNull())
            return insertNodeAfter(node, parent, referenceNode.previousSibling());

        auto parentRecord = d->recordOf(parent);
        if (!parentRecord || node.isNull())
            return {};
        double weight = 0;
        for (auto child = parent.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (auto r = d->recordOf(child)) {
                weight = r->fields().primaryWeight - 1;
                break;
            }
        }
        auto result = d->insertNode(node, parentRecord->rid, weight);
        d->finishLocalChange();
        return result;
    }

    QDomNode Document::insertNodeAfter(const QDomNode &node, const QDomNode &parent, const QDomNode &referenceNode)
    {
        if (node.isNull() || d->readOnly)
            return {};

        if (node.isDocumentFragment()) {
            QDomNode reference = referenceNode;
            for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) {
                auto inserted = insertNodeAfter(child, parent, reference);
                if (!inserted.isNull())
                    reference = inserted;
            }
            return {};
        }

        auto parentRecord = d->recordOf(parent);
        if (!parentRecord) {
            qDebug("jingle-sxe: trying to insert a node into a parent without a record");
            return {};
        }

        double     weight          = 0;
        const auto referenceRecord = referenceNode.parentNode() == parent ? d->recordOf(referenceNode) : nullptr;
        Record    *nextRecord      = nullptr;
        if (referenceRecord) {
            for (auto next = referenceNode.nextSibling(); !next.isNull() && !nextRecord; next = next.nextSibling())
                nextRecord = d->recordOf(next);
        }
        if (referenceRecord && nextRecord) {
            weight = (referenceRecord->fields().primaryWeight + nextRecord->fields().primaryWeight) / 2;
        } else {
            Record *last = referenceRecord;
            if (!last) {
                for (auto child = parent.lastChild(); !child.isNull() && !last; child = child.previousSibling())
                    last = d->recordOf(child);
            }
            if (last)
                weight = last->fields().primaryWeight + 1;
        }

        auto result = d->insertNode(node, parentRecord->rid, weight);
        d->finishLocalChange();
        return result;
    }

    void Document::removeNode(const QDomNode &node)
    {
        auto r = d->readOnly ? nullptr : d->recordOf(node);
        if (!r)
            return;
        d->deleteRecord(r->rid, false, true, true);
        d->finishLocalChange();
    }

    void Document::setAttribute(const QDomNode &node, const QString &attribute, const QString &value, int from, int n)
    {
        if (!node.isElement() || attribute.isEmpty() || d->readOnly)
            return;
        auto element = node.toElement();
        if (value.isNull()) {
            if (from < 0 && element.hasAttribute(attribute))
                removeNode(element.attributeNode(attribute));
            return;
        }
        if (element.hasAttribute(attribute)) {
            setNodeValue(element.attributeNode(attribute), value, from, n);
            return;
        }
        if (from >= 0)
            return; // partial replacement of a missing attribute
        auto attr = d->doc.createAttribute(attribute);
        attr.setValue(value);
        insertNodeAfter(attr, node);
    }

    void Document::setNodeValue(const QDomNode &node, const QString &value, int from, int n)
    {
        auto r = d->readOnly ? nullptr : d->recordOf(node);
        if (!r || !hasChdata(r->type))
            return;
        const auto current = r->fields().chdata;
        QString    next    = current;
        if (from >= 0 && n >= 0) {
            if (from + n > current.size())
                return;
            next.replace(from, n, value);
        } else {
            next = value;
            from = -1;
        }
        if (next == current)
            return;

        if (from < 0 && value.size() > MaxChdataPerEdit) {
            const auto rid = r->rid;
            d->localSet(r, { { QStringLiteral("chdata"), value.left(MaxChdataPerEdit) } });
            for (int pos = MaxChdataPerEdit; pos < value.size(); pos += MaxChdataPerEdit) {
                auto current = d->records.value(rid);
                if (!current)
                    break;
                auto append
                    = d->makeAppend(d->editsDoc, rid, current->version + 1, pos, value.mid(pos, MaxChdataPerEdit));
                d->queue(append);
                d->applySet(append, false);
            }
        } else if (from >= 0) {
            d->localSet(r,
                        { { QStringLiteral("chdata"), value },
                          { QStringLiteral("replacefrom"), QString::number(from) },
                          { QStringLiteral("replacen"), QString::number(n) } });
        } else {
            d->localSet(r, { { QStringLiteral("chdata"), value } });
        }
        d->finishLocalChange();
    }

    void Document::flush()
    {
        if (d->outgoing.isEmpty())
            return;
        auto edits = std::move(d->outgoing);
        d->outgoing.clear();
        emit outgoingEdits(edits);
    }

    void Document::applyEdits(const QDomElement &state)
    {
        QPointer<Document> guard(this);
        for (auto e = state.firstChildElement(); !e.isNull(); e = e.nextSiblingElement()) {
            if (!e.namespaceURI().isEmpty() && e.namespaceURI() != NS)
                continue;
            const auto tag = e.tagName();
            if (tag == QLatin1String("new"))
                d->applyNew(e, true);
            else if (tag == QLatin1String("set"))
                d->applySet(e, true);
            else if (tag == QLatin1String("remove"))
                d->applyRemove(e, true);
            if (!guard)
                return;
        }
        if (d->changed) {
            d->changed = false;
            emit documentUpdated(true);
            if (!guard)
                return;
        }
        if (!d->importing)
            flush();
    }

    void Document::beginImport(const QString &prolog)
    {
        d->clear();
        d->prolog    = prolog;
        d->importing = true;
    }

    void Document::endImport()
    {
        if (!d->importing)
            return;
        d->importing = false;
        d->changed   = false;
        emit documentUpdated(true);
    }

    bool Document::isImporting() const { return d->importing; }

    void Document::setReadOnly(bool readOnly) { d->readOnly = readOnly; }

    bool Document::isReadOnly() const { return d->readOnly; }

    QList<QDomElement> Document::snapshot(QDomDocument &doc) const
    {
        QList<QDomElement> edits;
        d->collectSnapshot(doc, QString(), edits);
        return edits;
    }

    void Document::setRidPrefix(const QString &prefix)
    {
        d->ridPrefix  = prefix.isEmpty() ? generateUUID() : prefix;
        d->ridCounter = 0;
    }

    QString Document::generateUUID()
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
#else
        const auto uuid = QUuid::createUuid().toString();
        return uuid.mid(1, uuid.size() - 2);
#endif
    }

    QString Document::prologOf(const QDomDocument &doc)
    {
        QString     prolog;
        QTextStream stream(&prolog);
        const auto  first = doc.firstChild();
        if (first.isProcessingInstruction()
            && first.toProcessingInstruction().target().compare(QLatin1String("xml"), Qt::CaseInsensitive) == 0)
            first.save(stream, 1);
        if (!doc.doctype().isNull())
            doc.doctype().save(stream, 1);
        stream.flush();
        return prolog;
    }

}}}
