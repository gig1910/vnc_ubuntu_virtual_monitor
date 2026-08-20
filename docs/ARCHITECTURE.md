# Architecture

## Goal

Expose one real virtual monitor inside the **current GNOME Wayland session** and display it through a legacy-compatible, view-only VNC connection.

The monitor is connection-scoped: it does not exist while the daemon is idle.

## End-to-end data path

```text
VNC viewer
    |
    | RFB 3.8 / RA2r encrypted records
    v
public RA2r frontend :5901
    |
    | decrypted local RFB, active session only
    v
LibVNCServer 127.0.0.1:5903
    |
    | adaptive framebuffer updates
    v
JPEG21 / CopyRect / lossless repair
    ^
    | BGRx framebuffer
FrameBridge
    ^
    | copied frame + cursor composition
native PipeWire capture
    ^
    | RecordVirtual stream
Mutter ScreenCast + RemoteDesktop
    |
    v
one virtual monitor in the current GNOME Wayland session
```

## Idle vs active process model

Idle daemon:

```text
public listener :5901
no client worker
no LibVNCServer :5903
no PipeWire capture
no Mutter virtual monitor
```

Accepted session:

```text
single client worker
  -> RA2r/PAM
  -> Mutter virtual monitor
  -> PipeWire capture
  -> session-local LibVNCServer :5903
  -> encrypted RFB relay
```

Disconnect or detected failure tears the entire session down and returns to the idle listener.

## Strong single-connect ownership

The beta intentionally has **one display owner**. This is a server invariant, not a side effect of a blocking listener and not a configurable `max-clients` value.

The public listener stays in the main thread. The first accepted socket atomically reserves the single `ClientSlot`, and one detached worker owns the display-session lifecycle.

While that slot is reserved, any later connection is accepted only far enough to identify the conflict. The extra socket receives an immediate close/reset and never enters RA2 negotiation:

```text
client #1 -> ClientSlot -> worker -> display session
client #2 -> listener -> immediate reject/reset
client #3 -> listener -> immediate reject/reset
```

The slot is reserved **before authentication**, so two clients cannot race during RA2/PAM setup.

This does not introduce multi-client state: there is still one FrameBridge, one virtual monitor/capture pipeline and one adaptive RFB state for the current session.

## Connection-liveness layers

A single-slot server must recover both from a vanished established peer and from an unauthenticated client that connects but never completes negotiation.

### Established peer: kernel TCP liveness

Before the client worker starts, the external TCP socket requires:

- `SO_KEEPALIVE`;
- `TCP_KEEPIDLE`;
- `TCP_KEEPINTVL`;
- `TCP_KEEPCNT`;
- `TCP_USER_TIMEOUT` when provided by the target Linux headers/kernel.

Default policy:

```text
keepalive idle      15 s
keepalive interval   5 s
keepalive probes     3
TCP user timeout    20 s
```

These values detect transport failure. They are deliberately **not an application-idle timeout**: a healthy viewer displaying a static framebuffer may stay connected indefinitely.

When a half-open Wi-Fi/LAN connection is declared dead by TCP, the RA2 relay returns and normal session teardown releases the sole slot.

### Unauthenticated/stalled peer: monotonic I/O deadline

TCP keepalive cannot protect against a peer whose TCP stack is alive but whose protocol negotiation stalls. The bounded handshake therefore installs one thread-local monotonic `io_*` deadline, default 60000 ms.

`io_read_exact()` / `io_write_exact()` already wait through a central `poll()` wrapper. The wrapper now computes the earliest of:

```text
process shutdown
thread-local protocol deadline
socket SO_RCVTIMEO/SO_SNDTIMEO, when present
socket readiness/error
```

The handshake deadline is **not reset for each byte or record**, so a trickle-slow peer cannot extend the sole slot indefinitely.

The same worker performs the auth-helper `io_*` request/response, so those waits participate in the same deadline. After successful RA2/PAM authentication the deadline is cleared completely before the normal long-lived RFB session starts.

### Daemon shutdown

The accepted external client fd is registered with the existing shutdown supervisor. On SIGINT/SIGTERM the listener and client socket are shut down, and the main thread waits until the one client worker has finished before destroying shared FrameBridge/statistics objects.

## Virtual-monitor lifecycle

The confirmed Mutter sequence is:

1. `org.gnome.Mutter.RemoteDesktop.CreateSession`;
2. read the RemoteDesktop session `SessionId`;
3. `org.gnome.Mutter.ScreenCast.CreateSession` linked with `remote-desktop-session-id`;
4. `ScreenCast.Session.RecordVirtual` with cursor mode and `is-platform=true`;
5. subscribe for `PipeWireStreamAdded`;
6. start the **RemoteDesktop** session;
7. use the returned PipeWire node.

`ScreenCast.Session.Start` is intentionally not called for the linked session.

Stopping the RemoteDesktop session removes the virtual monitor.

## Capture ownership

A `struct pw_buffer *` is never retained after its PipeWire process callback.

Valid BGRx video is copied into private storage before the PipeWire buffer is recycled. Video chunk validity and cursor metadata validity are handled independently because Mutter can produce an empty/corrupted video chunk that still carries useful `SPA_META_Cursor` data.

## Cursor model

Production uses PipeWire cursor metadata.

The capture layer keeps:

- a cursor-free base framebuffer;
- cached cursor bitmap/position/hotspot;
- scratch framebuffer for composition.

On publish it composes the premultiplied RGBA cursor over the BGRx base. Cursor-only PipeWire buffers can therefore generate a new VNC frame without a new video payload.

## Persistent configuration

Runtime configuration is layered in this order:

