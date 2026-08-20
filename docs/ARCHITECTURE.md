# Architecture

## Goal

Expose one real virtual monitor inside the **currently active local GNOME Wayland login session** while preventing VNC from following or selecting an inactive user's desktop.

The monitor is connection-scoped: it does not exist while no authenticated client is attached.

## Beta.3 privilege split

```text
                         machine scope
VNC viewer
    |
    | TCP :5901
    v
vnc-monitor-broker                     root system service
    |
    | logind seat0.ActiveSession policy
    | SCM_RIGHTS (same accepted TCP fd)
    v
                         user/session scope
vnc-monitor --agent                    unprivileged systemd --user service
    |
    | RA2r + PAM
    v
LibVNCServer 127.0.0.1:5903            authenticated session only
    |
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
one virtual monitor in that login session
```

The broker does **not** proxy framebuffer traffic and does not handle RA2 passwords. It only owns the public listener, determines the eligible active login session and passes the accepted socket descriptor to that user's agent.

## Active-session gate

The broker queries `org.freedesktop.login1` for `seat0.ActiveSession`, then verifies the selected Session properties:

```text
Active == true
Remote == false
Class  == user
Type   == wayland
```

GDM/greeter is therefore not an attachable target.

A handoff records:

```text
logind Session ID
Unix UID
remote peer address
```

and the connection remains bound to that exact Session ID + UID for its lifetime.

### Switch-user / logoff invariant

The broker subscribes to logind seat/session changes and also performs a low-cost periodic fail-safe check.

If `seat0.ActiveSession` is no longer exactly the Session ID to which the VNC connection was bound, the broker calls `shutdown()` on its duplicate of the client TCP socket.

```text
session 3 / uid 1000 active
        |
VNC -> agent(uid 1000)
        |
Switch User
        |
seat0.ActiveSession != session 3
        |
shutdown(client TCP)
        |
RFB/RA2 exits
        |
Mutter virtual monitor removed
```

The connection is not redirected to GDM, Bob, or a later reactivated `gig` session. Reconnection and new RA2/PAM authentication are mandatory.

This prevents a surviving inactive graphical session from being used as a VNC target after Fast User Switching.

## Broker-agent IPC

The agent listens on:

```text
$XDG_RUNTIME_DIR/vnc-monitor/agent.sock
```

using Unix `SOCK_SEQPACKET`, mode `0600`, inside a systemd `RuntimeDirectory=vnc-monitor`.

The broker connects to the active user's socket and verifies:

```text
SO_PEERCRED.uid == active logind session UID
```

The agent independently verifies the connecting control peer is root.

The accepted external TCP socket is passed with `SCM_RIGHTS`. After a successful handoff both processes hold descriptors referring to the same TCP socket:

- agent descriptor: normal RA2/RFB I/O;
- broker descriptor: policy revocation only.

`shutdown()` affects the underlying socket rather than one descriptor, so broker revocation immediately propagates into the agent transport.

A separate control channel stays open for the complete session. The agent sends `DONE`, `BUSY`, or `REJECT` status. The agent also keeps a control-guard thread: if the broker disappears unexpectedly, EOF on the control channel shuts down the VNC TCP socket. A session is therefore not allowed to outlive its policy controller.

## Strong single-connect

The machine has exactly one external viewer slot.

The broker owns the only public listener. While one session is active, later connections are accepted only far enough to be immediately reset. This keeps client behaviour deterministic without introducing multi-viewer state.

The agent itself also retains its local single-client guard as a second invariant.

## Authentication boundary

RA2/PAM remains in the unprivileged session agent.

The root PAM helper socket is reachable by local user agents, but the helper obtains `SO_PEERCRED` from the caller and requires:

```text
RA2 username == Unix username owning the calling agent process
```

Thus broker routing and PAM authentication form independent checks:

```text
currently active logind UID
        ==
selected agent UID
        ==
RA2/PAM Unix username
```

A connection routed to Bob's active agent cannot authenticate as `gig`.

## Configuration ownership

Agent configuration remains layered:

```text
built-ins
  < /etc/vnc-monitor/config.ini
  < ~/.config/vnc-monitor/config.ini or --config
  < CLI
```

The machine-wide public `[network] port` is different: the system broker reads it only from:

```text
/etc/vnc-monitor/config.ini
```

Per-user config cannot redefine the machine listener.

RA2 identity and monitor-layout state remain per user.

## Virtual-monitor lifecycle

The confirmed Mutter sequence remains unchanged:

1. `org.gnome.Mutter.RemoteDesktop.CreateSession`;
2. read the RemoteDesktop `SessionId`;
3. `ScreenCast.CreateSession` linked with `remote-desktop-session-id`;
4. `RecordVirtual` with cursor mode and `is-platform=true`;
5. subscribe for `PipeWireStreamAdded`;
6. start the **RemoteDesktop** session;
7. consume the returned PipeWire node.

`ScreenCast.Session.Start` is intentionally not called for the linked session.

Stopping RemoteDesktop removes the virtual monitor.

## Capture ownership

A `struct pw_buffer *` is never retained after its PipeWire process callback. Valid BGRx pixels are copied into private storage before the PipeWire buffer is recycled.

Video payload validity and `SPA_META_Cursor` validity remain independent, so cursor-only metadata can generate a new VNC frame even when the video chunk is empty.

## Dynamic display sizing

`display.mode=auto` keeps configured width/height as initial/fallback dimensions. Viewers advertising RFB `ExtendedDesktopSize` may send `SetDesktopSize`; the backend coordinates Mutter/PipeWire, FrameBridge, adaptive transport state and LibVNCServer framebuffer resizing transactionally.

Older clients supporting only `NewFBSize` cannot communicate a preferred size and use the configured fallback.

`display.mode=fixed` rejects client resize with `ResizeProhibited`.

## Adaptive transport

Beta.3 does not redesign the proven beta.2 transport:

- initial real framebuffer is lossless;
- latest-only source handling under downstream pressure;
- JPEG21 Q30 for active changed regions;
- standard CopyRect for byte-exact translations;
- zero to four JPEG residual rectangles after CopyRect;
- progressive 32x32 lossless ZRLE repair while quiet;
- CopyRect/reference state reset on framebuffer resize.

The earlier stale progressive-repair tile race remains a tracked beta limitation.

## Strict view-only boundary

Production remains display-only:

- keyboard ignored;
- pointer/touch ignored;
- clipboard input disabled;
- file transfer disabled.

Standalone direct-listener mode remains available for development diagnostics, but packaged/source systemd production uses the broker + `--agent` architecture.
