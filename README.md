# VNC Ubuntu Virtual Monitor

Current development version: **0.0.25**.

This project exposes a true virtual monitor from the current GNOME Wayland session to an old VNC client. The target use case is an old iPad used strictly as an additional display, not as an input/control surface.

Git tags use the usual `v` prefix (`v0.0.25`), while the program reports the version without it (`0.0.25`).

## Production architecture

```text
old iPad VNC client
    |
    | RFB 3.8 + RA2r / AES-128-EAX
    | encrypted username/password
    v
RA2r front-end :5901
    |
    +----> /run/vnc-monitor-auth.sock
    |          privileged PAM helper
    |
    | decrypted RFB, loopback only
    v
LibVNCServer backend 127.0.0.1:5903
    |
    |  small/simple delta -> ZRLE
    |  heavy delta        -> JPEG encoding 21
    |  idle               -> progressive lossless repair
    v
FrameBridge / framebuffer diff
    ^
    |
native PipeWire consumer
    ^
    |
Mutter RecordVirtual
    ^
    |
current GNOME Wayland session
```

The virtual monitor exists only while an authenticated VNC client is connected. No X11 session, remote-login desktop or separate desktop session is created.

## Mutter virtual monitor

The monitor is created in the existing GNOME Wayland session through:

```text
RemoteDesktop.CreateSession
    -> ScreenCast.CreateSession(remote-desktop-session-id=...)
    -> ScreenCast.Session.RecordVirtual(cursor-mode=..., is-platform=true)
    -> RemoteDesktop.Session.Start
    -> PipeWireStreamAdded(node_id)
```

`ScreenCast.Session.Start` is intentionally not called separately. Starting the RemoteDesktop session starts the associated stream.

Stopping the RemoteDesktop session removes the virtual monitor.

## Capture path

Native libpipewire capture is the production/default backend:

```text
--capture-backend pipewire
```

The PipeWire `process()` callback drains buffers, copies valid BGRx pixels into owned memory and requeues each `pw_buffer` immediately. A `struct pw_buffer *` never survives the callback.

A PipeWire chunk with either:

```text
chunk->size == 0
```

or:

```text
SPA_CHUNK_FLAG_CORRUPTED
```

is treated as containing no new video pixels. Its memory may contain stale data from an earlier buffer use and is not copied.

The older GStreamer path remains only for A/B diagnostics:

```bash
./vnc-monitor-test --capture-backend gstreamer
```

## Cursor path

For the production Mutter path use cursor metadata:

```text
--mutter-cursor metadata
```

The PipeWire consumer reads `SPA_META_Cursor`. Base framebuffer pixels and cursor state are kept separately and the cursor is composed in software. Cursor-only metadata can therefore produce a small framebuffer delta without waiting for a new full video frame.

Do not re-enable `MUTTER_DEBUG_DISABLE_HW_CURSORS=1`; the current design does not require it.

## 0.0.25 adaptive transport

0.0.25 keeps the 0.0.24 latest-only/backpressure governor but changes the meaning of resynchronization completely.

### Lossless state does not need a keyframe after a skipped source frame

For normal ZRLE updates, framebuffer diff compares the newest FrameBridge image with the last source state already submitted to the backend framebuffer. Intermediate source frames can therefore be skipped safely:

```text
last submitted source state
        |
        | direct diff
        v
newest source state
```

A source sequence gap, transport backpressure or a large lossless change does **not** by itself mean that the client framebuffer is corrupt.

A normal period with no PipeWire video frames is also not a capture failure. `capture-stall`/idle-gap detection is diagnostic only and does not schedule a repair or full-frame keyframe.

### Adaptive codec selection

The old iPad advertises RFB JPEG encoding `21`. LibVNCServer 0.9.15 does not implement that encoding itself, so the project registers encoding 21 through the LibVNCServer extension mechanism and emits the JPEG rectangle directly into the same internal RFB connection.

The RA2r relay remains an opaque encrypted transport and does not parse or transcode framebuffer updates.

Current experimental defaults:

```text
JPEG encoding:          21
JPEG quality:           80
heavy-change threshold: 25% of framebuffer pixels
```

Policy:

```text
small/simple delta
    -> normal LibVNCServer ZRLE
    -> lossless

heavy delta >= 25%
    -> JPEG encoding 21
    -> lossy, lower bandwidth

idle
    -> progressive ZRLE repair
    -> restores exact pixels
```

Only a new delta that is itself heavy joins a pending JPEG update. Small UI/cursor deltas keep their normal lossless priority even when a JPEG rectangle is waiting for the client/transport.

The JPEG sender remains RFB demand-driven: it sends only when the viewer has an outstanding `FramebufferUpdateRequest`, no normal LibVNCServer update is pending, and transport queues are at low water.

### Exactness / repair bitmap

After a JPEG rectangle is actually sent, its covered `32x32` tiles are marked inexact. This is separate from the source framebuffer: the source can already contain the newest exact pixels while the iPad temporarily contains their JPEG approximation.

Lossless live updates clear fully covered repair tiles. Remaining tiles are repaired only while the source is quiet and transport is free.

Current experimental repair defaults:

```text
repair tile:       32x32
idle before repair: 250 ms
repair budget:      4096 kbit/s raw-equivalent pacing
```

The repair order is spatially progressive rather than top-to-bottom:

