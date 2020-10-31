#pragma once

#include <QtGlobal>
#include <cinttypes>

#define MS_WARN_TAG(tag, args...) qWarning("sctp:" args)
#define MS_DEBUG_TAG(tag, args...) qDebug("sctp:" args)
#define MS_TRACE()
#define MS_ERROR(args...) qCritical(args)
#define MS_DEBUG_DEV(args...) qDebug(args)
#define MS_HAS_DEBUG_TAG(tag) false

#define MS_ABORT(desc, ...)                                                                                            \
    do {                                                                                                               \
        qFatal("(ABORT) " desc, ##__VA_ARGS__);                                                                        \
    } while (false)

#define MS_ASSERT(condition, desc, ...)                                                                                \
    if (!(condition)) {                                                                                                \
        MS_ABORT("failed assertion `%s': " desc, #condition, ##__VA_ARGS__);                                           \
    }
