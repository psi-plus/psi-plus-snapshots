// SPDX-License-Identifier: LGPL-2.1-or-later

#include <iris/stunbinding.h>
#include <iris/stuntransaction.h>

#include <QCoreApplication>
#include <QPointer>

using namespace XMPP;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    auto                          pool = StunTransactionPool::Ptr::create(StunTransaction::Udp);
    QPointer<StunTransactionPool> poolGuard(pool.data());

    // A binding is QObject-owned by its pool. It must not also keep a strong
    // shared reference back to that parent or the pair forms an ownership cycle.
    new StunBinding(pool.data());

    pool.clear();

    if (poolGuard)
        qFatal("StunBinding kept its parent StunTransactionPool alive");

    return 0;
}
