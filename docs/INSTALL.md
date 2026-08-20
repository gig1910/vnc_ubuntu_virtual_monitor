# Installation and service operation

This document describes the supported `0.1.0-beta.2` deployment model on a GNOME Wayland desktop.

## Runtime model

VNC Monitor has two privilege domains:

- `vnc-monitor.service` is a **systemd user service** running as the logged-in GNOME user so it can reach session D-Bus, Mutter RemoteDesktop/ScreenCast and PipeWire;
- `vnc-monitor-auth.socket` + `vnc-monitor-auth@.service` are **system units** exposing the privileged PAM helper through `/run/vnc-monitor-auth.sock`.

Do not convert the main daemon into a root system service.

The public server is intentionally **STRONG SINGLE CONNECT**. Exactly one accepted viewer owns the display session. The public listener remains responsive while that session runs only so additional connections can be immediately reset/rejected.

The internal LibVNCServer backend is session-scoped: `127.0.0.1:5903` exists only after successful RA2r/PAM authentication and is removed on disconnect.

## Configuration layers

The daemon reads configuration in this order:

```text
built-in defaults
    < /etc/vnc-monitor/config.ini
    < ~/.config/vnc-monitor/config.ini
    < command-line options
```

Both persistent files are optional.

An explicit:

```bash
vnc-monitor --config /path/to/file.ini
```

replaces the normal per-user override path but does **not** disable the system baseline:

```text
built-ins < /etc/vnc-monitor/config.ini < --config FILE < CLI
```

The parser is strict. Unknown sections/keys and invalid values are startup errors.

Repository template:

```text
config/vnc-monitor.conf
```

The `.deb` installs that template as:

```text
/etc/vnc-monitor/config.ini
```

and marks it as a package conffile so administrator edits are preserved by dpkg upgrade semantics.

Source installation creates the per-user file once:

```text
~/.config/vnc-monitor/config.ini
```

and preserves it on later source upgrades. Package installation does not create or overwrite files in user home directories.

Show the effective configuration:

```bash
vnc-monitor --show-config
```

The output lists both config paths and shows the final effective values.

## Source install / upgrade

Run from the repository as the logged-in GNOME desktop user:

```bash
./install.sh
```

Do **not** run it through `sudo`. The script invokes `sudo` itself only for the PAM helper and system units.

The installer:

1. checks required commands and development libraries before changing anything;
2. builds with all logical CPUs by default (`make -j"$(nproc)"`);
3. builds the daemon, PAM helper and source-install socket unit;
4. creates `~/.config/vnc-monitor/config.ini` only when absent;
5. installs/updates both privilege domains;
6. restarts the auth socket and user daemon only after successful build/install;
7. prints effective config and final service status.

Clean rebuild:

```bash
./install.sh --clean
```

Optional job limit:

```bash
./install.sh --jobs 8
# or
JOBS=8 ./install.sh
```

The installer intentionally does not run `git pull`.

### Source build dependency preflight

Required commands include `make`, `cc`, `pkg-config`, `nproc`, `mktemp`, `install`, `sed`, `systemctl` and `sudo`.

Development modules checked through `pkg-config --exists`:

- `libvncserver`;
- `openssl`;
- `nettle`;
- `glib-2.0`;
- `gio-2.0`;
- `gstreamer-1.0`;
- `gstreamer-app-1.0`;
- `gstreamer-video-1.0`;
- `libpipewire-0.3`.

libjpeg and PAM are checked by real compile/link probes against `-ljpeg` and `-lpam`.

Typical Ubuntu **build** packages are:

```bash
sudo apt install \
  build-essential pkg-config \
  libvncserver-dev libssl-dev nettle-dev libglib2.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libpipewire-0.3-dev libjpeg-dev libpam0g-dev
```

These are build-machine requirements, not runtime dependencies of a compiled package.

## Build a binary Debian/Ubuntu package

Build as a normal user:

```bash
./build-deb.sh
```

The builder does not use `sudo`.

Default parallelism is:

```text
make -j"$(nproc)"
```

Optional clean build:

```bash
./build-deb.sh --clean
```

Optional job/output overrides:

