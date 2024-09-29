/*
 * xmpp_mamtask.cpp - XEP-0313 Message Archive Management
 * Copyright (C) 2024 mcneb10
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

#include "xmpp_mamtask.h"

using namespace XMLHelper;
using namespace XMPP;

class MAMTask::Private {
public:
    int  mamPageSize;    // TODO: this is the max page size for MAM request. Should be made into a config option in Psi+
    int  mamMaxMessages; // maximum mam pages total, also should be config. zero means unlimited
    int  messagesFetched;
    bool flipPages;
    bool backwards;
    bool allowMUCArchives;
    bool metadataFetched;
    Jid  j;
    MAMTask           *q;
    QString            firstID;
    QString            lastID;
    QString            lastArchiveID;
    QString            fromID;
    QString            toID;
    QString mainQueryID;
    QString            currentPageQueryID;
    QString currentPageQueryIQID;
    QDateTime          from;
    QDateTime          to;
    QList<QDomElement> archive;

    void  getPage();
    void  getArchiveMetadata();
    XData makeMAMFilter();
};

MAMTask::MAMTask(Task *parent) : Task(parent) { d = new Private; }
MAMTask::MAMTask(const MAMTask &x) : Task(x.parent()) { d = x.d; }
MAMTask::~MAMTask() { delete d; }

const QList<QDomElement> &MAMTask::archive() const { return d->archive; }

XData MAMTask::Private::makeMAMFilter()
{
    XData::FieldList fl;

    XData::Field with;
    with.setType(XData::Field::Field_JidSingle);
    with.setVar(QLatin1String("with"));
    with.setValue(QStringList(j.full()));
    fl.append(with);

    XData::Field includeGroupchat;
    includeGroupchat.setType(XData::Field::Field_Boolean);
    includeGroupchat.setVar(QLatin1String("include-groupchat"));
    includeGroupchat.setValue(QStringList(QLatin1String(allowMUCArchives ? "true" : "false")));
    fl.append(includeGroupchat);

    if (from.isValid()) {
        XData::Field start;
        start.setType(XData::Field::Field_TextSingle);
        start.setVar(QLatin1String("start"));
        from.setTimeSpec(Qt::UTC);
        start.setValue(QStringList(from.toString()));
        fl.append(start);
    }

    if (to.isValid()) {
        XData::Field end;
        end.setType(XData::Field::Field_TextSingle);
        end.setVar(QLatin1String("end"));
        to.setTimeSpec(Qt::UTC);
        end.setValue(QStringList(to.toString()));
        fl.append(end);
    }

    if (!fromID.isNull()) {
        XData::Field start_id;
        start_id.setType(XData::Field::Field_TextSingle);
        start_id.setVar(QLatin1String("after-id"));
        start_id.setValue(QStringList(fromID));
        fl.append(start_id);
    }

    if (!toID.isNull()) {
        XData::Field end_id;
        end_id.setType(XData::Field::Field_TextSingle);
        end_id.setVar(QLatin1String("before-id"));
        end_id.setValue(QStringList(toID));
        fl.append(end_id);
    }

    XData x;
    x.setType(XData::Data_Submit);
    x.setFields(fl);
    x.setRegistrarType(XMPP_MAM_NAMESPACE);

    return x;
}

void MAMTask::Private::getPage()
{
    currentPageQueryIQID = q->genUniqueID();
    QDomElement iq    = createIQ(q->doc(), QLatin1String("set"), QLatin1String(), currentPageQueryIQID);
    QDomElement query = q->doc()->createElementNS(XMPP_MAM_NAMESPACE, QLatin1String("query"));
    currentPageQueryID    = q->genUniqueID();
    query.setAttribute(QLatin1String("queryid"), currentPageQueryID);
    XData x = makeMAMFilter();

    SubsetsClientManager rsm;
    rsm.setMax(mamMaxMessages);

    if (flipPages)
        query.appendChild(emptyTag(q->doc(), QLatin1String("flip-page")));

    if (lastArchiveID.isNull()) {
        if (backwards) {
            rsm.getLast();
        } else {
            rsm.getFirst();
        }
    } else {
        if (backwards) {
            rsm.setFirstID(lastArchiveID);
            rsm.getPrevious();
        } else {
            rsm.setLastID(lastArchiveID);
            rsm.getNext();
        }
    }

    query.appendChild(x.toXml(q->doc()));
    query.appendChild(rsm.makeQueryElement(q->doc()));
    iq.appendChild(query);
    q->send(iq);
}

void MAMTask::Private::getArchiveMetadata()
{
    // Craft a query to get the first and last messages in an archive
    mainQueryID = q->genUniqueID();
    QDomElement iq       = createIQ(q->doc(), QLatin1String("get"), QLatin1String(), mainQueryID);
    QDomElement metadata = emptyTag(q->doc(), QLatin1String("metadata"));
    metadata.setAttribute(QLatin1String("xmlns"), XMPP_MAM_NAMESPACE);
    iq.appendChild(metadata);

    q->send(iq);
}

// Note: Set `j` to a resource if you just want to query that resource
// if you want to query all resources, set `j` to the bare JID

// Filter by time range
void MAMTask::get(const Jid &j, const QDateTime &from, const QDateTime &to, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    d->archive         = {};
    d->messagesFetched = 0;
    d->metadataFetched = false;

    d->j                = j;
    d->from             = from;
    d->to               = to;
    d->allowMUCArchives = allowMUCArchives;
    d->mamPageSize      = mamPageSize;
    d->mamMaxMessages   = mamMaxMessages;
    d->flipPages        = flipPages;
    d->backwards        = backwards;
    d->q                = this;
}

// Filter by id range
void MAMTask::get(const Jid &j, const QString &fromID, const QString &toID, const bool allowMUCArchives,
                  int mamPageSize, int mamMaxMessages, bool flipPages, bool backwards)
{
    d->archive         = {};
    d->messagesFetched = 0;
    d->metadataFetched = false;

    d->j                = j;
    d->fromID           = fromID;
    d->toID             = toID;
    d->allowMUCArchives = allowMUCArchives;
    d->mamPageSize      = mamPageSize;
    d->mamMaxMessages   = mamMaxMessages;
    d->flipPages        = flipPages;
    d->backwards        = backwards;
}

void MAMTask::onGo() { d->getArchiveMetadata(); }

bool MAMTask::take(const QDomElement &x)
{
    if (d->metadataFetched) {
        if (iqVerify(x, QString(), d->currentPageQueryIQID)) {
            if (!x.elementsByTagNameNS(QLatin1String("urn:ietf:params:xml:ns:xmpp-stanzas"),
                                       QLatin1String("item-not-found"))
                     .isEmpty()) {
                setError(2, "First or last stanza UID of filter was not found in the archive");
                return true;
            } else if (!x.elementsByTagNameNS(XMPP_MAM_NAMESPACE, QLatin1String("fin")).isEmpty()) {
                // We are done?
                //setSuccess();
                //return true;
                return false; // TODO: testing
            }
            // Probably ignore it
            return false;
        }

        QDomElement result = x.firstChildElement("result");
        if (result != QDomElement() && result.namespaceURI() == XMPP_MAM_NAMESPACE
            && result.attribute(QLatin1String("queryid")) == d->currentPageQueryID) {

            d->archive.append(result);
            d->lastArchiveID   = result.attribute(QLatin1String("id"));
            d->messagesFetched = d->messagesFetched + 1;

            // Check if we are done
            if (result.attribute(QLatin1String("id")) == d->lastID || d->messagesFetched >= d->mamMaxMessages) {
                setSuccess();
            } else if (d->messagesFetched % d->mamPageSize == 0) {
                d->getPage();
            }
        }
    } else {
        if (!iqVerify(x, QString(), d->mainQueryID))
            return false;

        // Return if the archive is empty
        QDomElement queryMetadata = x.firstChildElement(QLatin1String("metadata"));
        if(queryMetadata == QDomElement()) {
            setError(1, "Malformed server metadata response");
            return true;
        }
        if (!queryMetadata.hasChildNodes()) {
            // No data in archive
            setSuccess();
            return true;
        }

        QDomElement start_id = queryMetadata.firstChildElement(QLatin1String("start"));
        QDomElement end_id = queryMetadata.firstChildElement(QLatin1String("end"));

        if (start_id.isNull() || end_id.isNull()) {
            setError(1, "Malformed server metadata response");
            return true;
        }

        if (d->backwards) {
            d->lastID  = start_id.attribute(QLatin1String("id"));
            d->firstID = end_id.attribute(QLatin1String("id"));
        } else {
            d->firstID = start_id.attribute(QLatin1String("id"));
            d->lastID  = end_id.attribute(QLatin1String("id"));
        }
        d->metadataFetched = true;
        d->getPage();
    }

    return true;
}
