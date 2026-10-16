# Qt Interactive Text Element

Allows to manage interactive elements on QTextEdit.

`ITEVideoController` embeds an inline video player with play/pause, seeking and
elapsed time. It accepts a URL or the same `ITEMediaOpener` used by audio.
The fullscreen button (or double click on the image) opens the same player in a
fullscreen window, without reopening the media. Exit with the button, Escape,
double click or the window close action. Controls appear on mouse movement and
hide after two seconds of inactivity; they remain visible while being used.
Playback is lazy: merely displaying a message does not open the source.
Scrolling the player completely out of view or hiding the editor pauses playback.

The text document holds a fixed-size object, while an opaque viewport child
contains a frame widget and the controls. Video frames and playback progress never
modify the document or request updates on the text viewport. Geometry is updated
on scrolling, resizing and document edits. Removing the object releases its
player and stream, including its fullscreen window. The output is deliberately
an ordinary QWidget, with no native child windows or window containers inside
QTextEdit. This avoids native Wayland exposes reentering painting during layout
and resize. A bounded mailbox delivers the latest frame to the GUI thread.
Qt 6 uses QVideoSink/QVideoFrame::paint; Qt 5 negotiates image-compatible RGB
frames with QAbstractVideoSurface. Painting uses the raster path, so CPU scaling
or GPU readback may cost more than a native video window, but text is not repainted.
Qite does not copy frames into text document resources.

Qt Widgets and Multimedia are required (Qt 5 / Qt 6).
The demo's **Open video** action inserts a local clip.

Audio and video can optionally show a download button. Opt in per element and
connect the controller's `downloadRequested(const QUrl &)` signal to the host's
save action; clicking does not open a decoder or start playback:

```cpp
audio.insert(source, opener, true);
video.insert(source, opener, QSize(320, 220), true);
```

An opener can override `mediaState(source)` to return a host-owned `ITEMediaState`
shared by players of that resource. The default implementation returns `nullptr`.
Set `setCachedRanges({{0, .2}, {.8, 1}})` for disjoint availability ranges and
`setDownload(true, .4)` for an active download. Finish with `setDownload(false)`
or supply an error string to make the button available for retry. Keep the state
and its updates on the GUI thread. Players tolerate its destruction.

The translucent blue layer shows available **file bytes**, with the narrower
playback stripe/handle visible over it. Byte fractions are not exact timestamps
for VBR/container media. The host supplies current coverage, including evictions;
Qite normalizes/merges overlapping ranges but does not accumulate them forever.
Availability and download updates do not modify the document. The video slider
updates only its local widget; audio invalidates only its object's viewport rect.

To build and run the regression tests:

```
cmake -S libqite -B build -DQT_DEFAULT_MAJOR_VERSION=6 -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests run offscreen and check layout/removal. To also exercise decoding, seeking,
pausing and opener stream cleanup with `tests/data/test.webm`, configure with
`-DQITE_TEST_PLAYBACK=ON`. This requires a working WebM/VP8 multimedia backend;
these two tests are explicitly skipped otherwise. This avoids making portable
geometry tests depend on codecs or an audio service.
Tests also inject 120 frames and check that neither the text editor's
paint event, the text object renderer, nor document changes/layout signals fire.
The fixture is generated test content (no third-party media):

```
ffmpeg -f lavfi -i testsrc2=size=160x90:rate=30 -t 2 -an -c:v libvpx -b:v 80k tests/data/test.webm
```