```bash
./build-deb.sh --jobs 8
./build-deb.sh --output-dir /tmp/packages
# or
JOBS=8 OUT_DIR=/tmp/packages ./build-deb.sh
```

The package revision can be changed without modifying the application version:

```bash
DEB_REVISION=2 ./build-deb.sh
```

### Binary package build preflight

In addition to the project compiler/library checks, the package builder requires:

- `dpkg-deb`;
- `dpkg-shlibdeps`;
- `dpkg-architecture`.

On Ubuntu these packaging tools are normally provided by `dpkg` / `dpkg-dev`.

The builder extracts `VNC_MONITOR_VERSION` from `include/config.h` and converts prerelease ordering to Debian form. For example:

```text
0.1.0-beta.2 -> 0.1.0~beta.2-1
```

The tilde keeps the beta version ordered before a later final `0.1.0` package.

### What is inside the `.deb`

Main files:

```text
/usr/bin/vnc-monitor
/usr/libexec/vnc-monitor-auth-helper

/usr/lib/systemd/user/vnc-monitor.service
/usr/lib/systemd/system/vnc-monitor-auth.socket
/usr/lib/systemd/system/vnc-monitor-auth@.service

/etc/vnc-monitor/config.ini
/etc/pam.d/vnc-monitor

/usr/share/doc/vnc-monitor/...
```

The package service runs `/usr/bin/vnc-monitor`. It relies on the daemon's normal layered config loading and therefore does not hard-code `--config`.

The package PAM socket is generic and does not contain the username of the machine that built the package. Its Unix socket is locally reachable, while the root helper uses `SO_PEERCRED` and rejects any authentication request whose requested username does not match the Unix account owning the connecting process.

### Runtime dependencies

`build-deb.sh` runs `dpkg-shlibdeps` against the two compiled ELF executables:

```text
/usr/bin/vnc-monitor
/usr/libexec/vnc-monitor-auth-helper
```

The generated shared-library dependencies are written to the package `Depends` field, along with the required PipeWire runtime.

Compiler/build tooling and development packages are **not** runtime dependencies of the finished `.deb`:

```text
NOT runtime dependencies:
  build-essential
  gcc / make
  pkg-config
  dpkg-dev
  lib*-dev
```

Inspect the finished metadata with:

```bash
dpkg-deb --info dist/vnc-monitor_*.deb
```

### Install the `.deb`

Typical output filename:

```text
dist/vnc-monitor_0.1.0~beta.2-1_amd64.deb
```

Install through apt so runtime dependencies are resolved:

```bash
sudo apt install ./dist/vnc-monitor_0.1.0~beta.2-1_amd64.deb
```

The package post-install step reloads system units and enables/restarts:

```text
vnc-monitor-auth.socket
```

The GNOME-facing daemon remains a user service and is deliberately not globally enabled for every account. Enable it once as the intended desktop user:

```bash
systemctl --user daemon-reload
systemctl --user enable --now vnc-monitor.service
```

This avoids having multiple simultaneously logged-in desktop users all trying to bind the same public TCP port.

## Migrating an existing source installation to `.deb`

A source installation places overrides in higher-priority systemd paths:

```text
~/.config/systemd/user/vnc-monitor.service
/etc/systemd/system/vnc-monitor-auth.socket
/etc/systemd/system/vnc-monitor-auth@.service
```

Those would override units installed by the package under `/usr/lib/systemd/...`.

Before the first package installation, remove only the source-installed service files/binaries:

```bash
make uninstall-service
make uninstall-pam-service
```

These targets preserve the user's:

```text
~/.config/vnc-monitor/config.ini
~/.config/vnc-monitor/ra2-server-key.pem
~/.config/vnc-monitor-server/
~/.cache/vnc-monitor/
```

Then install and enable the package service:

```bash
sudo apt install ./dist/vnc-monitor_0.1.0~beta.2-1_amd64.deb
systemctl --user daemon-reload
systemctl --user enable --now vnc-monitor.service
```

The existing per-user config remains a higher-priority override above the new `/etc/vnc-monitor/config.ini` baseline.

## Supported production config

