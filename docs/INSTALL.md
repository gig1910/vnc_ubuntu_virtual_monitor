# Installation and service operation

This document describes the supported `0.1.0-beta.2` deployment model on a GNOME Wayland desktop.

## Runtime model

VNC Monitor is split into two privilege domains:

- `vnc-monitor.service` is a **systemd user service** running as the logged-in GNOME user so it can reach session D-Bus, Mutter RemoteDesktop/ScreenCast and PipeWire;
- `vnc-monitor-auth.socket` + `vnc-monitor-auth@.service` are **system units** exposing the small privileged PAM helper through `/run/vnc-monitor-auth.sock`.

Do not convert the main daemon to a root system service.

The public server is intentionally **STRONG SINGLE CONNECT**. Exactly one accepted viewer owns the display session. The public listener remains responsive while that session runs only so additional connections can be immediately reset/rejected.

The internal LibVNCServer backend is session-scoped: `127.0.0.1:5903` exists only after successful RA2r/PAM authentication and is removed on disconnect.

## Unified installer

Run from the repository as the logged-in GNOME desktop user:

```bash
./install.sh
```

Do **not** run it through `sudo`. The script invokes `sudo` itself only for the PAM helper and system units.

The installer:

1. checks all required commands and development libraries before changing anything;
2. builds the daemon, PAM helper and generated PAM socket unit;
3. creates `~/.config/vnc-monitor/config.ini` only when it does not already exist;
4. installs/updates both privilege domains;
5. restarts the auth socket and user daemon only after successful build/install;
6. prints the effective parsed configuration and final service status.

Default parallelism is:

```text
make -j"$(nproc)"
```

Clean rebuild:

```bash
./install.sh --clean
```

Optional manual limit:

```bash
./install.sh --jobs 8
# or
JOBS=8 ./install.sh
```

The installer intentionally does not run `git pull`.

## Dependency preflight

Required commands include `make`, `cc`, `pkg-config`, `nproc`, `mktemp`, `install`, `sed`, `systemctl` and `sudo`.

The following development modules are verified through `pkg-config --exists` and their versions are printed:

- `libvncserver`;
- `openssl`;
- `nettle`;
- `glib-2.0`;
- `gio-2.0`;
- `gstreamer-1.0`;
- `gstreamer-app-1.0`;
- `gstreamer-video-1.0`;
- `libpipewire-0.3`.

libjpeg and PAM are checked with real compile/link probes against `-ljpeg` and `-lpam`.

Typical Ubuntu development packages are:

```bash
sudo apt install \
  build-essential pkg-config \
  libvncserver-dev libssl-dev nettle-dev libglib2.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libpipewire-0.3-dev libjpeg-dev libpam0g-dev
```

The probes, not the package names, are authoritative.

## Install / upgrade

```bash
git switch 0.1.0-beta
git pull --ff-only
./install.sh
```

The installer is idempotent for an existing deployment. It does not uninstall first: the old daemon keeps running during compilation, files are replaced only after a successful build, and then the installed services are restarted.

An active VNC connection is naturally disconnected by the final daemon restart during an upgrade.

Because beta.2 changes headers and session lifecycle substantially, use one clean build when first moving to it:

```bash
git pull --ff-only
./install.sh --clean
```

## Persistent configuration

Repository template:

```text
config/vnc-monitor.conf
```

Installed file:

```text
~/.config/vnc-monitor/config.ini
```

The installed file is mode `0600` and is **created once, then preserved on upgrades**.

Configuration precedence:

```text
built-in defaults < config.ini < command-line options
```

The user service starts:

```text
%h/.local/bin/vnc-monitor --config %h/.config/vnc-monitor/config.ini
```

Show the effective configuration without starting a server:

```bash
~/.local/bin/vnc-monitor \
  --config ~/.config/vnc-monitor/config.ini \
  --show-config
```

The parser is strict: unknown sections/keys and invalid values are startup errors.

After editing the config:

```bash
systemctl --user restart vnc-monitor.service
```

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

Older installed configs do not have to be rewritten when new optional keys are introduced: missing keys inherit built-in defaults.

## Strong single-connect policy

The slot is reserved immediately at `accept()`, before RA2/PAM completes. Therefore a second connection cannot race the first during authentication.

The active session runs in one client worker. The main listener continues accepting only to enforce the policy. If another client arrives while the slot is owned, its socket is closed with an immediate reset and an `info` log entry is emitted:

```text
Rejected additional client: ADDRESS (strong single-connect policy)
```

