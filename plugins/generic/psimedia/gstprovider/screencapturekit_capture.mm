// SPDX-License-Identifier: LGPL-2.1-or-later
#include "screencapturekit_capture_p.h"
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <gst/app/gstappsrc.h>
#include <memory>
#include <mutex>

namespace {
struct CaptureState {
    GWeakRef   owner;
    std::mutex mutex;
    bool       stopped = false;
    explicit CaptureState(GstElement *source) { g_weak_ref_init(&owner, G_OBJECT(source)); }
    ~CaptureState() { g_weak_ref_clear(&owner); }
    bool cancelled()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped;
    }
    void cancel()
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true;
    }
    void fail(const char *message)
    {
        if (cancelled())
            return;
        auto *source = GST_ELEMENT(g_weak_ref_get(&owner));
        if (!source)
            return;
        GST_ELEMENT_ERROR(source, RESOURCE, READ, ("%s", message), ("ScreenCaptureKit capture failed"));
        gst_app_src_end_of_stream(GST_APP_SRC(source));
        gst_object_unref(source);
    }
};
}

API_AVAILABLE(macos(12.3))
@interface PsiScreenCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
    std::shared_ptr<CaptureState> state_;
    SCStream                     *stream_;
    uint32_t                      displayId_, windowId_;
}
- (instancetype)initWithState:(std::shared_ptr<CaptureState>)state display:(uint32_t)display window:(uint32_t)window;
- (void)start;
- (void)openContent:(SCShareableContent *)content error:(NSError *)error;
- (void)stop;
@end

