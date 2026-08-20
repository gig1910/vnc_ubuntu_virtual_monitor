# VNC Monitor for GNOME Wayland

`vnc-monitor` turns an old VNC-capable tablet into a view-only extended monitor for the current GNOME Wayland desktop session.

## Beta status

Current beta: `0.1.0-beta.1`.

The production path uses Mutter RemoteDesktop/ScreenCast, native PipeWire capture, LibVNCServer, RA2r authentication with PAM, adaptive JPEG21 transport, CopyRect motion reuse and progressive lossless repair.

The virtual monitor exists only while a VNC client is connected and is removed after disconnect.

## Quick install

Run as the logged-in GNOME desktop user:

```bash
git switch 0.1.0-beta
git pull --ff-only
./install.sh
```

For the first build after switching from an older development branch:

```bash
./install.sh --clean
```

Before compilation the installer verifies the required build tools, all `pkg-config` modules, libjpeg and PAM development headers/libraries. The build then uses all available logical CPUs by default (`nproc`).

The installer builds and installs both privilege domains:

- `vnc-monitor.service` — systemd **user** service for the active GNOME Wayland session;
- `vnc-monitor-auth.socket` + `vnc-monitor-auth@.service` — system units for PAM authentication.

It must **not** be run with `sudo`; sudo is invoked internally only for the PAM/system-unit installation.

See [`docs/INSTALL.md`](docs/INSTALL.md) for the full installation and service model.

## Runtime behaviour

Idle:

```text
vnc-monitor.service
    -> listens on TCP/5901
    -> no virtual monitor
    -> no PipeWire capture
```

Connected:

```text
old VNC viewer
    -> RFB 3.8 / RA2r
    -> PAM authentication
    -> Mutter virtual monitor
    -> PipeWire BGRx + cursor metadata
    -> adaptive RFB transport
    -> encrypted RA2r relay
```

Disconnect:

```text
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

```text
--verbose error   only errors
--verbose info    lifecycle/connect/disconnect
--verbose debug   negotiation/capture/transport summaries
--verbose trace   per-update transport diagnostics
```

The systemd user service defaults to `info`.

Live service log:

```bash
journalctl --user -u vnc-monitor.service -f
```

## Documentation

- [`docs/INSTALL.md`](docs/INSTALL.md) — build, installation and systemd operation;
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

User identity/layout data are preserved unless `make purge-config` is explicitly requested.
