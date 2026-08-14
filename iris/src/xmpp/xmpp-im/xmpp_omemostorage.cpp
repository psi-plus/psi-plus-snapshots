/*
 * xmpp_omemostorage.cpp - persistent state interface for XEP-0384 OMEMO
 * Copyright (C) 2026 Sergey Ilinykh
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 */

#include "xmpp_omemostorage.h"

namespace XMPP {

OmemoStorage::OmemoData MemoryOmemoStorage::allData() const { return data_; }

bool MemoryOmemoStorage::setOwnDevice(const std::optional<OwnDevice> &device)
{
    data_.ownDevice = device;
    return true;
}

bool MemoryOmemoStorage::addSignedPreKeyPair(uint32_t keyId, const SignedPreKeyPair &keyPair)
{
    data_.signedPreKeyPairs.insert(keyId, keyPair);
    return true;
}

bool MemoryOmemoStorage::removeSignedPreKeyPair(uint32_t keyId)
{
    data_.signedPreKeyPairs.remove(keyId);
    return true;
}

bool MemoryOmemoStorage::addPreKeyPairs(const QHash<uint32_t, QByteArray> &keyPairs)
{
    for (auto i = keyPairs.cbegin(); i != keyPairs.cend(); ++i)
        data_.preKeyPairs.insert(i.key(), i.value());
    return true;
}

bool MemoryOmemoStorage::removePreKeyPair(uint32_t keyId)
{
    data_.preKeyPairs.remove(keyId);
    return true;
}

bool MemoryOmemoStorage::addDevice(const QString &jid, uint32_t deviceId, const Device &device)
{
    data_.devices[jid].insert(deviceId, device);
    return true;
}

bool MemoryOmemoStorage::removeDevice(const QString &jid, uint32_t deviceId)
{
    auto i = data_.devices.find(jid);
    if (i == data_.devices.end())
        return true;
    i->remove(deviceId);
    if (i->isEmpty())
        data_.devices.erase(i);
    return true;
}

bool MemoryOmemoStorage::removeDevices(const QString &jid)
{
    data_.devices.remove(jid);
    return true;
}

bool MemoryOmemoStorage::resetAll()
{
    data_ = {};
    return true;
}

} // namespace XMPP