```ini
[capture]
backend=pipewire
timeout-ms=5000
cursor=metadata
fps=60

[display]
mode=auto
width=1024
height=768
layout-remember=on
layout-resave=off

[transport]
vnc-fps=source
latest-only=on
diff-detect=on
diff-tile-size=32

[network]
port=5901
backend-port=5903
backend-bind=127.0.0.1
external-send-buffer=65536
backend-recv-buffer=65536
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
client-handshake-timeout-ms=60000

[ra2]
record-size=16384
coalesce=on
coalesce-us=500
key=$HOME/.config/vnc-monitor/ra2-server-key.pem
auth-socket=/run/vnc-monitor-auth.sock

[logging]
level=info
```

Paths accept absolute paths, `~/...`, and `$HOME/...`.

Missing optional keys inherit the value from the previous configuration layer, ultimately falling back to built-in defaults.

## Strong single-connect policy

The slot is reserved immediately at `accept()`, before RA2/PAM completes. Therefore a second connection cannot race the first during authentication.

The active session runs in one client worker. The main listener continues accepting only to enforce the policy. If another client arrives while the slot is owned, its socket is immediately reset and an `info` log entry is emitted:

```text
Rejected additional client: ADDRESS (strong single-connect policy)
```

This is not multi-client support: there is still exactly one virtual monitor, capture pipeline and adaptive RFB session.

## Lost-client protection

### Established session: TCP liveness

```ini
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
```

These map to Linux TCP keepalive/user-timeout controls. They detect transport failure and are **not** application-idle timers. A healthy viewer displaying a static image may stay connected indefinitely.

If Wi-Fi/LAN connectivity disappears without FIN/RST, TCP failure propagates through the RA2 relay. The session then stops LibVNCServer, capture and Mutter and releases the sole slot.

### Unauthenticated/stalled client: handshake deadline

```ini
client-handshake-timeout-ms=60000
```

This is one monotonic deadline for the bounded handshake `io_*` phase. It is not reset by individual bytes, so a silent or trickle-slow peer cannot reserve the sole slot forever.

After successful authentication the deadline is cleared completely.

## Display size: auto vs fixed

### `mode=auto`

```ini
[display]
mode=auto
width=1024
height=768
```

`width` / `height` are initial/fallback dimensions.

A viewer advertising RFB `ExtendedDesktopSize` can send `SetDesktopSize`. For a valid one-screen request the server coordinates Mutter/PipeWire, FrameBridge, adaptive transport and LibVNCServer framebuffer resize without changing the persistent fallback for the next session.

A viewer supporting only `NewFBSize` can receive a server-side size change but cannot tell the server what size it wants, so it stays on the configured fallback.

### `mode=fixed`

```ini
[display]
mode=fixed
width=1024
height=768
```

The configured dimensions are mandatory and client `SetDesktopSize` requests are rejected with `ResizeProhibited`.

## RA2 identity

Per-user identity path:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

It remains in the user's home for both source and package installations. It is mode `0600` and is not installed into `/etc` or embedded in the package.

## Service lifecycle

Idle:

```text
public TCP listener active
no client worker
no virtual monitor
no PipeWire capture
no internal LibVNCServer listener
```

Active:

```text
one accepted client owns the slot
RA2r/PAM
  -> Mutter virtual monitor
  -> PipeWire capture
  -> per-session LibVNCServer 127.0.0.1:5903
  -> adaptive RFB transport
```

On clean disconnect, dead-peer detection, handshake failure/deadline, or daemon shutdown, the session is torn down and the slot becomes available again.

## Service commands

User daemon:

```bash
systemctl --user status vnc-monitor.service
systemctl --user restart vnc-monitor.service
systemctl --user stop vnc-monitor.service
journalctl --user -u vnc-monitor.service -f
```

PAM layer:

```bash
systemctl status vnc-monitor-auth.socket
```

For source installs, combined project status is also available through:

```bash
make status-support
```

## Uninstall

### Source installation

```bash
make uninstall-service
make uninstall-pam-service
```

Delete user config/identity/layout only when explicitly intended:

```bash
make purge-config
```

### Binary package

```bash
sudo apt remove vnc-monitor
```

Package removal does not delete per-user config, RA2 identity or layout/cache data from home directories.
