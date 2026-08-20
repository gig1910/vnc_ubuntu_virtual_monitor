# Architecture

## Goal

Present a real virtual monitor to the **current GNOME Wayland session** and display it on a view-only VNC client. The monitor exists only for the lifetime of an authenticated viewer connection.

## End-to-end data path

```text
VNC viewer
    |
    | RFB 3.8, RA2r, encrypted records
    v
RA2r frontend :5901
    |
    | decrypted local RFB, active session only
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

## Service and session lifecycle

The persistent user daemon keeps only the public RA2r listener while idle. It does **not** keep a Mutter monitor, PipeWire stream, or internal LibVNCServer listener alive.

On a connection:

1. RA2r/PAM authentication completes;
2. a session-local `RuntimeConfig` is copied from the parsed persistent config;
3. the FrameBridge is prepared at the configured initial/fallback size;
4. the Mutter virtual monitor and PipeWire capture start;
5. the per-session LibVNCServer backend starts on loopback;
6. decrypted RFB is relayed between the authenticated viewer and that backend.

On disconnect the internal RFB backend is stopped first, then capture and the virtual monitor are removed. The next viewer starts from the persistent config again; a resize requested by one client does not alter the configured fallback for later clients.

## Strong single-connect ownership

The server deliberately has exactly one active display owner. This is a production invariant in the beta, not an accidental consequence of a blocking `accept()` loop.

The public listener remains responsive while the active session runs. The active session is executed by one detached worker that exclusively owns the shared FrameBridge/capture/backend lifecycle. If another TCP connection reaches the public listener while that worker is active, the new socket is immediately reset and closed; it never reaches RA2 negotiation and cannot disturb the active display owner.

This arrangement is intentionally **not** multi-client support. There is still only one capture pipeline, one virtual monitor and one per-session adaptive RFB state.

### Dead-peer release

The external client socket enables Linux TCP liveness controls before RA2 negotiation:

- `SO_KEEPALIVE`;
- `TCP_KEEPIDLE`;
- `TCP_KEEPINTVL`;
- `TCP_KEEPCNT`;
- `TCP_USER_TIMEOUT` when available.

The defaults are 15 seconds idle before keepalive probing, 5 seconds between probes, 3 failed probes and a 20-second user timeout for unacknowledged data.

These are transport-liveness checks, **not application-idle checks**. A healthy client may display a completely static framebuffer indefinitely. If Wi-Fi/LAN connectivity disappears without a clean TCP close, the kernel eventually fails the socket; the RA2 relay exits, the worker tears down LibVNCServer/PipeWire/Mutter and the single-client slot becomes available again.

During daemon shutdown the main thread shuts down the active client socket and waits for the only client worker to finish before destroying shared FrameBridge/statistics state.

## Virtual-monitor lifecycle

The Mutter sequence is:

1. `org.gnome.Mutter.RemoteDesktop.CreateSession`;
2. read the RemoteDesktop session `SessionId`;
3. `org.gnome.Mutter.ScreenCast.CreateSession` linked with `remote-desktop-session-id`;
4. `ScreenCast.Session.RecordVirtual` with the requested cursor mode and `is-platform=true`;
5. subscribe for `PipeWireStreamAdded`;
6. start the **RemoteDesktop** session;
7. capture the returned PipeWire node.

`ScreenCast.Session.Start` is intentionally not called for the linked RemoteDesktop session.

Stopping the RemoteDesktop session removes the virtual monitor.

## Persistent configuration

The systemd user service reads `~/.config/vnc-monitor/config.ini`. Values are owned by `RuntimeConfig`; no runtime string points into temporary parser storage.

Precedence is:

```text
built-in defaults < config.ini < CLI
```

The INI parser is strict. Unknown keys/sections and invalid values prevent startup instead of being silently ignored.

The installer creates the file once from `config/vnc-monitor.conf` and never overwrites an existing local config during upgrades.

## Dynamic display sizing

### RFB semantics

There are two different RFB resize capabilities:

- `NewFBSize` / `DesktopSize` (`-223`) lets a client **receive** a server framebuffer-size change;
- `ExtendedDesktopSize` (`-308`) plus client message `SetDesktopSize` lets a client **request** a framebuffer size/layout.

The first capability does not communicate the viewer's preferred dimensions to the server.

### `display.mode=auto`

`display.width` and `display.height` are the initial/fallback dimensions.

When a viewer that supports `ExtendedDesktopSize` sends a valid one-screen `SetDesktopSize` request, the LibVNCServer hook performs a coordinated resize in the backend thread. Only a single screen at `(0,0)` spanning the requested framebuffer is accepted because one VNC session maps to one Mutter virtual monitor.

The transaction is:

```text
preallocate new RFB framebuffer + diff storage
        |
        v
