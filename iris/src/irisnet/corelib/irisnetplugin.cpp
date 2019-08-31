/*
 * Copyright (C) 2006  Justin Karneges
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

#include "irisnetplugin.h"

namespace XMPP {
//----------------------------------------------------------------------------
// IrisNetProvider
//----------------------------------------------------------------------------
NetInterfaceProvider *IrisNetProvider::createNetInterfaceProvider()
{
    return nullptr;
}

NetGatewayProvider *IrisNetProvider::createNetGatewayProvider()
{
    return nullptr;
}

NetAvailabilityProvider *IrisNetProvider::createNetAvailabilityProvider()
{
    return nullptr;
}

NameProvider *IrisNetProvider::createNameProviderInternet()
{
    return nullptr;
}

NameProvider *IrisNetProvider::createNameProviderLocal()
{
    return nullptr;
}

ServiceProvider *IrisNetProvider::createServiceProvider()
{
    return nullptr;
}

//----------------------------------------------------------------------------
// NameProvider
//----------------------------------------------------------------------------
bool NameProvider::supportsSingle() const
{
    return false;
}

bool NameProvider::supportsLongLived() const
{
    return false;
}

bool NameProvider::supportsRecordType(int type) const
{
    Q_UNUSED(type);
    return false;
}

void NameProvider::resolve_localResultsReady(int id, const QList<XMPP::NameRecord> &results)
{
    Q_UNUSED(id);
    Q_UNUSED(results);
}

void NameProvider::resolve_localError(int id, XMPP::NameResolver::Error e)
{
    Q_UNUSED(id);
    Q_UNUSED(e);
}

} // namespace XMPP
