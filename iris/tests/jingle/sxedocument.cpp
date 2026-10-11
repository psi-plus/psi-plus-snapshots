// SPDX-License-Identifier: LGPL-2.1-or-later
// XEP-0284 record model regressions. No XMPP traffic is involved.
#include <iris/xmpp-im/jingle-sxe-document.h>
#include <iris/xmpp-im/jingle-sxe.h>

#include <QCoreApplication>
#include <QTextStream>

using namespace XMPP::Jingle::SXE;

static void check(bool ok, const char *message)
{
    if (!ok)
        qFatal("%s", message);
}

static QString dump(const QDomDocument &doc)
{
    QString     out;
    QTextStream stream(&out);
    doc.documentElement().save(stream, -1);
    return out;
}

// Delivers local edits of one document to the others, like the SXE application does.
struct Link {
    QDomDocument       wire;
    QList<QDomElement> pending;

    explicit Link(Document &from)
    {
        QObject::connect(&from, &Document::outgoingEdits, &from,
                         [this](const QList<QDomElement> &edits) { pending += edits; });
    }

    QDomElement take()
    {
        auto state = wire.createElementNS(NS, QStringLiteral("state"));
        for (const auto &edit : std::as_const(pending))
            state.appendChild(wire.importNode(edit, true));
        pending.clear();
        return state;
    }
};

static QDomElement xml(QDomDocument &doc, const QString &text)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    const bool parsed = doc.setContent(text, true);
#else
    const auto parsed = doc.setContent(text, QDomDocument::ParseOption::UseNamespaceProcessing);
#endif
    check(bool(parsed), "invalid test XML");
    return doc.documentElement();
}