This is not multi-client support: there is still exactly one virtual monitor, one capture pipeline and one adaptive RFB session.

## Lost-client protection

There are two distinct protections because established and unauthenticated connections fail differently.

### Established session: TCP liveness

The accepted external socket requires:

```ini
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
```

These map to Linux `SO_KEEPALIVE`, `TCP_KEEPIDLE`, `TCP_KEEPINTVL`, `TCP_KEEPCNT`, and `TCP_USER_TIMEOUT` when available.

They are **transport liveness**, not application-idle timers. A healthy viewer displaying a completely static image may remain connected indefinitely because its TCP stack still responds.

If Wi-Fi/LAN connectivity disappears without FIN/RST, kernel TCP failure eventually propagates through the RA2 relay. The session worker then stops LibVNCServer, capture and Mutter, and releases the sole client slot.

Exact wall-clock detection time depends on TCP/network state; the defaults are intended to recover in the order of tens of seconds rather than leave a half-open session indefinitely.

### Unauthenticated/stalled client: handshake deadline

```ini
client-handshake-timeout-ms=60000
```

This is one **monotonic deadline** for the bounded handshake `io_*` phase. It is not reset by each individual byte, so a silent or trickle-slow client cannot reserve the sole slot forever.

The deadline participates directly in the project's `poll()`-based I/O wrapper and therefore covers external RA2 reads/writes plus auth-helper request/response waits that use `io_*` in the same worker.

After successful authentication the deadline is cleared completely. The long-lived VNC session is then governed only by TCP liveness, not by an application inactivity timeout.

## Display size: auto vs fixed

### `mode=auto`

```ini
[display]
mode=auto
width=1024
height=768
```

`width` / `height` are the initial/fallback dimensions.

A viewer advertising RFB `ExtendedDesktopSize` can send `SetDesktopSize`. For a valid one-screen request beta.2 coordinates:

1. Mutter virtual monitor/capture;
2. FrameBridge storage;
3. adaptive JPEG/repair/CopyRect size-dependent state;
4. LibVNCServer framebuffer through `rfbNewFramebuffer()`;
5. dimension-specific layout cache.

The RFB connection stays open. JPEG21 capability is preserved; CopyRect exactness/reference state is invalidated and rebuilt for the new dimensions.

Only one screen at `(0,0)` spanning the framebuffer is accepted.

### Older clients

`NewFBSize` / `DesktopSize` (`-223`) only allows a viewer to **receive** a server-side size change. It does not communicate preferred dimensions.

A viewer must advertise `ExtendedDesktopSize` (`-308`) and send `SetDesktopSize` to drive auto sizing. Otherwise the configured fallback is used.

### `mode=fixed`

```ini
[display]
mode=fixed
width=1024
height=768
```

The configured dimensions are mandatory and client `SetDesktopSize` requests are rejected with `ResizeProhibited`.

## RA2 identity

Current identity path:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

Older development versions used `./ra2-server-key.pem`. If the installed identity does not yet exist, the installation path migrates the legacy key when available; otherwise the server creates a persistent identity on first use.

The identity is mode `0600`; its config directory is mode `0700`.

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

Combined project helper:

```bash
make status-support
```

## Logging

Configure normally through:

```ini
[logging]
level=debug
```

Values: `error`, `info`, `debug`, `trace` or `0..3`.

At `info`, lifecycle and rejected extra clients are visible. At `debug`, negotiated capture/transport state and effective TCP liveness/deadline values are shown. Use `trace` only for short diagnostics.

CLI `--verbose` overrides the file for foreground tests.

## Firewall

The external viewer connects to `[network] port` (TCP/5901 by default). The internal backend uses `[network] backend-bind:backend-port` (`127.0.0.1:5903` by default) only during an authenticated session and must not be exposed externally.

## Layout cache

Mutter layout state remains under the historical beta namespace:

```text
~/.config/vnc-monitor-server/
```

Cache files are dimension-specific, so newly requested framebuffer sizes can learn independent GNOME placement.

## Manual development install

```bash
make -j"$(nproc)"
make install-service
```

For normal deployment prefer `./install.sh` because it also performs dependency preflight, deterministic upgrade restart and effective-config validation.

## Uninstall

Remove user daemon/service while preserving config/identity/layout:

```bash
make uninstall-service
```

Remove PAM support separately:

```bash
make uninstall-pam-service
```

Delete user config, identity and layout only when explicitly intended:

```bash
make purge-config
```

Deleting/changing the RA2 identity can require the viewer to accept a new server key.
