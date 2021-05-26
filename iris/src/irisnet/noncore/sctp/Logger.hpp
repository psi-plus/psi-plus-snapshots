#pragma once

#include <QtGlobal>
#include <cinttypes>

// #define PSI_ENABLE_SCTP_DEBUG
#define MS_WARN_TAG(tag, ...) qWarning("sctp:" __VA_ARGS__)
#ifdef PSI_ENABLE_SCTP_DEBUG
#define MS_DEBUG_TAG(tag, ...) qDebug("sctp:" __VA_ARGS__)
#define MS_TRACE()
#define MS_DEBUG_DEV(...) qDebug(__VA_ARGS__)
#else
#define MS_DEBUG_TAG(tag, ...)
#define MS_TRACE()
#define MS_DEBUG_DEV(...)
#endif
#define MS_ERROR(...) qCritical(__VA_ARGS__)
#define MS_HAS_DEBUG_TAG(tag) false

#define MS_ABORT(desc, ...)                                                                                            \
    do {                                                                                                               \
        qFatal("(ABORT) " desc, ##__VA_ARGS__);                                                                        \
    } while (false)

#define MS_ASSERT(condition, desc, ...)                                                                                \
    if (!(condition)) {                                                                                                \
        MS_ABORT("failed assertion `%s': " desc, #condition, ##__VA_ARGS__);                                           \
    }
