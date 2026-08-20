# Installation and service operation

This document describes the supported `0.1.0-beta.2` deployment model on a GNOME Wayland desktop.

## Runtime model

VNC Monitor is split deliberately into two privilege domains:

- `vnc-monitor.service` is a **systemd user service**. It runs as the logged-in GNOME user so it can reach the session D-Bus, Mutter RemoteDesktop/ScreenCast interfaces and PipeWire.
- `vnc-monitor-auth.socket` and `vnc-monitor-auth@.service` are **system units**. They expose the small privileged PAM authentication helper through `/run/vnc-monitor-auth.sock`.

Do not convert the main daemon to a root system service. Doing so breaks the session ownership model and is not a supported installation.

The internal LibVNCServer backend is session-scoped in beta.2: `127.0.0.1:5903` is opened only after a viewer has completed RA2r/PAM authentication and is closed on disconnect.

## Unified installer

The preferred installation/upgrade entrypoint is:

```bash
./install.sh
```

Run it as the logged-in GNOME desktop user, **not** through `sudo`.

The installer:

1. performs dependency preflight before compilation or service changes;
2. builds `vnc-monitor`, the PAM helper and generated PAM socket unit;
3. creates `~/.config/vnc-monitor/config.ini` from the repository template only when it does not already exist;
4. installs/updates the PAM helper and its system units using scoped `sudo` operations;
5. installs `~/.local/bin/vnc-monitor` and the systemd user unit;
6. restarts the auth socket and user daemon only after successful installation;
7. prints the effective parsed config and final status of both service layers.

Default parallelism is the number of logical CPUs reported by `nproc`:

```bash
./install.sh
```

Clean rebuild:

```bash
./install.sh --clean
```

Override parallelism only when needed:

```bash
./install.sh --jobs 8
# or
JOBS=8 ./install.sh
```

The installer intentionally does **not** run `git pull`; updating source code and installing it remain separate operations.

## Dependency preflight

`install.sh` verifies dependencies before it runs `make`.

Required commands:

- `make`;
- `cc`;
- `pkg-config`;
- `nproc`;
- `mktemp`;
- `install`;
- `sed`;
- `systemctl`;
- `sudo`.

The following development libraries are checked through `pkg-config --exists` and their versions are printed:

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

The probes, rather than package names, are authoritative.

## Install / upgrade

```bash
git switch 0.1.0-beta
git pull --ff-only
./install.sh
```

The build defaults to `make -j"$(nproc)"`. `-MMD -MP` dependency files make ordinary incremental upgrades sufficient.

The installer is idempotent for an existing deployment. It does not uninstall first: the old daemon keeps running while the new version is compiled; files are replaced only after successful build, then both service layers are restarted.

An active VNC session will naturally be disconnected by the final daemon restart during upgrade.

## Persistent configuration

Repository template:

```text
config/vnc-monitor.conf
```

Installed file:

```text
~/.config/vnc-monitor/config.ini
```

The installed file is mode `0600`. It is **created once and preserved on upgrades**.

Configuration precedence:

```text
built-in defaults < config.ini < command-line options
```

The systemd service starts:

```text
%h/.local/bin/vnc-monitor --config %h/.config/vnc-monitor/config.ini
```

Show the effective configuration without starting a server:

```bash
~/.local/bin/vnc-monitor \
  --config ~/.config/vnc-monitor/config.ini \
  --show-config
```

The parser is intentionally strict: unknown sections/keys and invalid values are startup errors rather than silently ignored settings.

After editing the config:

```bash
systemctl --user restart vnc-monitor.service
```

### Main config sections

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

[ra2]
record-size=16384
coalesce=on
coalesce-us=500
key=$HOME/.config/vnc-monitor/ra2-server-key.pem
auth-socket=/run/vnc-monitor-auth.sock

[rfb]
zrle=on
raw=on
cursor=on
newfbsize=on

[logging]
level=info
```

Paths accept absolute paths, `~/...`, or `$HOME/...`.

## Display size: auto vs fixed

### `mode=auto`

```ini
[display]
mode=auto
width=1024
height=768
```

`width`/`height` are the initial and fallback size.

A client that advertises the RFB `ExtendedDesktopSize` pseudo-encoding can send `SetDesktopSize`. For a valid one-screen request, beta.2 transactionally changes:

1. the Mutter virtual monitor/capture;
2. the FrameBridge storage;
3. adaptive JPEG/repair/CopyRect size-dependent state;
4. the LibVNCServer framebuffer via `rfbNewFramebuffer()`;
5. the layout-cache context for the new dimensions.

The RFB connection remains the same. CopyRect exactness is invalidated and relearned after the resize; JPEG capability negotiation is preserved.

If the requested monitor/capture cannot be created, the server attempts to restore the previous size and returns an RFB resize error.

Only one virtual screen spanning the framebuffer is accepted. Multi-screen `SetDesktopSize` layouts are rejected because this project exposes one Mutter virtual monitor per VNC session.

### Older clients

`NewFBSize`/`DesktopSize` support alone lets a viewer **receive** a size change from the server. It does not communicate a preferred size to the server.

Therefore a viewer that does not advertise `ExtendedDesktopSize` cannot drive auto sizing. Such a client simply receives the configured fallback `width`/`height`.

### `mode=fixed`

```ini
[display]
mode=fixed
width=1024
height=768
```

The monitor is always created at the configured dimensions and `SetDesktopSize` requests are rejected with `ResizeProhibited`.

## Preserve the existing RA2 identity

Older development versions stored the key as:

```text
./ra2-server-key.pem
```

The beta stores it under:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

If the installed identity does not yet exist, `make install`/`install.sh` migrates the legacy local key when available. Otherwise the running server creates a persistent identity on first use.

The identity file is mode `0600`; the config directory is mode `0700`.

## Service behaviour

Idle:

```text
public TCP listener active
no virtual monitor
no PipeWire capture
no internal LibVNCServer listener
```

Authenticated connection:

```text
RA2r/PAM
  -> Mutter virtual monitor
  -> PipeWire capture
  -> per-session LibVNCServer on 127.0.0.1:5903
  -> adaptive RFB transport
```

Disconnect tears down the backend, capture and virtual monitor and returns to the public listener.

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

or:

```bash
make status-support
```

## Logging

Prefer the config:

```ini
[logging]
level=debug
```

then restart the user service. Available values are `error`, `info`, `debug`, and `trace` (or `0..3`). Use `trace` only for short diagnostics.

CLI `--verbose` still overrides the config for foreground tests.

## Firewall

The external viewer connects to `network.port` (TCP/5901 by default). The internal backend binds to `network.backend-bind:backend-port` (default `127.0.0.1:5903`) only during an authenticated session and must not be exposed externally.

## Layout cache

The monitor layout cache uses Mutter `org.gnome.Mutter.DisplayConfig` state and retains its historical `~/.config/vnc-monitor-server/` namespace during beta stabilization.

A separate cache file is keyed by framebuffer dimensions, so a newly requested client size can have its own remembered GNOME placement.

## Manual development install

The low-level targets remain available:

```bash
make -j"$(nproc)"
make install-service
```

For normal deployment prefer `./install.sh`, which adds dependency preflight, deterministic restart and effective-config output.

## Uninstall

Remove the user daemon/service while preserving config/identity/layout:

```bash
make uninstall-service
```

Remove PAM support separately:

```bash
make uninstall-pam-service
```

Delete all user configuration, identity and layout only when explicitly intended:

```bash
make purge-config
```

Changing/deleting the RA2 identity may require a viewer to accept a new server key.