static void importState(Document &target, const Document &source)
{
    QDomDocument wire;
    auto         state = wire.createElementNS(NS, QStringLiteral("state"));
    for (const auto &edit : source.snapshot(wire))
        state.appendChild(edit);
    target.beginImport(source.prolog());
    target.applyEdits(state);
    target.endImport();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // The initial document travels as a snapshot.
    Document host, joiner;
    host.reset(emptyWhiteboardDocument());
    check(host.document().documentElement().attribute(QStringLiteral("viewBox")) == QStringLiteral("0 0 600 400"),
          "empty whiteboard has no canvas");
    importState(joiner, host);
    check(dump(joiner.document()) == dump(host.document()), "snapshot did not reproduce the document");

    Link toJoiner(host), toHost(joiner);

    // A local subtree is sent as <new/> edits, parents first.
    QDomDocument temp;
    auto         path     = xml(temp,
                                QStringLiteral("<path xmlns='http://www.w3.org/2000/svg' id='p1' d='M 0 0 L 10 10' "
                                                           "stroke='#ff0000'/>"));
    auto         inserted = host.insertNodeAfter(path, host.document().documentElement());
    check(!inserted.isNull() && inserted.ownerDocument() == host.document(), "insert did not create a local node");
    host.flush();
    joiner.applyEdits(toJoiner.take());
    check(dump(joiner.document()) == dump(host.document()), "new edits diverged");

    // In-order <set/> and partial character data replacement.
    joiner.setAttribute(joiner.document().documentElement().firstChildElement(), QStringLiteral("stroke"),
                        QStringLiteral("#0000ff"));
    joiner.setAttribute(joiner.document().documentElement().firstChildElement(), QStringLiteral("d"),
                        QStringLiteral("20 20"), 8, 5);
    joiner.flush();
    host.applyEdits(toHost.take());
    check(host.document().documentElement().firstChildElement().attribute(QStringLiteral("d"))
              == QStringLiteral("M 0 0 L 20 20"),
          "partial replacement failed");
    check(dump(joiner.document()) == dump(host.document()), "set edits diverged");

    // Conflicting non-commutative edits of the same version are both undone (XEP-0284 7.3).
    auto hostPath   = host.document().documentElement().firstChildElement();
    auto joinerPath = joiner.document().documentElement().firstChildElement();
    host.setAttribute(hostPath, QStringLiteral("stroke"), QStringLiteral("#111111"));
    joiner.setAttribute(joinerPath, QStringLiteral("stroke"), QStringLiteral("#222222"));
    host.flush();
    joiner.flush();
    host.applyEdits(toHost.take());
    joiner.applyEdits(toJoiner.take());
    check(dump(joiner.document()) == dump(host.document()), "conflicting edits did not converge");
    check(hostPath.attribute(QStringLiteral("stroke")) == QStringLiteral("#0000ff"),
          "conflicting edits were not reverted to the previous version");
    // The records stay synchronized afterwards.
    host.setAttribute(hostPath, QStringLiteral("stroke"), QStringLiteral("#333333"));
    host.flush();
    joiner.applyEdits(toJoiner.take());
    check(joinerPath.attribute(QStringLiteral("stroke")) == QStringLiteral("#333333"),
          "edit after a conflict was not applied in order");

    // Ordering follows primary-weight regardless of arrival order.
    QDomDocument temp2;
    auto         first = xml(temp2, QStringLiteral("<g xmlns='http://www.w3.org/2000/svg' id='front'/>"));
    host.insertNodeBefore(first, host.document().documentElement(), hostPath);
    host.flush();
    joiner.applyEdits(toJoiner.take());
    check(joiner.document().documentElement().firstChildElement().attribute(QStringLiteral("id"))
              == QStringLiteral("front"),
          "primary-weight ordering ignored");
    check(dump(joiner.document()) == dump(host.document()), "insertBefore diverged");

    // Long character data is split into edits and reassembled; the snapshot keeps the version.
    const QString image = QStringLiteral("data:image/png;base64,") + QString(3 * Document::MaxChdataPerEdit, 'A');
    host.setAttribute(hostPath, QStringLiteral("href"), image);
    host.flush();
    check(toJoiner.pending.size() > 1, "long character data was not split");
    joiner.applyEdits(toJoiner.take());
    check(joinerPath.attribute(QStringLiteral("href")) == image, "split character data was not reassembled");
    Document late;
    importState(late, host);
    check(dump(late.document()) == dump(host.document()), "snapshot with split data diverged");
    Link fromLate(late);
    late.setAttribute(late.document().documentElement().lastChildElement(), QStringLiteral("href"),
                      QStringLiteral("x"));
    late.flush();
    host.applyEdits(fromLate.take());
    check(hostPath.attribute(QStringLiteral("href")) == QStringLiteral("x"),
          "snapshot import did not preserve record versions");

    // Removing a parent removes its subtree; a remote removal of a parent with children
    // sends <remove/> for those children.
    host.removeNode(hostPath);
    host.flush();
    joiner.applyEdits(toJoiner.take());
    check(dump(joiner.document()) == dump(host.document()), "remove diverged");

    QDomDocument wire;
    auto         bad    = xml(wire,
                              QStringLiteral("<state xmlns='urn:xmpp:sxe:0'>"
                                                        "<new rid='orphan' type='element' name='path' parent='missing'/>"
                                                        "<new rid='badname' type='element' name='1 2' parent='%1'/>"
                                                        "</state>")
                                  .arg(QStringLiteral("unknown")));
    const auto   before = dump(joiner.document());
    joiner.applyEdits(bad);
    check(dump(joiner.document()) == before, "orphan or invalid records were applied");

    // Duplicate attributes are resolved by the secondary weight on every endpoint.
    QDomDocument dupWire;
    auto         rootRid = QString();
    {
        Document     a;
        QDomDocument doc;
        doc.appendChild(doc.createElement(QStringLiteral("root")));
        a.reset(doc);
        QDomDocument snapWire;
        auto         edits = a.snapshot(snapWire);
        rootRid            = edits.first().attribute(QStringLiteral("rid"));
        auto state         = xml(dupWire,
                                 QStringLiteral("<state xmlns='urn:xmpp:sxe:0'>"
                                                        "<new rid='b' type='attr' name='x' chdata='from-b' parent='%1'/>"
                                                        "<new rid='a' type='attr' name='x' chdata='from-a' parent='%1'/>"
                                                        "</state>")
                                     .arg(rootRid));
        a.applyEdits(state);
        check(a.document().documentElement().attribute(QStringLiteral("x")) == QStringLiteral("from-b"),
              "duplicate attribute kept the lower secondary weight");
        // A removed rid can't be recreated.
        auto again = xml(dupWire,
                         QStringLiteral("<state xmlns='urn:xmpp:sxe:0'><remove target='b'/>"
                                        "<new rid='b' type='attr' name='y' chdata='v' parent='%1'/></state>")
                             .arg(rootRid));
        a.applyEdits(again);
        check(!a.document().documentElement().hasAttribute(QStringLiteral("y")), "a removed rid was reused");
    }

    qInfo("SXE document regressions passed");
    return 0;
}
