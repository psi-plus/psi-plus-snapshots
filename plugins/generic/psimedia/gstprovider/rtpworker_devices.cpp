/*
 * Copyright (C) 2026  Psi IM team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "rtpworker.h"

#include <QMutexLocker>

namespace PsiMedia {
namespace {

    enum class InputSourceMode {
        None,
        Live,
        File,
        Data,
    };

    struct InputSourceIdentity {
        InputSourceMode mode = InputSourceMode::None;
        QString         audioInput;
        QString         videoInput;
        QString         fileName;
        QByteArray      fileData;

        bool operator!=(const InputSourceIdentity &other) const
        {
            return mode != other.mode || audioInput != other.audioInput || videoInput != other.videoInput
                || fileName != other.fileName || fileData != other.fileData;
        }
    };

    InputSourceIdentity inputSourceIdentity(const QString &audioInput, const QString &videoInput,
                                            const QString &fileName, const QByteArray &fileData)
    {
        InputSourceIdentity result;
        result.audioInput = audioInput;
        result.videoInput = videoInput;
        result.fileName   = fileName;
        result.fileData   = fileData;

        if (!fileData.isEmpty())
            result.mode = InputSourceMode::Data;
        else if (!fileName.isEmpty())
            result.mode = InputSourceMode::File;
        else if (!audioInput.isEmpty() || !videoInput.isEmpty())
            result.mode = InputSourceMode::Live;
        return result;
    }

} // namespace

void RtpWorker::setInputDevices(const QString &audioInput, const QString &videoInput, const QString &fileName,
                                const QByteArray &fileData, bool loop)
{
    const auto oldSource     = inputSourceIdentity(ain, vin, infile, indata);
    const auto newSource     = inputSourceIdentity(audioInput, videoInput, fileName, fileData);
    const bool sourceChanged = oldSource != newSource;

    const bool additiveLiveSource = oldSource.mode == InputSourceMode::Live && newSource.mode == InputSourceMode::Live
        && (oldSource.audioInput.isEmpty() || oldSource.audioInput == newSource.audioInput)
        && (oldSource.videoInput.isEmpty() || oldSource.videoInput == newSource.videoInput)
        && (!oldSource.audioInput.isEmpty() || !newSource.audioInput.isEmpty())
        && (!oldSource.videoInput.isEmpty() || !newSource.videoInput.isEmpty());

    // Adding the second live media source (audio -> A/V or video -> A/V) is a
    // topology extension, not a source replacement. RtpWorker::update() grafts
    // the missing branch onto the active sender. Real replacement/removal still
    // rebuilds the sender to keep the legacy semantics deterministic.
    const bool rebuildSender = sourceChanged && sendbin && !additiveLiveSource;

    bool audioWasTransmitting = false;
    bool videoWasTransmitting = false;
    if (rebuildSender) {
        {
            QMutexLocker locker(&rtpaudioout_mutex);
            audioWasTransmitting = rtpaudioout;
        }
        {
            QMutexLocker locker(&rtpvideoout_mutex);
            videoWasTransmitting = rtpvideoout;
        }

        // Capture identity is part of the running sender, not merely mutable
        // configuration. Revoke the old source before committing the new one,
        // but preserve the independent receive graph and playback state.
        cleanupSend();
        localAudioPayloadInfo.clear();
        localVideoPayloadInfo.clear();
        actual_localAudioPayloadInfo.clear();
        actual_localVideoPayloadInfo.clear();
        canTransmitAudio = false;
        canTransmitVideo = false;
    }

    ain      = audioInput;
    vin      = videoInput;
    infile   = fileName;
    indata   = fileData;
    loopFile = loop;

    if (rebuildSender) {
        const bool fileLikeSource = newSource.mode == InputSourceMode::File || newSource.mode == InputSourceMode::Data;

        QMutexLocker audioLocker(&rtpaudioout_mutex);
        rtpaudioout = audioWasTransmitting && (fileLikeSource || !audioInput.isEmpty());
        audioLocker.unlock();

        QMutexLocker videoLocker(&rtpvideoout_mutex);
        rtpvideoout = videoWasTransmitting && (fileLikeSource || !videoInput.isEmpty());
    }
}

} // namespace PsiMedia
