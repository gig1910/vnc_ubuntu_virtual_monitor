# Troubleshooting

Start with the normal service log:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
journalctl --user -u vnc-monitor.service -n 100 --no-pager
```

For protocol/capture details temporarily switch the service to `--verbose debug`; use `trace` only for short frame-level diagnostics.

## Service is not listening on TCP/5901

Check:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
ss -ltnp | grep ':5901'
```

Typical causes:

- the user service was not installed/enabled;
- another process already owns TCP/5901;
- the binary under `~/.local/bin/vnc-monitor` is missing or stale;
- a hardening/path error prevents service startup.

Reinstall after a successful build:

```bash
make install-service
```

## Service is running but the client cannot authenticate

Check the PAM socket:

```bash
make pam-service-status
ls -l /run/vnc-monitor-auth.sock
```

The socket is system-level even though the main daemon is a user service.

At `debug` level the RA2r negotiation should reach the authentication stage. A failure before that can instead indicate a changed RA2 identity or protocol mismatch.

## Client reports a changed server identity

The beta identity path is:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

Older development versions used `./ra2-server-key.pem` in the checkout. `make install` migrates that key only when the beta destination does not already exist.

Do not casually delete/regenerate this file: the old viewer may pin or prompt for the server identity.

## Authentication succeeds but no virtual monitor appears

Run with `--verbose debug` and inspect Mutter/session errors.

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
~/.local/bin/vnc-monitor --capture-backend gstreamer --verbose debug
```

Restore the service afterwards:

```bash
systemctl --user start vnc-monitor.service
```

Do not run two instances simultaneously on the same ports.

## Cursor is missing or does not update

Default cursor mode is:

```text
--mutter-cursor metadata
```

This uses PipeWire `SPA_META_Cursor` and is the tested production path. Embedded cursor mode is retained only as a compatibility choice; it previously failed to generate frames for cursor-only motion on the target setup.

## GUI movement is jerky

At `trace`, look for `COPY` records. The old iPad advertises CopyRect and normal window dragging should often move most pixels locally on the client.

If no CopyRect is selected:

- the change may not be a pure translation;
- the source may be outside the ±256 px search radius;
- the exact 32x32 verification may reject the candidate;
- the viewer may be in a negotiated state where CopyRect is unavailable.

Fallback to JPEG21 is safe and expected.

## Full-screen video is much slower than local video playback on the iPad

This is a known beta limitation, not evidence of network saturation by itself.

The current VNC path sends independent JPEG21 rectangles. The old viewer must decode/draw each framebuffer update and then continue its RFB request cycle. Local video playback can use a dedicated hardware H.264 pipeline and is therefore not comparable.

Before blaming bandwidth, inspect `debug`/`trace` telemetry for:

- external output queue;
- backend input queue;
- TCP unacked packets;
- RTT;
- JPEG bytes and rectangle area;
- source-frames coalesced per update.

The established target-iPad tests showed large-motion performance constrained primarily on the viewer side rather than by LAN bandwidth or server JPEG encoding.

## Image becomes sharp after motion stops

Expected behaviour. Active regions are intentionally lossy JPEG. During idle time the repair scheduler sends lossless 32x32 ZRLE tiles until the framebuffer is exact.

## Brief stale 32x32 tile during renewed motion

This remains an open beta issue. A repair tile may have been scheduled from an earlier source state just before new motion resumes.

Do not hide a reproducible case. Capture a short `--verbose trace` log around the event; this is the main repair-scheduler correctness item still under observation.

## Layout is not restored

The existing layout-cache namespace from the development versions is deliberately retained in beta so already learned layouts keep working.

Check that the service can write its allowed user config paths and that:

```text
--layout-remember on
```

is in effect.

To intentionally learn a replacement layout, use one foreground/service run with:

```text
--layout-resave on
```

then return it to `off`.

## Need maximum diagnostic detail

Use:

```bash
~/.local/bin/vnc-monitor --verbose trace 2>&1 | tee vnc-monitor-trace.log
```

Stop the systemd user service first so the foreground process can bind the public/backend ports.

Trace output can be large. It is intentionally not the service default.
