# Screen capture provider extension

`psimedia/psimediascreencapture.h` defines the optional
`org.psi-im.psimedia.ScreenCaptureProvider/1.0` interface. The host queries it
with `qobject_cast` on the existing provider QObject. The base Provider and RTP
interfaces keep their ABI. This header is independent of GStreamer and of the
base SDK header, so building the plugin does not require a newer base Psi SDK.
The matching header is shipped in Psi's plugin SDK.

The host owns the common source picker and its presentation. The provider owns
platform enumeration, permission handling, system pickers, selected source
configuration, and native resource lifetimes. All context calls and notifications
run on its QObject owner (GUI) thread. Destroy contexts and release source leases
before unloading the plugin.

1. `screenCaptureAvailable()` probes capabilities without enumerating windows or
   requesting permission.
2. `createScreenCapture()` returns a parentless context owned by the caller.
3. Following explicit user action, `startSelection()` begins enumeration or opens
   a system picker. It does not start media transmission.
4. For a host picker, `sourcesAvailable()` announces `sources()`. Each entry has
   an opaque choice ID, kind, label and an optional PNG thumbnail. The host calls
   `choose(id)` after confirmation. A system picker can instead produce `ready`
   directly, without exposing a source list.
5. `ready(deviceId, label)` provides a distinct opaque video device ID. Pass it
   unchanged to `RtpSessionContext::setVideoInputDevice()`. Neither this ID nor a
   choice ID is a native window handle or a media pipeline description.
6. `stop()` cancels pending selection, revokes issued IDs, and emits `stopped()`
   before releasing native permission/resources. Consumers must immediately stop
   transmitting. Retain `sourceLease()` in the media session until its old source
   has been released. Late asynchronous results cannot revive stopped capture.
   Cancellation emits `stopped()`; failures additionally emit `failed(QString)`.

The GStreamer provider implements the same context on every platform:

- X11: private display connection for source enumeration, native XRandR pixel
  rectangles, optional previews, and `ximagesrc` capture.
- Wayland: ScreenCast portal selection and its permission session, followed by
  `pipewiresrc` using the passed Unix FD. Qt D-Bus support is detected in PsiMedia.
- Windows: monitor/window enumeration, DXGI monitor capture and MSVC/WGC window
  capture through `d3d11screencapturesrc`.
- macOS: ScreenCaptureKit enumeration and permission, and the private
  `psiscreencapturekitsrc` element that starts capture only with media activation.

Only the provider resolves a selected device ID to a GStreamer source. The
registry is synchronized across the GUI and media worker threads. Revocation
removes the ID immediately, preventing another media graph from opening it.
Already-created source elements retain their device lease through teardown,
including the PipeWire FD, without retaining portal permission after stop.

Regression tests separate host UI, provider selection/lifetime, portal protocol,
and the RTP pipeline. The camera/screen test uses an opaque screen device ID and
checks concurrent protected RTP and independent screen teardown. Native Windows
and macOS capture/permission behavior requires testing on those operating systems;
mock selection and headless lifecycle tests do not exercise OS capture.
