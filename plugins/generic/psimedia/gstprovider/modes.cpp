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

#include "modes.h"

//#include <gst/gst.h>

namespace PsiMedia {

// FIXME: any better way besides hardcoding?

/*static bool have_element(const QString &name)
{
    GstElement *e = gst_element_factory_make(name.toLatin1().data(), nullptr);
    if(!e)
        return false;

    g_object_unref(G_OBJECT(e));
    return true;
}

static bool have_codec(const QString &enc, const QString &dec, const QString &pay, const QString &depay)
{
    if(have_element(enc) && have_element(dec) && have_element(pay) && have_element(depay))
        return true;
    else
        return false;
}

static bool have_pcmu()
{
    return have_codec("mulawenc", "mulawdec", "rtppcmupay", "rtppcmudepay");
}

static bool have_h263p()
{
    return have_codec("ffenc_h263p", "ffdec_h263", "rtph263ppay", "rtph263pdepay");
}*/

// opus, theora, and vorbis are guaranteed to exist

QList<PAudioParams> modes_supportedAudio()
{
    QList<PAudioParams> list;
    /*if(have_pcmu())
    {
        PAudioParams p;
        p.codec = "pcmu";
        p.sampleRate = 8000;
        p.sampleSize = 16;
        p.channels = 1;
        list += p;
    }*/
    {
        PAudioParams p;
        p.codec      = "opus";
        p.sampleRate = 8000;
        p.sampleSize = 16;
        p.channels   = 1;
        list += p;
    }
    {
        PAudioParams p;
        p.codec      = "opus";
        p.sampleRate = 16000;
        p.sampleSize = 16;
        p.channels   = 1;
        list += p;
    }
    /*{
        PAudioParams p;
        p.codec = "opus";
        p.sampleRate = 32000;
        p.sampleSize = 16;
        p.channels = 1;
        list += p;
    }
    {
        PAudioParams p;
        p.codec = "vorbis";
        p.sampleRate = 44100;
        p.sampleSize = 32;
        p.channels = 2;
        list += p;
    }*/
    return list;
}

QList<PVideoParams> modes_supportedVideo()
{
    QList<PVideoParams> list;
    /*if(have_h263p())
    {
        PVideoParams p;
        p.codec = "h263p";
        p.size = QSize(160, 120);
        p.fps = 30;
        list += p;
    }
    {
        PVideoParams p;
        p.codec = "theora";
        p.size = QSize(160, 120);
        p.fps = 30;
        list += p;
    }
    {
        PVideoParams p;
        p.codec = "theora";
        p.size = QSize(640, 480);
        p.fps = 15;
        list += p;
    }*/
    {
        PVideoParams p;
        p.codec = "theora";
        p.size  = QSize(640, 480);
        p.fps   = 30;
        list += p;
    }
    /*{
        PVideoParams p;
        p.codec = "theora";
        p.size = QSize(640, 480);
        p.fps = 15;
        list += p;
    }
    {
        PVideoParams p;
        p.codec = "theora";
        p.size = QSize(640, 480);
        p.fps = 30;
        list += p;
    }*/
    return list;
}

}
