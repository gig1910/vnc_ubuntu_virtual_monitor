# Troubleshooting

Start with the normal service logs:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
journalctl --user -u vnc-monitor.service -n 100 --no-pager
systemctl status vnc-monitor-auth.socket --no-pager -l
```

## Confirm which installation is active

A source install normally uses:

```text
~/.local/bin/vnc-monitor
~/.config/systemd/user/vnc-monitor.service
/etc/systemd/system/vnc-monitor-auth.socket
```

A binary package uses:

```text
/usr/bin/vnc-monitor
/usr/lib/systemd/user/vnc-monitor.service
/usr/lib/systemd/system/vnc-monitor-auth.socket
```

Higher-priority source-installed units can shadow package units. Check with:

```bash
systemctl --user cat vnc-monitor.service
systemctl cat vnc-monitor-auth.socket
command -v vnc-monitor
```

If migrating from source install to `.deb`, remove the old unit overrides first:

```bash
make uninstall-service
make uninstall-pam-service
```

These targets preserve user config, RA2 identity and layout state.

## Configuration layers

The daemon reads:

```text
built-in defaults
    < /etc/vnc-monitor/config.ini
    < ~/.config/vnc-monitor/config.ini
    < CLI
```

An explicit `--config FILE` replaces the normal user override but still loads `/etc/vnc-monitor/config.ini` first.

Show exactly what the parser sees:

```bash
vnc-monitor --show-config
```

The output shows both persistent config paths, whether each exists, and the final effective values.

For a source checkout without an installed binary:

```bash
./vnc-monitor --show-config
```

The parser is strict: an unknown section/key, malformed value, bad `display.mode`, or invalid path is an error rather than a silently ignored setting.

The repository template is:

```text
config/vnc-monitor.conf
```

The `.deb` installs it as `/etc/vnc-monitor/config.ini`; source install creates the user copy only when absent.

## Service is not listening on the public port

Check:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
ss -ltnp | grep -E ':(5901|5903)\b'
```

Typical causes:

- user service not enabled;
- another process already owns `[network] port`;
- a stale source unit is shadowing the package unit;
- config validation failed;
- daemon is running outside the active GNOME Wayland session.

For a package install, confirm the packaged service is enabled for the desktop user:

```bash
systemctl --user daemon-reload
systemctl --user enable --now vnc-monitor.service
```

## Why TCP/5903 is not listening while idle

This is intentional. The internal LibVNCServer backend is session-scoped and starts only **after** RA2r/PAM authentication. It is removed on disconnect.

The persistent idle listener is the public `[network] port` (5901 by default).

## A second viewer cannot connect

This is intentional. The beta has a **strong single-connect** policy: one accepted viewer owns the virtual-display session from RA2 negotiation through teardown.

The public listener remains responsive only so additional connection attempts can be rejected immediately. At `info` level:

```text
Rejected additional client: ADDRESS (strong single-connect policy)
```

The slot is reserved at `accept()`, so a second viewer is also rejected while the first is still authenticating.

## A client connected but never completed authentication

The effective config contains:

```ini
[network]
client-handshake-timeout-ms=60000
```

This is one monotonic deadline for the bounded handshake `io_*` phase. It is not reset by trickling individual bytes. Once it expires, the negotiation fails and the single-client slot is released.

Check the effective value:

```bash
vnc-monitor --show-config | grep -i handshake
```

## Wi-Fi disappeared but the virtual monitor is still present

The active external socket uses TCP dead-peer detection rather than an application-idle timer:

```ini
[network]
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
```

A healthy viewer showing a static desktop may remain connected indefinitely because its TCP stack still responds.

At `debug`, verify the applied policy:

```text
Client liveness: keepalive idle=15s interval=5s probes=3 user-timeout=20000ms handshake-deadline=60000ms
```

After TCP declares the peer dead, the RA2 relay exits and normal teardown removes the virtual monitor and releases the slot.

## Authentication fails

Check the PAM socket:

```bash
systemctl status vnc-monitor-auth.socket --no-pager -l
ls -l /run/vnc-monitor-auth.sock
```

For a package install, also confirm the helper path:

```bash
systemctl cat vnc-monitor-auth@.service
ls -l /usr/libexec/vnc-monitor-auth-helper
```

