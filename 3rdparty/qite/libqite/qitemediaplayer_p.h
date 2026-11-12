/* SPDX-License-Identifier: Apache-2.0 */
#ifndef QITEMEDIAPLAYER_P_H
#define QITEMEDIAPLAYER_P_H

#include "qite.h"
#include <QIODevice>
#include <QMediaPlayer>
#include <QPointer>
#include <QUrl>
#include <utility>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QPlaybackOptions>
#include <chrono>
#endif

class ITEMediaPlayer : public QMediaPlayer {
public:
    using QMediaPlayer::QMediaPlayer;
    ~ITEMediaPlayer() override { abortInput(); }
    void abortInput()
    {
        if (cancelOpen_) {
            const auto cancel = std::exchange(cancelOpen_, {});
            cancel();
        }
        if (opener_ && stream_)
            opener_->abort(stream_);
    }
    void setPendingOpen(ITEMediaOpener::CancelOpen cancel) { cancelOpen_ = std::move(cancel); }
    void setInput(ITEMediaOpener *opener, QIODevice *stream)
    {
        opener_ = opener;
        stream_ = stream;
        if (stream)
            new StreamCleanup(opener, stream, this);
        if (opener)
            connect(opener, &QObject::destroyed, this, [this] { delete this; });
    }

private:
    // QObject children are destroyed after the QMediaPlayer base destructor.
    // Qt 5 disconnects its signals during teardown, so destroyed() is too late
    // to arrange cleanup through a connection on the player itself.
    class StreamCleanup : public QObject {
    public:
        StreamCleanup(ITEMediaOpener *opener, QIODevice *stream, QObject *parent) :
            QObject(parent), opener_(opener), stream_(stream)
        {
        }
        ~StreamCleanup() override
        {
            if (opener_ && stream_)
                opener_->close(stream_);
        }

    private:
        QPointer<ITEMediaOpener> opener_;
        QPointer<QIODevice>      stream_;
    };
    QPointer<ITEMediaOpener> opener_;
    QPointer<QIODevice>      stream_;
    ITEMediaOpener::CancelOpen cancelOpen_;
};

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
