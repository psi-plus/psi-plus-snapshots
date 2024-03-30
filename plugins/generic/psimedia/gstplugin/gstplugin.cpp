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

#include "psimediaprovider.h"

#include "gstprovider.h"

#include <QtPlugin>

namespace PsiMedia {

class GstPlugin : public QObject, public Plugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.psi-im.GstPlugin")
    Q_INTERFACES(PsiMedia::Plugin)

public:
    virtual Provider *createProvider(const QVariantMap &vm)
    {
        auto p = new GstProvider(vm);
        if (!p->isInitialized()) {
            delete p;
            p = nullptr;
        }
        return p;
    }
};

}

#include "gstplugin.moc"