```text
built-in defaults
    < /etc/vnc-monitor/config.ini
    < ~/.config/vnc-monitor/config.ini
    < CLI
```

The system file is optional for source installs and is installed as a conffile by the `.deb`. The user file is also optional; source install creates it once and preserves it on upgrades, while package install leaves user homes untouched.

`--config FILE` replaces the normal user override path but does **not** suppress the system baseline:

```text
built-ins < /etc/vnc-monitor/config.ini < --config FILE < CLI
```

Runtime strings are copied into owned `RuntimeConfig` storage; they do not point into parser-temporary data.

The INI parser is strict. Unknown keys/sections and invalid values prevent startup.

This split keeps machine-wide defaults under `/etc` while preserving per-user RA2 identity, layout and optional overrides under the user's home directory.

## Dynamic display sizing

### RFB semantics

Two resize capabilities must not be confused:

- `NewFBSize` / `DesktopSize` (`-223`) means the viewer can **receive** a server framebuffer-size change;
- `ExtendedDesktopSize` (`-308`) + client `SetDesktopSize` means the viewer can **request** dimensions/layout.

A client advertising only `-223` does not tell the server what size it wants.

### `display.mode=auto`

Configured `width` / `height` are initial/fallback dimensions.

For a valid single-screen `SetDesktopSize` request, the backend coordinates:

```text
preallocate new RFB framebuffer + diff storage
        |
        v
stop old capture / Mutter monitor
        |
        v
resize FrameBridge
        |
        v
start new Mutter/PipeWire monitor
        |
        v
resize JPEG/repair state
invalidate + resize CopyRect reference
        |
        v
rfbNewFramebuffer()
restore BGRx server pixel format
rebuild client translation tables
        |
        v
refresh dimension-specific layout cache
```

Only one screen at `(0,0)` spanning the framebuffer is accepted because one VNC session maps to one Mutter virtual monitor.

The resize is session-local. Persistent fallback dimensions are not modified, so the next connection starts from the config again.

A successful resize preserves the already-negotiated JPEG21 capability while clearing size-dependent pending/repair state and invalidating CopyRect reference pixels from the old dimensions.

If recreation fails, the code attempts to restore the previous monitor/FrameBridge size and returns an RFB resize error rather than committing a partial RFB framebuffer change.

### `display.mode=fixed`

`SetDesktopSize` is rejected with `ResizeProhibited`; configured dimensions are mandatory.

### Legacy viewer limitation

A viewer that only advertises `NewFBSize` cannot drive auto sizing. It receives the configured fallback size. This is a protocol capability limit, not something the server can infer reliably.

## RA2r frontend

The target legacy viewer uses RFB security type 13 (`RA2r`). The confirmed implementation path is:

- 2048-bit RSA key exchange;
- RSA-encrypted ServerRandom/ClientRandom;
- AES-128-EAX session keys;
- mutual SHA-1 public-key hash verification;
- authentication subtype 1;
- encrypted username/password;
- PAM verification through the privileged local helper socket.

After authentication the frontend is a codec-agnostic encrypted relay to the loopback LibVNCServer backend.

## Strict view-only boundary

View-only is a hard production invariant:

- keyboard events discarded;
- pointer/touch events discarded;
- clipboard input disabled;
- file transfer disabled.

No public runtime setting relaxes this boundary.

## Adaptive framebuffer transport

### Initial state

The first real framebuffer after monitor creation, and the first framebuffer after a resize, is sent losslessly to establish a coherent client state.

### Latest-only source handling

Under downstream pressure the publisher stops consuming intermediate source states. Once low-water is restored, it resumes from the newest FrameBridge state rather than replaying stale frames.

### JPEG21

A viewer advertising encoding 21 enables the custom JPEG21 path. Active changed regions are encoded at quality 30.

Pending damage can coalesce while waiting for the next `FramebufferUpdateRequest`; custom JPEG responses preserve RFB demand-driven semantics.

### CopyRect

When the viewer advertises standard CopyRect encoding 1, translated pixels may be reused from the last framebuffer state actually delivered to that client.

Candidates are accepted only after byte-exact 32x32 verification. A successful update is:

```text
CopyRect(exact known pixels)
+ zero to four JPEG21 residual rectangles
```

Otherwise normal JPEG21 is used.

### Progressive exact repair

Lossy JPEG regions mark 32x32 tiles as inexact. After source activity becomes quiet and transport is below low-water, dispersed lossless ZRLE tiles repair the client framebuffer until exactness is restored.

The previously observed stale-repair-tile race when motion resumes remains a tracked beta limitation.

## Logging

- `error`: failures;
- `info`: service/session lifecycle and rejected additional clients;
- `debug`: negotiation, capture/transport summaries, TCP liveness/deadline policy;
- `trace`: per-update JPEG/CopyRect/repair/latency detail.

## Service privilege model

### User daemon

`vnc-monitor.service` runs under the user systemd manager because it must share the logged-in GNOME Wayland session and access session D-Bus/PipeWire.

Source installs place the unit in `~/.config/systemd/user/` and run `~/.local/bin/vnc-monitor`. Binary packages place the unit in `/usr/lib/systemd/user/` and run `/usr/bin/vnc-monitor`.

### PAM helper

Authentication remains a narrow privileged system socket/service. The main VNC daemon itself is not root.

The package-installed socket is generic rather than bound to the username of the package builder. The helper validates the local peer with `SO_PEERCRED` and only permits PAM authentication for the same Unix username as the connecting process.

## Future protocol backends

The capture side remains conceptually independent of the external protocol:

```text
Mutter -> PipeWire -> framebuffer -> protocol backend
```

Legacy-RDP work should branch from the stabilized VNC beta rather than changing the confirmed VNC transport in place.
