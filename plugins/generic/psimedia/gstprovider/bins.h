/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
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

#ifndef PSI_BINS_H
#define PSI_BINS_H

#include <gst/gstelement.h>

class QString;
class QSize;

namespace PsiMedia {

GstElement *bins_videoprep_create(const QSize &size, int fps, bool is_live);

GstElement *bins_audioenc_create(const QString &codec, int id, int rate, int size, int channels);
GstElement *bins_videoenc_create(const QString &codec, int id, int maxkbps);
GstElement *bins_audiodec_create(const QString &codec);
GstElement *bins_videodec_create(const QString &codec);

}

#endif