@implementation PsiScreenCapture
- (instancetype)initWithState:(std::shared_ptr<CaptureState>)state display:(uint32_t)display window:(uint32_t)window
{
    if ((self = [super init])) {
        state_     = std::move(state);
        displayId_ = display;
        windowId_  = window;
    }
    return self;
}
- (void)start
{
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                               onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *content, NSError *error) {
                                                     [self openContent:content error:error];
                                                 }];
}
- (void)openContent:(SCShareableContent *)content error:(NSError *)error
{
    // Serializes pending enumeration/start against stop. A cancelled selection
    // can never start a stream later in a completion block.
    @synchronized(self) {
        if (state_->cancelled())
            return;
        if (error) {
            state_->fail(error.localizedDescription.UTF8String);
            return;
        }
        SCContentFilter *filter = nil;
        double           width = 0, height = 0;
        if (windowId_) {
            for (SCWindow *window in content.windows) {
                if (window.windowID == windowId_) {
                    filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
                    // Pick the display containing most of this window. Native
                    // pixel coordinates must not assume a 2x Retina scale.
                    double area = 0, pixelScale = 1;
                    for (SCDisplay *display in content.displays) {
                        const CGRect overlap   = CGRectIntersection(window.frame, display.frame);
                        const double candidate = overlap.size.width * overlap.size.height;
                        const CGRect bounds    = CGDisplayBounds(display.displayID);
                        if (!CGRectIsNull(overlap) && candidate > area && bounds.size.width > 0) {
                            area       = candidate;
                            pixelScale = double(CGDisplayPixelsWide(display.displayID)) / bounds.size.width;
                        }
                    }
                    width  = window.frame.size.width * pixelScale;
                    height = window.frame.size.height * pixelScale;
                    break;
                }
            }
        } else {
            for (SCDisplay *display in content.displays) {
                if (display.displayID == displayId_) {
                    filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
                    width  = CGDisplayPixelsWide(display.displayID);
                    height = CGDisplayPixelsHigh(display.displayID);
                    break;
                }
            }
        }
        if (!filter || width < 1 || height < 1) {
            state_->fail("The selected display or window is no longer available");
            return;
        }
        const double           scale         = std::min({ 1.0, 1920.0 / width, 1080.0 / height });
        SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
        configuration.width                  = std::max(2, int(std::floor(width * scale / 2)) * 2);
        configuration.height                 = std::max(2, int(std::floor(height * scale / 2)) * 2);
        configuration.pixelFormat            = kCVPixelFormatType_32BGRA;
        configuration.minimumFrameInterval   = CMTimeMake(1, 15);
        configuration.queueDepth             = 3;
        configuration.showsCursor            = YES;
        // Only video: microphone and other call contents stay independent.
        stream_      = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:self];
        auto *source = GST_ELEMENT(g_weak_ref_get(&state_->owner));
        if (!source)
            return;
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRA", "width", G_TYPE_INT,
                                            int(configuration.width), "height", G_TYPE_INT, int(configuration.height),
                                            "framerate", GST_TYPE_FRACTION, 15, 1, "pixel-aspect-ratio",
                                            GST_TYPE_FRACTION, 1, 1, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        gst_caps_unref(caps);
        gst_object_unref(source);
        NSError *outputError = nil;
        if (![stream_ addStreamOutput:self
                                 type:SCStreamOutputTypeScreen
                   sampleHandlerQueue:dispatch_queue_create("org.psi-im.screen-capture.frames", DISPATCH_QUEUE_SERIAL)
                                error:&outputError]) {
            state_->fail(outputError.localizedDescription.UTF8String);
            stream_ = nil;
            return;
        }
        SCStream *startingStream = stream_;
        [startingStream startCaptureWithCompletionHandler:^(NSError *startError) {
            if (state_->cancelled())
                [startingStream stopCaptureWithCompletionHandler:^(NSError *) {}];
            else if (startError)
                state_->fail(startError.localizedDescription.UTF8String);
        }];
    }
}
- (void)stop
{
    state_->cancel();
    @synchronized(self) {
        SCStream *stream = stream_;
        stream_          = nil;
        if (stream) {
            [stream removeStreamOutput:self type:SCStreamOutputTypeScreen error:nullptr];
            [stream stopCaptureWithCompletionHandler:^(NSError *) {}];
        }
    }
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    state_->fail(error.localizedDescription.UTF8String);
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeScreen || state_->cancelled() || !CMSampleBufferIsValid(sample))
        return;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments)) {
        NSDictionary *info   = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
        NSNumber     *status = info[SCStreamFrameInfoStatus];
        if (status && status.integerValue == SCFrameStatusStopped) {
            state_->fail("The selected screen capture was stopped");
            return;
        }
        if (status && status.integerValue != SCFrameStatusComplete)
            return;
    }
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (!pixels || CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA
        || CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
        return;
    const size_t width = CVPixelBufferGetWidth(pixels), height = CVPixelBufferGetHeight(pixels);
    const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
    GstBuffer   *buffer = gst_buffer_new_allocate(nullptr, width * height * 4, nullptr);
    GstMapInfo   mapping;
    if (buffer && gst_buffer_map(buffer, &mapping, GST_MAP_WRITE)) {
        const auto *base = static_cast<const unsigned char *>(CVPixelBufferGetBaseAddress(pixels));
        for (size_t row = 0; row < height; ++row)
            std::memcpy(mapping.data + row * width * 4, base + row * stride, width * 4);
        gst_buffer_unmap(buffer, &mapping);
    } else {
        if (buffer)
            gst_buffer_unref(buffer);
        buffer = nullptr;
    }
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    if (!buffer)
        return;
    if (state_->cancelled()) {
        gst_buffer_unref(buffer);
        return;
    }
    auto *source = GST_ELEMENT(g_weak_ref_get(&state_->owner));
    if (source) {
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 15;
        // appsrc owns the buffer, including during flushing. Its bounded leaky
        // queue drops old frames instead of blocking the native capture queue.
        gst_app_src_push_buffer(GST_APP_SRC(source), buffer);
        gst_object_unref(source);
    } else {
        gst_buffer_unref(buffer);
    }
}
@end

void *startScreenCaptureKit(GstElement *source, unsigned display, unsigned window)
{
    if (@available(macOS 12.3, *)) {
        auto              state      = std::make_shared<CaptureState>(source);
        PsiScreenCapture *controller = [[PsiScreenCapture alloc] initWithState:state display:display window:window];
        [controller start];
        return (__bridge_retained void *)controller;
    }
    GST_ELEMENT_ERROR(source, RESOURCE, NOT_FOUND, ("Screen sharing requires macOS 12.3 or later"), (nullptr));
    return nullptr;
}
void stopScreenCaptureKit(void *capture)
{
    if (@available(macOS 12.3, *)) {
        PsiScreenCapture *controller = (__bridge_transfer PsiScreenCapture *)capture;
        [controller stop];
    }
}
