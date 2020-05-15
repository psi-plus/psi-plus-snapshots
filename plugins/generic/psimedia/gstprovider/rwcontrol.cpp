/*
 * Copyright (C) 2008-2009  Barracuda Networks, Inc.
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

#include "rwcontrol.h"

#include "gstthread.h"
#include "rtpworker.h"
#include <QPointer>

// note: queuing frames doesn't really make much sense, since if the UI
//   receives 5 frames at once, they'll just get painted on each other in
//   succession and you'd only really see the last one.  however, we'll queue
//   frames in case we ever want to do timestamped frames.
#define QUEUE_FRAME_MAX 10

namespace PsiMedia {

static int queuedFrameInfo(const QList<RwControlMessage *> &list, RwControlFrame::Type type, int *firstPos)
{
    int  count = 0;
    bool first = true;
    for (int n = 0; n < list.count(); ++n) {
        RwControlMessage *msg = list[n];
        if (msg->type == RwControlMessage::Frame && static_cast<RwControlFrameMessage *>(msg)->frame.type == type) {
            if (first)
                *firstPos = n;
            ++count;
            first = false;
        }
    }
    return count;
}

static RwControlFrameMessage *getLatestFrameAndRemoveOthers(QList<RwControlMessage *> *list, RwControlFrame::Type type)
{
    RwControlFrameMessage *fmsg = nullptr;
    for (int n = 0; n < list->count(); ++n) {
        RwControlMessage *msg = list->at(n);
        if (msg->type == RwControlMessage::Frame && static_cast<RwControlFrameMessage *>(msg)->frame.type == type) {
            // if we already had a frame, discard it and take the next
            delete fmsg;

            fmsg = static_cast<RwControlFrameMessage *>(msg);
            list->removeAt(n);
            --n; // adjust position
        }
    }
    return fmsg;
}

static RwControlAudioIntensityMessage *getLatestAudioIntensityAndRemoveOthers(QList<RwControlMessage *> *   list,
                                                                              RwControlAudioIntensity::Type type)
{
    RwControlAudioIntensityMessage *amsg = nullptr;
    for (int n = 0; n < list->count(); ++n) {
        RwControlMessage *msg = list->at(n);
        if (msg->type == RwControlMessage::AudioIntensity
            && static_cast<RwControlAudioIntensityMessage *>(msg)->intensity.type == type) {
            // if we already had a msg, discard it and take the next
            delete amsg;

            amsg = static_cast<RwControlAudioIntensityMessage *>(msg);
            list->removeAt(n);
            --n; // adjust position
        }
    }
    return amsg;
}

static void simplifyQueue(QList<RwControlMessage *> *list)
{
    // is there a stop message?
    int at = -1;
    for (int n = 0; n < list->count(); ++n) {
        if (list->at(n)->type == RwControlMessage::Stop) {
            at = n;
            break;
        }
    }

    // if there is, remove all messages after it
    if (at != -1) {
        for (int n = at + 1; n < list->count();)
            list->removeAt(n);
    }
}

static RwControlStatusMessage *statusFromWorker(RtpWorker *worker)
{
    auto msg                          = new RwControlStatusMessage;
    msg->status.localAudioParams      = worker->localAudioParams;
    msg->status.localVideoParams      = worker->localVideoParams;
    msg->status.localAudioPayloadInfo = worker->localAudioPayloadInfo;
    msg->status.localVideoPayloadInfo = worker->localVideoPayloadInfo;
    msg->status.canTransmitAudio      = worker->canTransmitAudio;
    msg->status.canTransmitVideo      = worker->canTransmitVideo;
    return msg;
}

static void applyDevicesToWorker(RtpWorker *worker, const RwControlConfigDevices &devices)
{
    worker->aout     = devices.audioOutId;
    worker->ain      = devices.audioInId;
    worker->vin      = devices.videoInId;
    worker->infile   = devices.fileNameIn;
    worker->indata   = devices.fileDataIn;
    worker->loopFile = devices.loopFile;
    worker->setOutputVolume(devices.audioOutVolume);
    worker->setInputVolume(devices.audioInVolume);
}

static void applyCodecsToWorker(RtpWorker *worker, const RwControlConfigCodecs &codecs)
{
    if (codecs.useLocalAudioParams)
        worker->localAudioParams = codecs.localAudioParams;
    if (codecs.useLocalVideoParams)
        worker->localVideoParams = codecs.localVideoParams;
    if (codecs.useRemoteAudioPayloadInfo)
        worker->remoteAudioPayloadInfo = codecs.remoteAudioPayloadInfo;
    if (codecs.useRemoteVideoPayloadInfo)
        worker->remoteVideoPayloadInfo = codecs.remoteVideoPayloadInfo;

    worker->maxbitrate = codecs.maximumSendingBitrate;
}

//----------------------------------------------------------------------------
// RwControlLocal
//----------------------------------------------------------------------------
RwControlLocal::RwControlLocal(GstMainLoop *thread, QObject *parent) :
    QObject(parent), app(nullptr), cb_rtpAudioOut(nullptr), cb_rtpVideoOut(nullptr), cb_recordData(nullptr),
    wake_pending(false)
{
    thread_ = thread;
    remote_ = nullptr;

    // create RwControlRemote, block until ready
    QMutexLocker locker(&m);
    timer = g_timeout_source_new(0);
    g_source_set_callback(timer, cb_doCreateRemote, this, nullptr);
    g_source_attach(timer, thread_->mainContext());
    w.wait(&m);
}

RwControlLocal::~RwControlLocal()
{
    // delete RwControlRemote, block until done
    QMutexLocker locker(&m);
    timer = g_timeout_source_new(0);
    g_source_set_callback(timer, cb_doDestroyRemote, this, nullptr);
    g_source_attach(timer, thread_->mainContext());
    w.wait(&m);

    qDeleteAll(in);
}

void RwControlLocal::start(const RwControlConfigDevices &devices, const RwControlConfigCodecs &codecs)
{
    auto msg     = new RwControlStartMessage;
    msg->devices = devices;
    msg->codecs  = codecs;
    remote_->postMessage(msg);
}

void RwControlLocal::stop()
{
    auto msg = new RwControlStopMessage;
    remote_->postMessage(msg);
}

void RwControlLocal::updateDevices(const RwControlConfigDevices &devices)
{
    auto msg     = new RwControlUpdateDevicesMessage;
    msg->devices = devices;
    remote_->postMessage(msg);
}

void RwControlLocal::updateCodecs(const RwControlConfigCodecs &codecs)
{
    auto msg    = new RwControlUpdateCodecsMessage;
    msg->codecs = codecs;
    remote_->postMessage(msg);
}

void RwControlLocal::setTransmit(const RwControlTransmit &transmit)
{
    auto msg      = new RwControlTransmitMessage;
    msg->transmit = transmit;
    remote_->postMessage(msg);
}

void RwControlLocal::setRecord(const RwControlRecord &record)
{
    auto msg    = new RwControlRecordMessage;
    msg->record = record;
    remote_->postMessage(msg);
}

void RwControlLocal::rtpAudioIn(const PRtpPacket &packet) { remote_->rtpAudioIn(packet); }

void RwControlLocal::rtpVideoIn(const PRtpPacket &packet) { remote_->rtpVideoIn(packet); }

// note: this is executed in the remote thread
gboolean RwControlLocal::cb_doCreateRemote(gpointer data)
{
    return static_cast<RwControlLocal *>(data)->doCreateRemote();
}

// note: this is executed in the remote thread
gboolean RwControlLocal::doCreateRemote()
{
    QMutexLocker locker(&m);
    timer   = nullptr;
    remote_ = new RwControlRemote(thread_->mainContext(), this);
    w.wakeOne();
    return FALSE;
}

// note: this is executed in the remote thread
gboolean RwControlLocal::cb_doDestroyRemote(gpointer data)
{
    return static_cast<RwControlLocal *>(data)->doDestroyRemote();
}

// note: this is executed in the remote thread
gboolean RwControlLocal::doDestroyRemote()
{
    QMutexLocker locker(&m);
    timer = nullptr;
    delete remote_;
    remote_ = nullptr;
    w.wakeOne();
    return FALSE;
}

void RwControlLocal::processMessages()
{
    in_mutex.lock();
    wake_pending                   = false;
    QList<RwControlMessage *> list = in;
    in.clear();
    in_mutex.unlock();

    QPointer<QObject> self = this;

    // we only care about the latest preview frame
    RwControlFrameMessage *fmsg;
    fmsg = getLatestFrameAndRemoveOthers(&list, RwControlFrame::Preview);
    if (fmsg) {
        QImage i = fmsg->frame.image;
        delete fmsg;
        emit previewFrame(i);
        if (!self) {
            qDeleteAll(list);
            return;
        }
    }

    // we only care about the latest output frame
    fmsg = getLatestFrameAndRemoveOthers(&list, RwControlFrame::Output);
    if (fmsg) {
        QImage i = fmsg->frame.image;
        delete fmsg;
        emit outputFrame(i);
        if (!self) {
            qDeleteAll(list);
            return;
        }
    }

    // we only care about the latest audio output intensity
    RwControlAudioIntensityMessage *amsg
        = getLatestAudioIntensityAndRemoveOthers(&list, RwControlAudioIntensity::Output);
    if (amsg) {
        int i = amsg->intensity.value;
        delete amsg;
        emit audioOutputIntensityChanged(i);
        if (!self) {
            qDeleteAll(list);
            return;
        }
    }

    // we only care about the latest audio input intensity
    amsg = getLatestAudioIntensityAndRemoveOthers(&list, RwControlAudioIntensity::Input);
    if (amsg) {
        int i = amsg->intensity.value;
        delete amsg;
        emit audioInputIntensityChanged(i);
        if (!self) {
            qDeleteAll(list);
            return;
        }
    }

    // process the remaining messages
    while (!list.isEmpty()) {
        RwControlMessage *msg = list.takeFirst();
        if (msg->type == RwControlMessage::Status) {
            auto            smsg   = static_cast<RwControlStatusMessage *>(msg);
            RwControlStatus status = smsg->status;
            delete smsg;
            emit statusReady(status);
            if (!self) {
                qDeleteAll(list);
                return;
            }
        } else
            delete msg;
    }
}

// note: this may be called from the remote thread
void RwControlLocal::postMessage(RwControlMessage *msg)
{
    QMutexLocker locker(&in_mutex);

    // if this is a frame, and the queue is maxed, then bump off the
    //   oldest frame to make room
    if (msg->type == RwControlMessage::Frame) {
        auto fmsg     = static_cast<RwControlFrameMessage *>(msg);
        int  firstPos = -1;
        if (queuedFrameInfo(in, fmsg->frame.type, &firstPos) >= QUEUE_FRAME_MAX)
            in.removeAt(firstPos);
    }

    in += msg;
    if (!wake_pending) {
        QMetaObject::invokeMethod(this, "processMessages", Qt::QueuedConnection);
        wake_pending = true;
    }
}

//----------------------------------------------------------------------------
// RwControlRemote
//----------------------------------------------------------------------------
RwControlRemote::RwControlRemote(GMainContext *mainContext, RwControlLocal *local) :
    timer(nullptr), start_requested(false), blocking(false), pending_status(false)
{
    mainContext_                    = mainContext;
    local_                          = local;
    worker                          = new RtpWorker(mainContext_);
    worker->app                     = this;
    worker->cb_started              = cb_worker_started;
    worker->cb_updated              = cb_worker_updated;
    worker->cb_stopped              = cb_worker_stopped;
    worker->cb_finished             = cb_worker_finished;
    worker->cb_error                = cb_worker_error;
    worker->cb_audioOutputIntensity = cb_worker_audioOutputIntensity;
    worker->cb_audioInputIntensity  = cb_worker_audioInputIntensity;
    worker->cb_previewFrame         = cb_worker_previewFrame;
    worker->cb_outputFrame          = cb_worker_outputFrame;
    worker->cb_rtpAudioOut          = cb_worker_rtpAudioOut;
    worker->cb_rtpVideoOut          = cb_worker_rtpVideoOut;
    worker->cb_recordData           = cb_worker_recordData;
}

RwControlRemote::~RwControlRemote()
{
    delete worker;

    qDeleteAll(in);
}

gboolean RwControlRemote::cb_processMessages(gpointer data)
{
    return static_cast<RwControlRemote *>(data)->processMessages();
}

void RwControlRemote::cb_worker_started(void *app) { static_cast<RwControlRemote *>(app)->worker_started(); }

void RwControlRemote::cb_worker_updated(void *app) { static_cast<RwControlRemote *>(app)->worker_updated(); }

void RwControlRemote::cb_worker_stopped(void *app) { static_cast<RwControlRemote *>(app)->worker_stopped(); }

void RwControlRemote::cb_worker_finished(void *app) { static_cast<RwControlRemote *>(app)->worker_finished(); }

void RwControlRemote::cb_worker_error(void *app) { static_cast<RwControlRemote *>(app)->worker_error(); }

void RwControlRemote::cb_worker_audioOutputIntensity(int value, void *app)
{
    static_cast<RwControlRemote *>(app)->worker_audioOutputIntensity(value);
}

void RwControlRemote::cb_worker_audioInputIntensity(int value, void *app)
{
    static_cast<RwControlRemote *>(app)->worker_audioInputIntensity(value);
}

void RwControlRemote::cb_worker_previewFrame(const RtpWorker::Frame &frame, void *app)
{
    static_cast<RwControlRemote *>(app)->worker_previewFrame(frame);
}

void RwControlRemote::cb_worker_outputFrame(const RtpWorker::Frame &frame, void *app)
{
    static_cast<RwControlRemote *>(app)->worker_outputFrame(frame);
}

void RwControlRemote::cb_worker_rtpAudioOut(const PRtpPacket &packet, void *app)
{
    static_cast<RwControlRemote *>(app)->worker_rtpAudioOut(packet);
}

void RwControlRemote::cb_worker_rtpVideoOut(const PRtpPacket &packet, void *app)
{
    static_cast<RwControlRemote *>(app)->worker_rtpVideoOut(packet);
}

void RwControlRemote::cb_worker_recordData(const QByteArray &packet, void *app)
{
    static_cast<RwControlRemote *>(app)->worker_recordData(packet);
}

gboolean RwControlRemote::processMessages()
{
    m.lock();
    timer = nullptr;
    m.unlock();

    while (true) {
        m.lock();
        if (in.isEmpty()) {
            m.unlock();
            break;
        }

        // if there is a stop message in the queue, remove all others
        //   because they are unnecessary
        simplifyQueue(&in);

        RwControlMessage *msg
            = in.takeFirst(); // FIXME we crashed here once likely because cb_processMessages was called too late
        m.unlock();

        bool ret = processMessage(msg);
        delete msg;

        if (!ret) {
            m.lock();
            blocking = true;
            if (timer) {
                g_source_destroy(timer);
                timer = nullptr;
            }
            m.unlock();
            break;
        }
    }

    return FALSE;
}

bool RwControlRemote::processMessage(RwControlMessage *msg)
{
    if (msg->type == RwControlMessage::Start) {
        auto smsg = static_cast<RwControlStartMessage *>(msg);

        applyDevicesToWorker(worker, smsg->devices);
        applyCodecsToWorker(worker, smsg->codecs);

        start_requested = true;
        pending_status  = true;
        worker->start();
        return false;
    } else if (msg->type == RwControlMessage::Stop) {
        auto smsg = static_cast<RwControlStopMessage *>(msg);
        Q_UNUSED(smsg);

        if (start_requested) {
            pending_status = true;
            worker->stop();
        } else {
            // this can happen if we stop before we even start.
            //   just send back a stopped status and don't muck
            //   with the worker.
            auto msg            = new RwControlStatusMessage;
            msg->status.stopped = true;
            local_->postMessage(msg);
        }

        return false;
    } else if (msg->type == RwControlMessage::UpdateDevices) {
        auto umsg = static_cast<RwControlUpdateDevicesMessage *>(msg);

        applyDevicesToWorker(worker, umsg->devices);

        worker->update();
        return false;
    } else if (msg->type == RwControlMessage::UpdateCodecs) {
        auto umsg = static_cast<RwControlUpdateCodecsMessage *>(msg);

        applyCodecsToWorker(worker, umsg->codecs);

        pending_status = true;
        worker->update();
        return false;
    } else if (msg->type == RwControlMessage::Transmit) {
        auto tmsg = static_cast<RwControlTransmitMessage *>(msg);

        if (tmsg->transmit.useAudio)
            worker->transmitAudio();
        else
            worker->pauseAudio();

        if (tmsg->transmit.useVideo)
            worker->transmitVideo();
        else
            worker->pauseVideo();
    } else if (msg->type == RwControlMessage::Record) {
        auto rmsg = static_cast<RwControlRecordMessage *>(msg);

        if (rmsg->record.enabled)
            worker->recordStart();
        else
            worker->recordStop();
    }

    return true;
}

void RwControlRemote::worker_started()
{
    pending_status              = false;
    RwControlStatusMessage *msg = statusFromWorker(worker);
    local_->postMessage(msg);
    resumeMessages();
}

void RwControlRemote::worker_updated()
{
    // only reply with status message if we were asking for one
    if (pending_status) {
        pending_status              = false;
        RwControlStatusMessage *msg = statusFromWorker(worker);
        local_->postMessage(msg);
    }

    resumeMessages();
}

void RwControlRemote::worker_stopped()
{
    pending_status              = false;
    RwControlStatusMessage *msg = statusFromWorker(worker);
    msg->status.stopped         = true;
    local_->postMessage(msg);
}

void RwControlRemote::worker_finished()
{
    RwControlStatusMessage *msg = statusFromWorker(worker);
    msg->status.finished        = true;
    local_->postMessage(msg);
}

void RwControlRemote::worker_error()
{
    RwControlStatusMessage *msg = statusFromWorker(worker);
    msg->status.error           = true;
    msg->status.errorCode       = worker->error;
    local_->postMessage(msg);
}

void RwControlRemote::worker_audioOutputIntensity(int value)
{
    auto msg             = new RwControlAudioIntensityMessage;
    msg->intensity.type  = RwControlAudioIntensity::Output;
    msg->intensity.value = value;
    local_->postMessage(msg);
}

void RwControlRemote::worker_audioInputIntensity(int value)
{
    auto msg             = new RwControlAudioIntensityMessage;
    msg->intensity.type  = RwControlAudioIntensity::Input;
    msg->intensity.value = value;
    local_->postMessage(msg);
}

void RwControlRemote::worker_previewFrame(const RtpWorker::Frame &frame)
{
    auto msg         = new RwControlFrameMessage;
    msg->frame.type  = RwControlFrame::Preview;
    msg->frame.image = frame.image;
    local_->postMessage(msg);
}

void RwControlRemote::worker_outputFrame(const RtpWorker::Frame &frame)
{
    auto msg         = new RwControlFrameMessage;
    msg->frame.type  = RwControlFrame::Output;
    msg->frame.image = frame.image;
    local_->postMessage(msg);
}

void RwControlRemote::worker_rtpAudioOut(const PRtpPacket &packet)
{
    if (local_->cb_rtpAudioOut)
        local_->cb_rtpAudioOut(packet, local_->app);
}

void RwControlRemote::worker_rtpVideoOut(const PRtpPacket &packet)
{
    if (local_->cb_rtpVideoOut)
        local_->cb_rtpVideoOut(packet, local_->app);
}

void RwControlRemote::worker_recordData(const QByteArray &packet)
{
    if (local_->cb_recordData)
        local_->cb_recordData(packet, local_->app);
}

void RwControlRemote::resumeMessages()
{
    QMutexLocker locker(&m);
    if (blocking) {
        blocking = false;
        if (!in.isEmpty() && !timer) {
            timer = g_timeout_source_new(0);
            g_source_set_callback(timer, cb_processMessages, this, nullptr);
            g_source_attach(timer, mainContext_);
        }
    }
}

// note: this may be called from the local thread
void RwControlRemote::postMessage(RwControlMessage *msg)
{
    QMutexLocker locker(&m);

    // if a stop message is sent, unblock so that it can get processed.
    //   this is so we can stop a session that is in the middle of
    //   starting.  note: care must be taken in the message handler, as
    //   this will cause processing to resume before resumeMessages() has
    //   been called.
    if (msg->type == RwControlMessage::Stop)
        blocking = false;

    in += msg;

    if (!blocking && !timer) {
        timer = g_timeout_source_new(0);
        g_source_set_callback(timer, cb_processMessages, this, nullptr);
        g_source_attach(timer, mainContext_);
    }
}

// note: this may be called from the local thread
void RwControlRemote::rtpAudioIn(const PRtpPacket &packet) { worker->rtpAudioIn(packet); }

// note: this may be called from the local thread
void RwControlRemote::rtpVideoIn(const PRtpPacket &packet) { worker->rtpVideoIn(packet); }

}
