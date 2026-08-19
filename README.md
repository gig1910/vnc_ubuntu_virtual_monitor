# VNC Ubuntu Virtual Monitor

Current development version: **0.0.21**.

This project exposes a true virtual monitor from the current GNOME Wayland session to an old VNC client. The target use case is an old iPad used strictly as an additional display, not as an input/control surface.

The previous V20 naming is normalized to **0.0.20**. Git tags should use the usual `v` prefix, for example `v0.0.20`, while the program version itself is `0.0.20` / `0.0.21`.

## Architecture

```text
iPad VNC client
    |
    | RFB 3.8 + RA2r / AES-128-EAX
    | Ubuntu username/password
    v
RA2r front-end :5901
    |
    +----> /run/vnc-monitor-auth.sock
    |          privileged PAM helper
    |
    | plain RFB on loopback only
    v
LibVNCServer backend 127.0.0.1:5903
    |
    v
latest-only VNC publisher / periodic full-frame resync
    |
    v
FrameBridge
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

The virtual monitor exists only while an authenticated VNC client is connected.

## 0.0.21

0.0.21 keeps the native libpipewire capture backend introduced in 0.0.20 and adds a downstream freshness policy.

### Native PipeWire capture remains the default

```text
--capture-backend pipewire
```

The PipeWire callback drains to the newest available buffer, copies BGRx to owned memory and immediately recycles the `pw_buffer` before downstream VNC work.

The previous GStreamer path remains available only for A/B diagnostics:

```bash
./vnc-monitor-test --capture-backend gstreamer
```

### Latest-only downstream governor

```text
--latest-only on
```

When the RA2/backend transport is visibly backpressured, the VNC publisher does not consume intermediate FrameBridge states. It keeps the LibVNCServer framebuffer as the client reference state and waits until the transport recovers. It then consumes the newest available source frame directly.

This avoids deliberately building a queue of stale desktop states.

### Full-frame keyframes / resync

VNC does not have H.264 I-frames, so the equivalent used here is a full framebuffer update.

Defaults:

```text
--keyframe on
--keyframe-interval-ms 2000
--keyframe-after-drop on
```

A full-frame resync is forced:

- for the initial real frame;
- periodically, on the next available source frame after the configured interval;
- after source-frame collapse/drop;
- after downstream backpressure;
- after a detected capture stall.

The important correctness rule is that the local LibVNCServer framebuffer is not advanced while stale source states are being dropped. Therefore the next diff is always calculated from the last submitted client-reference state to the newest source state. A full resync is additionally used after drops/backpressure to eliminate any possibility of accumulated visual artifacts.

Keyframe diagnostics look like:

```text
[KF] seq=1234 reason=periodic full=1024x768
[KF] seq=1301 reason=backpressure full=1024x768
[KF] seq=1408 reason=capture-stall full=1024x768
```

The governor reports transitions such as:

```text
[GOV] backpressure: defer publish, keep latest only ...
[GOV] backpressure cleared: publishing newest frame
```

and prints a shutdown summary:

```text
[GOV][SUMMARY] keyframes=... backpressure-events=... dropped-source=...
```

### Latency diagnostics

Enable per-publish diagnostics with:

```text
--latency-trace on
```

This reports source-sequence collapse, keyframe state, diff/RFB processing time, and the currently observed transport queues. It is intentionally diagnostic and is off by default.

## Build

```bash
make clean
make -j20
```

Expected pkg-config development dependencies:

```text
libvncserver
openssl
nettle
glib-2.0
gio-2.0
gstreamer-1.0
gstreamer-app-1.0
gstreamer-video-1.0
libpipewire-0.3
```

The PAM helper additionally requires PAM development headers/libraries.

Check the program version:

```bash
./vnc-monitor-test --version
```

Show all runtime options:

```bash
./vnc-monitor-test --help
```

Show resolved defaults:

```bash
./vnc-monitor-test --show-config
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

Do not run the whole Make process through `sudo`; the install target invokes `sudo` only for operations that need root privileges and keeps socket ownership tied to the invoking user.

Status:

```bash
make pam-service-status
```

The production support components are:

```text
/usr/local/libexec/vnc-monitor-auth-helper
/etc/pam.d/vnc-monitor
/etc/systemd/system/vnc-monitor-auth.socket
/etc/systemd/system/vnc-monitor-auth@.service
/run/vnc-monitor-auth.sock
```

There is no forced-redraw GNOME Shell helper in the current architecture.

Legacy 0.0.17/0.0.18 redraw/HW-cursor diagnostic leftovers can be removed with:

```bash
make cleanup-obsolete-support
```

This cleanup intentionally keeps the PAM authentication service/socket.

## Run

Normal production-direction test:

```bash
./vnc-monitor-test
```

Relevant defaults:

```text
source=mutter
capture-backend=pipewire
1024x768
max source FPS=60
latest-only=on
keyframe=on
keyframe interval=2000 ms
keyframe after drop=on
RFB backend=127.0.0.1:5903
RA2r public port=5901
RA2 record max=16384
RA2 coalescing=on, 500 us
view-only=on
clipboard input=off
file transfer=off
```

For the synthetic source:

```bash
./vnc-monitor-test --source test
```

For the old capture path:

```bash
./vnc-monitor-test --capture-backend gstreamer
```

For detailed latency/governor diagnostics:

```bash
./vnc-monitor-test --latency-trace on --frame-trace on
```

## GNOME / Mutter virtual-monitor path

The real monitor is created inside the existing GNOME Wayland session:

```text
RemoteDesktop.CreateSession
    -> ScreenCast.CreateSession(remote-desktop-session-id=...)
    -> RecordVirtual(cursor-mode=..., is-platform=true)
    -> RemoteDesktop.Session.Start
    -> PipeWireStreamAdded(node_id)
    -> native PipeWire capture
```

Stopping the RemoteDesktop session removes the virtual monitor.

The project does not require X11 and does not create a separate remote-login desktop.

## View-only security boundary

The external client receives RA2r-encrypted RFB. Authentication is performed through the local privileged PAM helper.

The internal LibVNCServer backend:

- binds to `127.0.0.1` only;
- uses RFB security type `None` only on loopback;
- is never intended to be reachable from the LAN.

Server-side input policy is strict view-only:

- keyboard input ignored;
- pointer/touch input ignored;
- client clipboard input disabled;
- file transfer disabled.

## RA2 server identity

The persistent RA2 RSA identity is created at runtime as:

```text
./ra2-server-key.pem
```

It is deliberately **not tracked by Git**. Treat it as a private key and keep it out of source-control archives.

If the key is deleted, the next run will create a new identity and the VNC client may ask to trust the server signature again.

## Versioning

This project is still pre-alpha/prototype software. Until the interface and architecture settle, versions use:

```text
0.0.x
```

Examples:

```text
program version: 0.0.20
git tag:         v0.0.20

program version: 0.0.21
git tag:         v0.0.21
```

The historical shorthand `V20`, `V19`, etc. should not be used for new releases or documentation.