stop old PipeWire/Mutter monitor
        |
        v
resize FrameBridge
        |
        v
start new Mutter/PipeWire monitor at requested dimensions
        |
        v
resize adaptive JPEG/repair state
invalidate + resize CopyRect reference
        |
        v
rfbNewFramebuffer()
        |
        v
refresh dimension-specific GNOME layout cache
```

Only after the new monitor/capture and transport state are available is the LibVNCServer framebuffer committed. If monitor creation fails, the previous monitor/FrameBridge size is restored. If adaptive storage allocation fails after monitor recreation, the server also attempts to roll the monitor back and returns `OutOfResources` to the resize request.

`rfbNewFramebuffer()` is the LibVNCServer-supported path for replacing a framebuffer and updating connected clients. After replacement, the backend explicitly restores the BGRx server pixel format and recalculates client translation tables so resize cannot silently revert LibVNCServer to its host-endian default channel layout.

The normal LibVNCServer `SetDesktopSize` machinery sends the mandatory `ExtendedDesktopSize` result to the requester.

A successful resize preserves the already-negotiated JPEG21 client capability. Size-dependent pending JPEG/repair state is cleared, and the CopyRect reference is invalidated so motion reuse is not attempted against pixels from the old dimensions.

### `display.mode=fixed`

Client `SetDesktopSize` requests are rejected with `ResizeProhibited`; the configured dimensions are always used.

### Older viewers

A viewer that only advertises `NewFBSize` cannot tell the server its preferred dimensions. In `auto` mode it therefore receives the configured initial/fallback dimensions. This is an RFB protocol capability limit rather than server-side guessing.

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

The first real framebuffer after a virtual monitor starts (and the first framebuffer after a resize) is submitted losslessly. This establishes a coherent client state without periodic diagnostic keyframes.

### Latest-only source handling

PipeWire may run faster than the remote viewer. When downstream queues indicate pressure, the publisher stops consuming intermediate source states and resumes from the newest available state after low-water recovery.

The goal is latency, not preserving every captured frame.

### JPEG21 live updates

A viewer advertising RFB encoding 21 enables the custom JPEG21 path. Active non-zero changes are sent at quality 30.

Pending damage may coalesce while the server waits for the next `FramebufferUpdateRequest`. A custom JPEG response consumes exactly one outstanding request, preserving normal RFB demand-driven semantics.

### CopyRect acceleration

A client advertising standard CopyRect encoding 1 can reuse pixels already known to it.

For a pending update the server searches the last framebuffer state **actually delivered to that client** for a translated copy of current pixels. Candidate translations are only accepted after byte-exact verification on 32x32 tiles.

A successful update contains:

```text
CopyRect(known pixels)
+ zero to four JPEG21 residual rectangles
```

If no safe translation exists, the regular JPEG21 path is unchanged.

### Progressive exact repair

Every lossy JPEG region marks 32x32 tiles as inexact. Once source activity is quiet and transport is below low-water, the server schedules dispersed lossless ZRLE tiles until the client framebuffer is exact again.

A previously observed race in which an already-scheduled repair tile can become stale when motion resumes is still tracked as a beta limitation.

## Logging

Logging is a single hierarchy:

- `error`: failures;
- `info`: service/connection/monitor lifecycle and rejected extra clients;
- `debug`: protocol/capture/transport state, periodic statistics and applied TCP liveness policy;
- `trace`: per-update frame/JPEG/CopyRect/repair/latency details.

The persistent `[logging] level=` setting controls the service; `--verbose` is a CLI override for foreground diagnostics.

## Process/service architecture

### User daemon

`vnc-monitor.service` runs under the user systemd manager and shares the graphical session. It must be able to access session D-Bus, Mutter RemoteDesktop/ScreenCast, PipeWire, and private user config/identity/layout files.

### PAM helper

The PAM helper is intentionally separate and privileged. A system socket/service pair exposes only the narrow authentication operation needed by the unprivileged daemon.

## Future protocol backends

The capture/virtual-monitor side remains conceptually independent of the external protocol:

```text
Mutter -> PipeWire -> framebuffer -> protocol backend
```

The later legacy-RDP experiment should branch from this stabilized VNC beta rather than altering the confirmed VNC transport in place.
