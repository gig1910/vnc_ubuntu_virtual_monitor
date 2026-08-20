# Architecture

## Goal

Present a real virtual monitor to the **current GNOME Wayland session** and display it on an old view-only VNC client. The monitor exists only for the lifetime of an authenticated viewer connection.

## End-to-end data path

```text
old VNC viewer
    |
    | RFB 3.8, RA2r, encrypted records
    v
RA2r frontend :5901
    |
    | decrypted local RFB
    v
LibVNCServer 127.0.0.1:5903
    |
    | framebuffer updates
    v
adaptive RFB transport
    ^
    | BGRx framebuffer
FrameBridge
    ^
    | copied PipeWire frames + cursor composition
native PipeWire capture
    ^
    | RecordVirtual stream
Mutter ScreenCast + RemoteDesktop
    |
    v
virtual monitor in the current GNOME Wayland session
```

## Virtual-monitor lifecycle

The daemon itself starts with the graphical user session and only opens its public listening socket.

On a successful viewer connection it performs the following Mutter sequence:

1. `org.gnome.Mutter.RemoteDesktop.CreateSession`;
2. read the RemoteDesktop session `SessionId`;
3. `org.gnome.Mutter.ScreenCast.CreateSession` linked with `remote-desktop-session-id`;
4. `ScreenCast.Session.RecordVirtual` with the requested cursor mode and `is-platform=true`;
5. subscribe for `PipeWireStreamAdded`;
6. start the **RemoteDesktop** session;
7. capture the returned PipeWire node.

`ScreenCast.Session.Start` is intentionally not called for the linked RemoteDesktop session.

Stopping the RemoteDesktop session removes the virtual monitor.

## Capture ownership rule

A `struct pw_buffer *` is never retained beyond the PipeWire process callback. Valid BGRx video is copied to private memory and the PipeWire buffer is immediately recycled.

Mutter may produce buffers with an empty/corrupted video chunk while still carrying meaningful `SPA_META_Cursor` data. Cursor metadata is therefore processed independently of video payload validity.

## Cursor model

Production uses cursor metadata rather than an embedded cursor stream.

The capture layer keeps:

- a cursor-free base framebuffer;
- cached cursor bitmap, position and hotspot;
- a scratch framebuffer.

For every publish it copies base → scratch and composites the premultiplied RGBA cursor into BGRx scratch. Cursor-only PipeWire updates can therefore create a new VNC frame even when no video pixels were supplied by Mutter.

## RA2r frontend

The legacy client uses RFB security type 13 (`RA2r`). The implementation supports the path experimentally confirmed with the target viewer:

- 2048-bit RSA public-key exchange;
- server/client random exchange encrypted with RSA;
- AES-128-EAX session keys;
- mutual SHA-1 public-key hash verification;
- RA2 authentication subtype 1;
- encrypted username/password;
- PAM authentication through the local privileged helper socket.

After authentication the frontend acts as a codec-agnostic encrypted RFB relay to the loopback LibVNCServer backend.

## Strict view-only boundary

The beta does not treat view-only as a preference. The internal RFB server installs handlers that discard keyboard and pointer events, disables client clipboard input and disallows file transfer.

The public RA2r frontend never grants additional input capabilities.

## Adaptive framebuffer transport

### Initial state

The first real framebuffer after a virtual monitor starts is submitted as a full lossless update. This establishes a coherent client state without retaining the obsolete periodic-keyframe diagnostic policy.

### Latest-only source handling

PipeWire may run faster than the remote viewer. When downstream queues indicate pressure, the publisher stops consuming intermediate source states and resumes from the newest available state after low-water recovery.

The goal is latency, not preserving every captured frame.

### JPEG21 live updates

The old target viewer advertises RFB encoding 21. Active non-zero changes are sent as JPEG21 at quality 30.

Pending damage may coalesce while the server waits for the next `FramebufferUpdateRequest`. A custom JPEG response consumes exactly one outstanding request, preserving normal RFB demand-driven semantics.

### CopyRect acceleration

The target client also advertises standard RFB CopyRect encoding 1.

For a pending update the server can search the last framebuffer state **actually delivered to that client** for a translated copy of the current pixels. It does not try to classify an application as GUI/video and does not depend on Mutter window metadata.

Candidate translations are only accepted after byte-exact verification on 32x32 tiles. A successful update contains:

```text
CopyRect(known pixels)
+ zero to four JPEG21 residual rectangles
```

If no safe translation exists, the regular JPEG21 path is used unchanged.

This is particularly effective for window dragging and scrolling; full-screen video generally cannot satisfy exact translation checks and remains JPEG.

### Progressive exact repair

Every lossy JPEG region marks 32x32 tiles as inexact. Once source activity is quiet and transport is below low-water, the server schedules dispersed lossless ZRLE tiles at low priority until the client framebuffer is exact again.

Repair passes spread visual refinement across the region instead of scanning linearly from one corner.

A previously observed race in which an already-scheduled repair tile can become stale when motion resumes is still tracked as a beta limitation.

## Logging

Logging is a single hierarchy rather than independent diagnostic switches:

- `error`: failures;
- `info`: service/connection/monitor lifecycle;
- `debug`: protocol/capture/transport state and periodic statistics;
- `trace`: per-update frame/JPEG/CopyRect/repair/latency details.

Internal telemetry fields are derived from this level and are not public configuration knobs.

## Process/service architecture

### User daemon

`vnc-monitor.service` runs under the user systemd manager and shares the graphical session. It must be able to access:

- session D-Bus;
- Mutter RemoteDesktop/ScreenCast;
- PipeWire runtime sockets;
- the user's private RA2 identity and layout cache.

### PAM helper

The PAM helper is intentionally separate and privileged. A system socket/service pair exposes only the narrow authentication operation needed by the unprivileged daemon.

## Future protocol backends

The capture/virtual-monitor side is deliberately independent of the external protocol. A future legacy-RDP experiment can reuse the same conceptual source:

```text
Mutter -> PipeWire -> framebuffer -> protocol backend
```

RDP experimentation is not part of `0.1.0-beta.1` and should happen after this checkpoint, without destabilizing the VNC beta path.
