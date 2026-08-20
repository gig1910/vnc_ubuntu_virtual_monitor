# VNC Monitor for GNOME Wayland

`vnc-monitor` turns a VNC-capable tablet into a view-only extended monitor for the current GNOME Wayland desktop session.

## Beta status

Current beta: `0.1.0-beta.2`.

The production path uses Mutter RemoteDesktop/ScreenCast, native PipeWire capture, LibVNCServer, RA2r authentication with PAM, adaptive JPEG21 transport, CopyRect motion reuse and progressive lossless repair.

The virtual monitor exists only while a VNC client is connected and is removed after disconnect.

## Quick install / upgrade

Run as the logged-in GNOME desktop user:

```bash
git switch 0.1.0-beta
git pull --ff-only
./install.sh
```

For a deliberately clean rebuild:

```bash
./install.sh --clean
```

Before compilation the installer verifies the required build tools, all `pkg-config` modules, libjpeg and PAM development headers/libraries. The build uses all available logical CPUs by default (`nproc`).

The installer builds and installs both privilege domains:

- `vnc-monitor.service` — systemd **user** service for the active GNOME Wayland session;
- `vnc-monitor-auth.socket` + `vnc-monitor-auth@.service` — system units for PAM authentication.

It must **not** be run with `sudo`; sudo is invoked internally only for the PAM/system-unit installation.

Existing services are upgraded in place and restarted only after a successful build/install.

## Persistent configuration

The service reads:

```text
~/.config/vnc-monitor/config.ini
```

On first install it is created from `config/vnc-monitor.conf` with mode `0600`. Subsequent upgrades preserve the local file unchanged.

Configuration precedence is:

```text
built-in defaults < config.ini < command-line options
```

Show the effective configuration without starting the server:

```bash
vnc-monitor --show-config
```

or explicitly:

```bash
~/.local/bin/vnc-monitor \
  --config ~/.config/vnc-monitor/config.ini \
  --show-config
```

After editing the installed config:

```bash
systemctl --user restart vnc-monitor.service
```

### Display sizing

The default is:

```ini
[display]
mode=auto
width=1024
height=768
```

`mode=auto` means `width`/`height` are the **initial/fallback** framebuffer size. A viewer that advertises RFB `ExtendedDesktopSize` and sends `SetDesktopSize` may request a different size during the session; Mutter/PipeWire and the RFB framebuffer are resized together.

Older viewers that only support `NewFBSize` can accept a server-side size change but cannot tell the server what size they want. They therefore use the configured fallback.

To force one exact size and reject client resize requests:

```ini
[display]
mode=fixed
width=1024
height=768
```

## Runtime behaviour

Idle:

```text
vnc-monitor.service
    -> listens on public TCP/5901
    -> no internal LibVNCServer listener
    -> no virtual monitor
    -> no PipeWire capture
```

Connected:

```text
VNC viewer
    -> RFB 3.8 / RA2r
    -> PAM authentication
    -> Mutter virtual monitor
    -> PipeWire BGRx + cursor metadata
    -> per-session LibVNCServer backend
    -> adaptive RFB transport
    -> encrypted RA2r relay
```

Disconnect:

```text
internal RFB backend stops
capture stops
virtual monitor is removed
layout is remembered
service returns to idle listener state
```

## Security properties

The beta is intentionally display-only:

- keyboard input ignored;
- pointer/touch input ignored;
- clipboard input disabled;
- file transfer disabled;
- persistent RA2 server identity stored mode `0600`;
- authentication delegated to the PAM helper over a local Unix socket.

The historical RA2 private key must be purged from Git history and the identity rotated before making the repository public. See [`SECURITY.md`](SECURITY.md).

## Logging

The persistent config uses:

```ini
[logging]
level=info
```

Available levels:

```text
error   only errors
info    lifecycle/connect/disconnect
DEBUG   negotiation/capture/transport summaries
trace   per-update transport diagnostics
```

CLI override example:

```bash
./vnc-monitor --verbose debug
```

Live service log:

```bash
journalctl --user -u vnc-monitor.service -f
```

## Documentation

- [`docs/INSTALL.md`](docs/INSTALL.md) — build, config, installation and systemd operation;
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — Mutter/PipeWire/RA2r/RFB transport architecture;
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) — diagnostics and recovery;
- [`SECURITY.md`](SECURITY.md) — security invariants and release cleanup;
- [`CHANGELOG.md`](CHANGELOG.md) — beta changes and historical milestones.

## Build manually

The unified installer is preferred for normal deployment. For development:

```bash
make -j"$(nproc)"
./vnc-monitor --verbose debug
```

The Makefile uses `-MMD -MP`, so included headers participate in dependency tracking.

## Service status

```bash
systemctl --user status vnc-monitor.service
systemctl status vnc-monitor-auth.socket
```

or:

```bash
make status-support
```

## Uninstall

```bash
make uninstall-service
make uninstall-pam-service
```

User config/identity/layout data are preserved unless `make purge-config` is explicitly requested.
