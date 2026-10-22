/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QITEMEDIAPLAYER_P_H
#define QITEMEDIAPLAYER_P_H

#include <QMediaPlayer>
#include <QUrl>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QPlaybackOptions>
#include <chrono>
#endif

inline void iteConfigureMediaNetworkTimeout(QMediaPlayer *player, const QUrl &url)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
        // An opener's HTTP proxy may need to fetch a complete media chunk
        // before replying. Qt's default 20 seconds can expire first on slow
        // connections, even though the upstream transfer is still progressing.
        auto options = player->playbackOptions();
        options.setNetworkTimeout(std::chrono::seconds(90));
        player->setPlaybackOptions(options);
    }
#else
    Q_UNUSED(player);
    Q_UNUSED(url);
#endif
}

#endif
