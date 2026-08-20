# Troubleshooting

Start with the normal service log:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
journalctl --user -u vnc-monitor.service -n 100 --no-pager
```

The service reads:

```text
~/.config/vnc-monitor/config.ini
```

Show exactly what the parser sees:

```bash
~/.local/bin/vnc-monitor \
  --config ~/.config/vnc-monitor/config.ini \
  --show-config
```

For protocol/capture details set `[logging] level=debug` in the config and restart the service. Use `trace` only for short frame-level diagnostics. CLI `--verbose` remains useful for foreground tests.

## Service fails immediately after editing config

Run `--show-config` as above. The beta.2 parser is intentionally strict: an unknown section/key, invalid boolean/range, bad `display.mode`, or malformed path is an error rather than a silently ignored setting.

The repository default is available at:

```text
config/vnc-monitor.conf
```

The installer never overwrites an existing installed config during an upgrade.

## Service is not listening on the configured public port

Check:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
ss -ltnp | grep vnc-monitor
```

Typical causes:

- the user service was not installed/enabled;
- another process already owns the configured `[network] port`;
- the binary under `~/.local/bin/vnc-monitor` is missing or stale;
- config validation failed;
- a hardening/path error prevents service startup.

Normal repair/upgrade path:

```bash
git pull --ff-only
./install.sh
```

## Why TCP/5903 is not listening while idle

This is intentional in beta.2. The internal LibVNCServer backend is session-scoped and is started only **after** RA2r/PAM authentication. It is removed again on disconnect.

The persistent idle listener is the public `[network] port` (5901 by default).

## A second viewer cannot connect

This is intentional. The beta has a **strong single-connect** policy: one accepted viewer owns the virtual-display session from RA2 negotiation through teardown.

The public listener remains responsive during that session only so additional connection attempts can be rejected immediately. The second socket is reset/closed and the active viewer is left untouched.

At `info` level the journal should contain:

```text
Rejected additional client: ADDRESS (strong single-connect policy)
```

A second client is also rejected while the first client is still authenticating; the slot is reserved at `accept()`, not only after PAM success.

## A client connected but never completed authentication

An unauthenticated connection must not own the only slot forever. The effective config contains:

```ini
[network]
client-handshake-timeout-ms=60000
```

This is one monotonic deadline for the bounded handshake `io_*` phase. It is not reset by trickling individual bytes. Once the deadline expires, blocked protocol/auth-helper I/O fails and the connection is torn down. After successful RA2/PAM authentication the deadline is cleared completely.

Check the effective value:

```bash
~/.local/bin/vnc-monitor --show-config | grep -i handshake
```

A timed-out or otherwise failed negotiation is logged as an RA2 handshake failure and the single-client slot is released.

## Wi-Fi disappeared but the virtual monitor is still present

The active external socket uses TCP dead-peer detection rather than an application-idle timer. Defaults are:

```ini
[network]
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
```

A healthy viewer showing a completely static desktop is allowed to remain connected indefinitely; it still answers TCP keepalive probes.

If the route/Wi-Fi disappears without a normal FIN/RST, the kernel needs time to establish that the peer is dead. Exact timing depends on TCP/network state, so do not treat the configured numbers as an exact wall-clock disconnect guarantee. With the defaults, the intent is to recover the abandoned slot in tens of seconds rather than indefinitely.

At `debug`, verify that the socket policy was applied:

```text
Client liveness: keepalive idle=15s interval=5s probes=3 user-timeout=20000ms handshake-deadline=60000ms
```

After the socket failure propagates through the RA2 relay, normal teardown should follow:

```text
Virtual monitor removed
Client disconnected: ADDRESS
```

A client process that is still alive enough for its operating system to ACK TCP packets is not a dead TCP peer. This policy deliberately avoids inventing an RFB activity timeout that would disconnect a legitimate static viewer.

## Service is running but the client cannot authenticate

Check the PAM socket:

```bash
make pam-service-status
ls -l /run/vnc-monitor-auth.sock
```

The socket is system-level even though the main daemon is a user service.

At `debug` level the RA2r negotiation should reach the authentication stage. A failure before that can instead indicate a changed RA2 identity, protocol mismatch, or the configured handshake deadline.

## Client reports a changed server identity

The beta identity path is:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

Older development versions used `./ra2-server-key.pem` in the checkout. Installation migrates that key only when the beta destination does not already exist.

Do not casually delete/regenerate this file: a viewer may pin or prompt for the server identity.

## Authentication succeeds but no virtual monitor appears

Use `debug` logging and inspect Mutter/session errors.

