# VNC Monitor for GNOME Wayland

`vnc-monitor` turns a VNC-capable tablet into a view-only extended monitor for the current GNOME Wayland desktop session.

## Beta status

Current beta: `0.1.0-beta.2`.

The production path uses Mutter RemoteDesktop/ScreenCast, native PipeWire capture, LibVNCServer, RA2r authentication with PAM, adaptive JPEG21 transport, CopyRect motion reuse and progressive lossless repair.

The virtual monitor exists only while a VNC client is connected and is removed after disconnect.

## Source install / upgrade

Run as the logged-in GNOME desktop user:

```bash
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

## Build a binary `.deb`

A compiled Debian/Ubuntu package can be built with:

```bash
./build-deb.sh
```

The builder:

- performs the same build-library preflight before compilation;
- builds with `make -j"$(nproc)"` by default;
- derives the package version from `VNC_MONITOR_VERSION` (`0.1.0-beta.2` becomes Debian version `0.1.0~beta.2-1`);
- stages the compiled daemon and PAM helper under `/usr`;
- installs the system baseline as `/etc/vnc-monitor/config.ini`;
- installs package-managed systemd units under `/usr/lib/systemd/system` and `/usr/lib/systemd/user`;
- uses `dpkg-shlibdeps` to calculate **runtime shared-library dependencies only**;
- creates the final package in `./dist/`.

Build/development packages such as `build-essential`, `pkg-config`, `dpkg-dev` and `*-dev` libraries are requirements of the **build machine only**. They are not copied into the binary package `Depends` field.

Optional clean build:

```bash
./build-deb.sh --clean
```

The resulting filename is similar to:

```text
dist/vnc-monitor_0.1.0~beta.2-1_amd64.deb
```

Install it with apt:

```bash
sudo apt install ./dist/vnc-monitor_0.1.0~beta.2-1_amd64.deb
```

Then enable the package-installed user service once, as the GNOME desktop user:

```bash
systemctl --user daemon-reload
systemctl --user enable --now vnc-monitor.service
```

The PAM socket is a system service and is enabled/restarted by the package. The package socket is generic rather than tied to the username of the build machine; the privileged helper validates the connecting Unix account using `SO_PEERCRED` and only authenticates that same username.

### Migrating from `./install.sh` to `.deb`

A source installation places higher-priority units in `~/.config/systemd/user/` and `/etc/systemd/system/`. Remove those overrides **before** installing the `.deb`:

```bash
make uninstall-service
make uninstall-pam-service
sudo apt install ./dist/vnc-monitor_0.1.0~beta.2-1_amd64.deb
systemctl --user daemon-reload
systemctl --user enable --now vnc-monitor.service
```

The source uninstall targets preserve the per-user config, RA2 identity and layout cache.

## Persistent configuration

Configuration is layered:

```text
built-in defaults
    < /etc/vnc-monitor/config.ini
    < ~/.config/vnc-monitor/config.ini
    < command-line options
```

`/etc/vnc-monitor/config.ini` is an optional system baseline. The `.deb` installs it as a package conffile.

`~/.config/vnc-monitor/config.ini` is the optional per-user override. Source installation creates it on first install and preserves it on upgrades. Package installation does not create or overwrite files in a user's home directory.

An explicit:

```bash
vnc-monitor --config /path/to/file.ini
```

replaces the normal per-user override path but still loads `/etc/vnc-monitor/config.ini` first. Effective precedence becomes:

```text
built-ins < /etc/vnc-monitor/config.ini < --config FILE < CLI
```

Show the effective configuration without starting the server:

```bash
vnc-monitor --show-config
```

After editing either persistent config layer:

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

### Strong single-connect policy

The beta intentionally serves **exactly one viewer at a time**. This is a fixed server policy, not a configurable client-count limit.

The public listener stays active while the first session is running so a second TCP connection can be detected and rejected immediately instead of sitting in the listen backlog. The second socket is closed with an immediate reset and the active session is left untouched. The slot is reserved from `accept()`, so this also applies while the first viewer is still authenticating.

Connection protection is configured under `[network]`:

```ini
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
client-handshake-timeout-ms=60000
```

The keepalive/user-timeout settings detect a vanished Wi-Fi/LAN peer and automatically tear down the abandoned session, releasing the single-client slot and removing its virtual monitor. They are **not** application-idle timers: a healthy viewer showing a completely static desktop may remain connected indefinitely.

`client-handshake-timeout-ms` is a single monotonic deadline for the bounded unauthenticated RA2/auth-helper `io_*` phase. It is not reset by each received byte, so a silent or trickle-slow client cannot monopolize the only slot forever. After successful authentication the deadline is cleared completely; the long-lived VNC session is governed only by TCP liveness.

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

While connected:

```text
second viewer -> immediate reject/reset
lost active peer -> TCP keepalive/user-timeout -> automatic session teardown
stalled unauthenticated peer -> monotonic handshake deadline -> slot release
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
- authentication delegated to the PAM helper over a local Unix socket;
- the privileged helper binds an authentication request to the local caller's Unix UID using `SO_PEERCRED`.

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
info    lifecycle/connect/disconnect/rejected extra clients
debug   negotiation/capture/transport/TCP liveness and handshake deadline
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

For development:

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

or for a source installation:

```bash
make status-support
```

## Uninstall

Source installation:

```bash
make uninstall-service
make uninstall-pam-service
```

Binary package:

```bash
sudo apt remove vnc-monitor
```

User config/identity/layout data are preserved unless explicitly removed.
