# VNC Monitor for GNOME Wayland

**Version: 0.1.0-beta.1**

VNC Monitor turns an old VNC viewer into a display-only virtual monitor for the **current GNOME Wayland session**. The virtual display is created only while a VNC client is connected and is removed when the client disconnects.

The project was built around an old iPad client that requires RFB 3.8 + RA2r authentication and benefits heavily from CopyRect acceleration.

## What works in this beta

- real Mutter virtual monitor in the current GNOME Wayland session;
- 1024x768 virtual display by default, configurable from the command line;
- native PipeWire capture (GStreamer remains available as a fallback backend);
- RFB 3.8 frontend with RA2r (security type 13);
- 2048-bit persistent RSA server identity;
- AES-128-EAX encrypted RA2r channel;
- username/password authentication through PAM;
- strict view-only operation;
- cursor rendering from PipeWire `SPA_META_Cursor`;
- adaptive JPEG21 live transport, Q30;
- CopyRect motion acceleration when the client advertises encoding 1;
- progressive lossless ZRLE repair after lossy activity becomes idle;
- latest-only/backpressure governor to avoid building stale update queues;
- systemd user service for unattended startup inside the GNOME session.

## Security invariants

The beta intentionally does **not** expose switches for remote input:

- keyboard: ignored;
- pointer/touch: ignored;
- clipboard input: disabled;
- file transfer: disabled.

Authentication is handled by a small privileged PAM helper reachable through `/run/vnc-monitor-auth.sock`; the main VNC daemon remains an ordinary user process.

## Build

On the target Ubuntu system:

```bash
make clean
make -j20
```

The Makefile now tracks header dependencies with `-MMD -MP`, so normal subsequent builds only require:

```bash
make -j20
```

The resulting binary is:

```text
./vnc-monitor
```

## Install as a service

Run from the logged-in GNOME user account (do **not** prefix the make command itself with `sudo`):

```bash
make install-service
```

This performs two deliberately separate installations:

1. installs the privileged PAM helper/socket as system units;
2. installs `vnc-monitor` as a **systemd user service** for the active GNOME session.

A root system service is intentionally not used: the monitor process must share the user's session D-Bus, Mutter and PipeWire environment.

Useful commands:

```bash
make status-service
make logs-service
make restart-service
make stop-service
```

Or directly:

```bash
systemctl --user status vnc-monitor.service
journalctl --user -u vnc-monitor.service -f
```

The service starts at `graphical-session.target` and waits quietly on TCP/5901. The Mutter virtual monitor is not created until a client has authenticated and connected.

## Client connection

Default endpoint:

```text
TCP/5901
RFB 3.8
RA2r security type 13
RA2 auth subtype 1 (username + password)
```

The PAM username/password are the credentials accepted by `/etc/pam.d/vnc-monitor`.

The internal LibVNCServer backend binds only to:

```text
127.0.0.1:5903
```

and uses security type None because it is reachable only through the local encrypted/authenticated frontend.

## Logging

`--verbose` replaces the old collection of diagnostic switches:

| Level | Name | Output |
|---:|---|---|
| 0 | `error` | errors only |
| 1 | `info` | service lifecycle, connections, virtual-monitor lifecycle |
| 2 | `debug` | negotiation, transport state, periodic pipeline statistics |
| 3 | `trace` | per-update JPEG/CopyRect/repair/frame/latency diagnostics |

Examples:

```bash
./vnc-monitor --verbose info
./vnc-monitor --verbose debug
./vnc-monitor --verbose trace
```

For a persistent service override:

```bash
systemctl --user edit vnc-monitor.service
```

For example:

```ini
[Service]
ExecStart=
ExecStart=%h/.local/bin/vnc-monitor --verbose debug
```

Then:

```bash
systemctl --user daemon-reload
systemctl --user restart vnc-monitor.service
```

## Important runtime options

```text
--port N
--width N
--height N
--fps N
--capture-backend pipewire|gstreamer
--mutter-cursor hidden|embedded|metadata
--vnc-fps source|N
--latest-only on|off
--diff-detect on|off
--diff-tile-size N
--layout-remember on|off
--layout-resave on|off
--ra2-key FILE
--auth-socket PATH
--verbose LEVEL
```

See the complete current list with:

```bash
./vnc-monitor --help
```

## Transport model

For the first real framebuffer the server sends a lossless update. During active changes, JPEG21 is used to minimize traffic. If the change can be proven to be a translation of pixels already present on the client, the server sends `CopyRect` plus only the residual JPEG rectangles. When activity becomes idle, 32x32 lossless repair tiles progressively restore exact pixels.

CopyRect detection never trusts an application/window identity. It compares the current framebuffer with the last state actually delivered to the client and requires byte-exact 32x32 matches before scheduling a copy.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for details.

## Known beta limitations

- One external viewer is served at a time.
- Full-screen high-motion content still uses JPEG21. On the old target iPad this is substantially slower than local hardware-decoded video; CopyRect mainly improves GUI/window motion and scrolling.
- Progressive repair is intentionally low-priority. The previously observed stale-repair-tile race remains a beta item to keep under observation; it is not considered closed solely because recent tests did not reproduce it.
- RDP is not part of this beta. It will be investigated separately after this checkpoint.

## Uninstall

Remove the user daemon/service while preserving the RA2 identity:

```bash
make uninstall-service
```

Remove PAM support too:

```bash
make uninstall-pam-service
```

Remove old redraw/HW-cursor diagnostic remnants from earlier development versions:

```bash
make cleanup-obsolete-support
```

To explicitly delete the user configuration, cached layout and RA2 identity:

```bash
make purge-config
```

## Security note before a public release

An old RA2 private key existed in earlier Git history during development. The current working tree does not require that key, but **Git history must be purged and the RA2 identity rotated before making the repository/public release trustworthy**. See [SECURITY.md](SECURITY.md).

## Documentation

- [Installation and service operation](docs/INSTALL.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Security](SECURITY.md)
- [Changelog](CHANGELOG.md)
