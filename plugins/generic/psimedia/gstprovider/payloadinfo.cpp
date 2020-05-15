/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "payloadinfo.h"

#include <QByteArray>
#include <QStringList>

namespace PsiMedia {

static QString hexEncode(const QByteArray &in)
{
    QString out;
    for (char n : in)
        out += QString("%1").arg(static_cast<unsigned char>(n), 2, 16, QChar('0'));

    return out;
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    else
        return -1;
}

static int hexByte(char hi, char lo)
{
    int nhi = hexValue(hi);
    if (nhi < 0)
        return -1;
    int nlo = hexValue(lo);
    if (nlo < 0)
        return -1;
    int value = (hexValue(hi) << 4) + hexValue(lo);
    return value;
}

static QByteArray hexDecode(const QString &in)
{
    QByteArray out;
    for (int n = 0; n + 1 < in.length(); n += 2) {
        int value = hexByte(in[n].toLatin1(), in[n + 1].toLatin1());
        if (value < 0)
            return QByteArray(); // error
        out += char(value);
    }
    return out;
}

class my_foreach_state {
public:
    PPayloadInfo *                  out;
    QStringList *                   whitelist;
    QList<PPayloadInfo::Parameter> *list;
};

gboolean my_foreach_func(GQuark field_id, const GValue *value, gpointer user_data)
{
    my_foreach_state &state = *(static_cast<my_foreach_state *>(user_data));

    QString name = QString::fromLatin1(g_quark_to_string(field_id));
    if (G_VALUE_TYPE(value) == G_TYPE_STRING && state.whitelist->contains(name)) {
        QString svalue = QString::fromLatin1(g_value_get_string(value));

        // FIXME: is there a better way to detect when we should do this conversion?
        if (name == "configuration" && (state.out->name == "THEORA" || state.out->name == "VORBIS")) {
            QByteArray config = QByteArray::fromBase64(svalue.toLatin1());
            svalue            = hexEncode(config);
        }

        PPayloadInfo::Parameter i;
        i.name  = name;
        i.value = svalue;
        state.list->append(i);
    }

    return TRUE;
}

GstStructure *payloadInfoToStructure(const PPayloadInfo &info, const QString &media)
{
    GstStructure *out = gst_structure_new_empty("application/x-rtp");

    {
        GValue gv;
        memset(&gv, 0, sizeof(GValue));
        g_value_init(&gv, G_TYPE_STRING);
        g_value_set_string(&gv, media.toLatin1().data());
        gst_structure_set_value(out, "media", &gv);
    }

    // payload id field required
    if (info.id == -1) {
        gst_structure_free(out);
        return nullptr;
    }

    {
        GValue gv;
        memset(&gv, 0, sizeof(GValue));
        g_value_init(&gv, G_TYPE_INT);
        g_value_set_int(&gv, info.id);
        gst_structure_set_value(out, "payload", &gv);
    }

    // name required for payload values 96 or greater
    if (info.id >= 96 && info.name.isEmpty()) {
        gst_structure_free(out);
        return nullptr;
    }

    {
        GValue gv;
        memset(&gv, 0, sizeof(GValue));
        g_value_init(&gv, G_TYPE_STRING);
        g_value_set_string(&gv, info.name.toLatin1().data());
        gst_structure_set_value(out, "encoding-name", &gv);
    }

    if (info.clockrate != -1) {
        GValue gv;
        memset(&gv, 0, sizeof(GValue));
        g_value_init(&gv, G_TYPE_INT);
        g_value_set_int(&gv, info.clockrate);
        gst_structure_set_value(out, "clock-rate", &gv);
    }

    if (info.channels != -1) {
        GValue gv;
        memset(&gv, 0, sizeof(GValue));
        g_value_init(&gv, G_TYPE_STRING);
        g_value_set_string(&gv, QString::number(info.channels).toLatin1().data());
        gst_structure_set_value(out, "encoding-params", &gv);
    }

    foreach (const PPayloadInfo::Parameter &i, info.parameters) {
        QString value = i.value;

        // FIXME: is there a better way to detect when we should do this conversion?
        if (i.name == "configuration" && (info.name.toUpper() == "THEORA" || info.name.toUpper() == "VORBIS")) {
            QByteArray config = hexDecode(value);
            if (config.isEmpty()) {
                gst_structure_free(out);
                return nullptr;
            }

            value = QString::fromLatin1(config.toBase64());
        }

        GValue gv;
        memset(&gv, 0, sizeof(GValue));
        g_value_init(&gv, G_TYPE_STRING);
        g_value_set_string(&gv, value.toLatin1().data());
        gst_structure_set_value(out, i.name.toLatin1().data(), &gv);
    }

    return out;
}

PPayloadInfo structureToPayloadInfo(GstStructure *structure, QString *media)
{
    PPayloadInfo out;
    QString      media_;

    gint         x;
    const gchar *str;

    str           = gst_structure_get_name(structure);
    QString sname = QString::fromLatin1(str);
    if (sname != "application/x-rtp")
        return PPayloadInfo();

    str = gst_structure_get_string(structure, "media");
    if (!str)
        return PPayloadInfo();
    media_ = QString::fromLatin1(str);

    // payload field is required
    if (!gst_structure_get_int(structure, "payload", &x))
        return PPayloadInfo();

    out.id = x;

    str = gst_structure_get_string(structure, "encoding-name");
    if (str) {
        out.name = QString::fromLatin1(str);
    } else {
        // encoding-name field is required for payload values 96 or greater
        if (out.id >= 96)
            return PPayloadInfo();
    }

    if (gst_structure_get_int(structure, "clock-rate", &x))
        out.clockrate = x;

    str = gst_structure_get_string(structure, "encoding-params");
    if (str) {
        QString qstr = QString::fromLatin1(str);
        bool    ok;
        int     n = qstr.toInt(&ok);
        if (!ok)
            return PPayloadInfo();
        out.channels = n;
    }

    // note: if we ever change away from the whitelist approach, be sure
    //   not to grab the earlier static fields (e.g. clock-rate) as
    //   dynamic parameters
    QStringList whitelist;
    whitelist << "sampling"
              << "width"
              << "height"
              << "delivery-method"
              << "configuration";

    QList<PPayloadInfo::Parameter> list;

    my_foreach_state state;
    state.out       = &out;
    state.whitelist = &whitelist;
    state.list      = &list;
    if (!gst_structure_foreach(structure, my_foreach_func, &state))
        return PPayloadInfo();

    out.parameters = list;

    if (media)
        *media = media_;

    return out;
}

}