The daemon must run as the **logged-in GNOME user**, not as a root system service. It needs the user's session D-Bus and PipeWire environment.

Useful checks inside the same desktop account:

```bash
loginctl show-session "$XDG_SESSION_ID" -p Type -p Class -p Active
systemctl --user status pipewire.service --no-pager
```

The target session must be Wayland and active.

## Virtual monitor appears but capture fails

Native PipeWire is the production default. To distinguish a native-capture problem from the Mutter stream itself, run a foreground test with the fallback backend:

```bash
systemctl --user stop vnc-monitor.service
~/.local/bin/vnc-monitor \
  --config ~/.config/vnc-monitor/config.ini \
  --capture-backend gstreamer \
  --verbose debug
```

Restore the service afterwards:

```bash
systemctl --user start vnc-monitor.service
```

Do not run two instances simultaneously on the same public port.

## Client does not change the screen size in `display.mode=auto`

First confirm the effective setting:

```bash
~/.local/bin/vnc-monitor --show-config | grep -E 'screen mode|framebuffer'
```

`auto` is not display-size guessing. RFB has to provide a client request.

A viewer must advertise `ExtendedDesktopSize` (`-308`) and send `SetDesktopSize` (client message 251) to request dimensions. A client advertising only `NewFBSize`/`DesktopSize` (`-223`) can **receive** a server resize but cannot tell the server its preferred size.

Such clients stay at the configured `[display] width` / `height` fallback.

At `debug`, a capable client request should eventually produce either:

```text
Accepted client framebuffer resize: WIDTHxHEIGHT
```

or a clear rejection/failure message.

## Client resize is rejected

Check:

- `[display] mode=auto` rather than `fixed`;
- requested dimensions are within 64..16384;
- the request contains exactly one screen at `(0,0)` spanning the requested framebuffer;
- Mutter/PipeWire can actually create/capture the requested mode;
- memory allocation did not fail.

`fixed` mode deliberately returns RFB `ResizeProhibited`.

If a new Mutter/PipeWire size cannot be created, the server attempts to restore the previous monitor size before rejecting the request.

## Resize succeeds but GNOME placement changes

Layout caches are dimension-specific. A previously unseen framebuffer size can therefore initially use Mutter's default placement.

Arrange it once in GNOME; on disconnect the layout for those dimensions is saved. Later sessions at that same size can restore it when `[display] layout-remember=on`.

## Cursor is missing or does not update

Default cursor mode is:

```ini
[capture]
cursor=metadata
```

This uses PipeWire `SPA_META_Cursor` and is the tested production path. Embedded cursor mode is retained as a compatibility choice; it previously failed to generate frames for cursor-only motion on the target setup.

## GUI movement is jerky

At `trace`, look for `COPY` records. Normal window dragging should often move most pixels locally when the viewer advertises CopyRect.

If no CopyRect is selected:

- the change may not be a pure translation;
- the source may be outside the ±256 px search radius;
- exact 32x32 verification may reject the candidate;
- the viewer may not support CopyRect.

Fallback to JPEG21 is safe and expected.

After a framebuffer resize the CopyRect reference is intentionally invalidated and must be re-established from a delivered frame before CopyRect can be used again.

## Full-screen video is much slower than local video playback

This can be viewer-side rather than LAN/server saturation. The VNC path sends framebuffer updates; local video playback can use a dedicated video decoder pipeline and is not directly comparable.

Inspect `debug`/`trace` telemetry for:

- external output queue;
- backend input queue;
- TCP unacked packets;
- RTT;
- JPEG bytes and rectangle area;
- source frames coalesced per update.

## Image becomes sharp after motion stops

Expected behaviour. Active regions are intentionally lossy JPEG. During idle time the repair scheduler sends lossless 32x32 ZRLE tiles until the framebuffer is exact.

## Brief stale 32x32 tile during renewed motion

This remains an open beta issue. A repair tile may have been scheduled from an earlier source state just before new motion resumes.

Capture a short `trace` log around a reproducible event; this is the main repair-scheduler correctness item still under observation.

## Layout is not restored

The layout-cache namespace from development versions is retained:

```text
~/.config/vnc-monitor-server/
```

Check `[display] layout-remember=on`. To intentionally learn a replacement layout, temporarily set `layout-resave=on`, complete a session, then return it to `off`.

## Need maximum diagnostic detail

Stop the service and run:

```bash
systemctl --user stop vnc-monitor.service
~/.local/bin/vnc-monitor \
  --config ~/.config/vnc-monitor/config.ini \
  --verbose trace 2>&1 | tee vnc-monitor-trace.log
```

Trace output can be large. It is intentionally not the service default.