For a source install the helper is under `/usr/local/libexec/`.

The package socket is generic, but the root helper binds requests to the connecting local process with `SO_PEERCRED`: the requested username must match that process' Unix account before PAM authentication is attempted.

A failure before the PAM stage can indicate a changed RA2 identity, protocol mismatch, or handshake deadline.

## Client reports a changed server identity

The per-user identity is:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

Do not casually delete/regenerate this file; a viewer may pin or prompt for the server identity.

The `.deb` never installs a private key and does not create one from root maintainer scripts. The user daemon creates/loads it in the user's home.

## Authentication succeeds but no virtual monitor appears

The daemon must run as the **logged-in GNOME user**, not as a root system service. It needs the user's session D-Bus and PipeWire environment.

Useful checks:

```bash
loginctl show-session "$XDG_SESSION_ID" -p Type -p Class -p Active
systemctl --user status pipewire.service --no-pager
```

The target session must be Wayland and active.

## Virtual monitor appears but capture fails

Native PipeWire is the default. To compare against the GStreamer fallback:

```bash
systemctl --user stop vnc-monitor.service
vnc-monitor --capture-backend gstreamer --verbose debug
```

Restore afterwards:

```bash
systemctl --user start vnc-monitor.service
```

Do not run two instances on the same public port.

## Client does not change screen size in `display.mode=auto`

Confirm effective settings:

```bash
vnc-monitor --show-config | grep -E 'screen mode|framebuffer'
```

`auto` is not display-size guessing. The viewer must advertise `ExtendedDesktopSize` (`-308`) and send `SetDesktopSize` to request dimensions.

A viewer advertising only `NewFBSize`/`DesktopSize` (`-223`) can **receive** a server resize but cannot tell the server its preferred dimensions, so the configured fallback remains in use.

At `debug`, a capable request should produce either:

```text
Accepted client framebuffer resize: WIDTHxHEIGHT
```

or a clear rejection/failure message.

## Client resize is rejected

Check:

- `[display] mode=auto`, not `fixed`;
- requested dimensions are within 64..16384;
- the request contains one screen at `(0,0)` spanning the framebuffer;
- Mutter/PipeWire can create/capture the requested mode;
- memory allocation succeeded.

`fixed` mode deliberately returns `ResizeProhibited`.

## Resize succeeds but GNOME placement changes

Layout caches are dimension-specific under:

```text
~/.config/vnc-monitor-server/
```

A new framebuffer size can initially use Mutter's default placement. Arrange it once; the layout is saved on disconnect and can be restored later when `layout-remember=on`.

## Cursor is missing or does not update

Default cursor mode is:

```ini
[capture]
cursor=metadata
```

This uses PipeWire `SPA_META_Cursor` and is the tested production path.

## GUI movement is jerky

At `trace`, look for CopyRect records. Moving windows should often reuse known pixels when the viewer advertises CopyRect.

Fallback to JPEG21 is safe and expected when an exact translation cannot be verified.

After framebuffer resize, the CopyRect reference is intentionally invalidated and rebuilt from newly delivered pixels.

## Full-screen video is slower than local playback

This can be viewer-side rather than LAN/server saturation. Inspect `debug`/`trace` telemetry for output/input queues, TCP unacked packets/RTT, JPEG bytes, rectangle area, and source-frame coalescing.

## Image becomes sharp after motion stops

Expected. Active regions are lossy JPEG; idle repair sends lossless 32x32 ZRLE tiles until the client framebuffer is exact.

## Brief stale 32x32 tile during renewed motion

This remains an open beta issue. A repair tile can have been scheduled from an earlier source state immediately before new motion resumes.

## Inspect a built `.deb`

Before installation:

```bash
dpkg-deb --info dist/vnc-monitor_*.deb
dpkg-deb --contents dist/vnc-monitor_*.deb
```

The `Depends` field should contain runtime libraries plus PipeWire, not compiler/development packages such as `build-essential`, `pkg-config`, `dpkg-dev`, or `*-dev` packages.

## Need maximum diagnostic detail

Stop the user service and run:

```bash
systemctl --user stop vnc-monitor.service
vnc-monitor --verbose trace 2>&1 | tee vnc-monitor-trace.log
```

Trace output can be large. It is intentionally not the service default.