```text
pass 0 -> sparse 4x4 grid across the whole damaged area
pass 1 -> remaining even/even positions
pass 2 -> checkerboard complement
pass 3 -> remaining tiles
```

Only one small repair tile is put in flight at a time. Any live framebuffer work naturally has priority over subsequent repair tiles.

Useful telemetry:

```text
[ADAPT] client advertised RFB JPEG encoding 21; adaptive JPEG enabled
[ADAPT] JPEG21 seq=... rect=... quality=80 bytes=... raw=... ratio=... repair=...
[REPAIR] schedule pass=... tile=... remaining=...
[REPAIR] exact pass=... tile=... remaining=...
[CAPTURE] idle-gap=...; no repair/keyframe scheduled
[ADAPT][SUMMARY] jpeg-updates=... jpeg-bytes=... raw-equivalent=... saved=... repair-tiles=...
```

The initial framebuffer is still sent losslessly. Legacy keyframe-related CLI options are currently retained for command-line compatibility, but source drops, backpressure, large changes and ordinary capture gaps no longer arm a repair keyframe in the production Mutter path.

## Latest-only / backpressure governor

When downstream transport is congested, new VNC updates are not manufactured. FrameBridge continues to receive current source state, and after recovery the publisher compares the last consumed source framebuffer directly with the newest one.

High-water conditions currently include:

```text
external outq >= configured external SO_SNDBUF
backend inq  >= configured backend SO_RCVBUF
TCP unacked  >= 24
```

Once congestion is entered, publishing resumes only near the low-water state:

```text
external outq <= about 1/4 buffer
backend inq  <= about 1/4 buffer
TCP unacked  <= 8
```

This hysteresis prevents repeated pressure/clear oscillation.

Do not tune socket buffer sizes, RA2 record size or coalescing delay as a first response to high-entropy image latency; 0.0.25 targets the much larger lossless-image payload directly.

## Build

```bash
make clean
make -j20
```

The main build uses development headers/libraries for:

```text
libvncserver
libjpeg
openssl
nettle
glib-2.0
gio-2.0
gstreamer-1.0
gstreamer-app-1.0
gstreamer-video-1.0
libpipewire-0.3
```

On Debian/Ubuntu the JPEG development package is normally `libjpeg-dev` (or the distribution-compatible libjpeg-turbo development package).

The PAM helper additionally requires PAM development headers/libraries.

Check the program version:

```bash
./vnc-monitor-test --version
```

Show runtime options/defaults:

```bash
./vnc-monitor-test --help
./vnc-monitor-test --show-config
```

## Run

Production-direction run:

```bash
./vnc-monitor-test --mutter-cursor metadata
```

Important current settings:

```text
source=mutter
capture-backend=pipewire
framebuffer=1024x768
max source FPS=60
VNC publisher=source-driven
latest-only=on
diff-detect=on
diff-tile-size=32
Mutter cursor=metadata (production invocation)
Mutter HW cursor=auto
RFB backend=127.0.0.1:5903
RA2r public port=5901
RA2 record max=16384
RA2 coalescing=on, 500 us
view-only=on
clipboard input=off
file transfer=off
```

For detailed scheduler/latency diagnostics:

```bash
./vnc-monitor-test --mutter-cursor metadata --latency-trace on --frame-trace on
```

Synthetic source and GStreamer remain diagnostic paths:

```bash
./vnc-monitor-test --source test
./vnc-monitor-test --capture-backend gstreamer
```

## PAM authentication helper

Build/render support files without changing the system:

```bash
make pam-service
```

Install the privileged helper, PAM policy and systemd socket/service:

```bash
make install-pam-service
```

Do not run the whole Make process through `sudo`; the install target invokes `sudo` only for operations that require root privileges and keeps socket ownership tied to the invoking user.

Production support components:

```text
/usr/local/libexec/vnc-monitor-auth-helper
/etc/pam.d/vnc-monitor
/etc/systemd/system/vnc-monitor-auth.socket
/etc/systemd/system/vnc-monitor-auth@.service
/run/vnc-monitor-auth.sock
```

There is no forced-redraw GNOME Shell helper/service in the current architecture. Old 0.0.17/0.0.18 redraw/HW-cursor diagnostic leftovers can be removed with:

```bash
make cleanup-obsolete-support
```

This cleanup intentionally keeps the PAM authentication service/socket.

## View-only security boundary

The external connection is RA2r-encrypted and authenticated through the local privileged PAM helper.

The internal LibVNCServer backend:

- binds only to `127.0.0.1`;
- uses RFB security type `None` only on loopback;
- is never intended to be reachable from the LAN.

Server-side input policy is strict view-only:

- keyboard ignored;
- pointer/touch ignored;
- client clipboard input disabled;
- file transfer disabled.

## RA2 server identity

The persistent RA2 RSA identity is created at runtime as:

```text
./ra2-server-key.pem
```

It is ignored by the current working tree and must remain private.

A private key existed in earlier Git history. Before publishing the repository, rewrite the Git history to remove it, generate a new RA2 server identity and re-confirm the new identity on the iPad.

## Versioning

The project is pre-alpha/prototype software and currently uses:

```text
program version: 0.0.x
git tag:         v0.0.x
```

Current development version:

```text
program version: 0.0.25
```
