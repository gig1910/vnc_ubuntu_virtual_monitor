# Installation and service operation

This document describes the supported `0.1.0-beta.1` deployment model on a GNOME Wayland desktop.

## Runtime model

VNC Monitor is split deliberately into two privilege domains:

- `vnc-monitor.service` is a **systemd user service**. It must run as the logged-in GNOME user so it can reach the session D-Bus, Mutter RemoteDesktop/ScreenCast interfaces and PipeWire.
- `vnc-monitor-auth.socket` and `vnc-monitor-auth@.service` are **system units**. They expose the small privileged PAM authentication helper through `/run/vnc-monitor-auth.sock`.

Do not convert the main daemon to a root system service. Doing so breaks the session ownership model and is not a supported installation.

## Unified installer

The preferred beta installation entrypoint is the executable repository-root script:

```bash
./install.sh
```

It performs the complete build and installation of both service layers in the correct privilege context:

1. performs a dependency preflight before any compilation or service changes;
2. builds `vnc-monitor`, the PAM helper and generated PAM socket unit;
3. installs/updates the PAM helper plus `vnc-monitor-auth.socket` / `vnc-monitor-auth@.service` using the Makefile's scoped `sudo` operations;
4. installs `~/.local/bin/vnc-monitor` and `~/.config/systemd/user/vnc-monitor.service`;
5. explicitly restarts both the auth socket and user daemon so an upgrade immediately runs the newly installed binaries/units;
6. prints the final status of both layers.

The script must be run as the logged-in GNOME desktop user, **not** through `sudo`.

Default parallelism is the number of logical CPUs reported by `nproc`:

```bash
./install.sh
```

Clean rebuild:

```bash
./install.sh --clean
```

Override build parallelism only when needed:

```bash
./install.sh --jobs 8
# or
JOBS=8 ./install.sh
```

The installer intentionally does **not** run `git pull`; updating source code and installing it remain separate operations.

## Dependency preflight

`install.sh` verifies dependencies before it runs `make`.

Required commands are checked first:

- `make`;
- `cc`;
- `pkg-config`;
- `nproc`;
- `mktemp`;
- `install`;
- `sed`;
- `systemctl`;
- `sudo`.

The following development libraries are verified through `pkg-config --exists` and their versions are printed:

- `libvncserver`;
- `openssl`;
- `nettle`;
- `glib-2.0`;
- `gio-2.0`;
- `gstreamer-1.0`;
- `gstreamer-app-1.0`;
- `gstreamer-video-1.0`;
- `libpipewire-0.3`.

libjpeg and PAM are checked with real compile/link probes against `-ljpeg` and `-lpam`. This verifies both development headers and linker-visible libraries rather than relying on distribution package names.

The installer also verifies that the systemd user manager is reachable from the current shell. If any preflight check fails, the script exits before compilation and before changing either service layer.

Typical Ubuntu development packages are:

```bash
sudo apt install \
  build-essential pkg-config \
  libvncserver-dev libssl-dev nettle-dev libglib2.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libpipewire-0.3-dev libjpeg-dev libpam0g-dev
```

The checks themselves, rather than the package names above, are authoritative because package/provider names can differ between Ubuntu releases.

## First beta build

After switching from a pre-beta checkout, perform one clean build because source names and dependency generation changed:

```bash
git switch 0.1.0-beta
git pull --ff-only
./install.sh --clean
```

After that, ordinary incremental installs are sufficient:

```bash
git pull --ff-only
./install.sh
```

The installer uses `make -j"$(nproc)"` by default. `-MMD -MP` dependency files are generated automatically, so header changes rebuild their dependants.

## Preserve the existing RA2 identity

Older development versions stored the server key as:

```text
./ra2-server-key.pem
```

The beta stores it under:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

`make install`/`make install-service` (and therefore `./install.sh`) migrates the existing local key when the new path does not yet exist. This preserves the server identity already accepted by the old viewer.

The file must remain mode `0600`; the directory is mode `0700`.

## Manual install and start

For development/debugging, the underlying Makefile target remains supported:

```bash
make -j"$(nproc)"
make install-service
```

Run it as the logged-in desktop user. Do not run `sudo make install-service`. The Makefile invokes `sudo` only for the PAM helper and its systemd system units.

The target performs:

1. build `vnc-monitor`;
2. install it as `~/.local/bin/vnc-monitor`;
3. create private config/cache directories;
4. install/update the PAM helper and socket;
5. install `~/.config/systemd/user/vnc-monitor.service`;
6. run `systemctl --user enable --now vnc-monitor.service`.

For upgrades, `./install.sh` is preferred because it also performs the dependency preflight and explicitly restarts already-active units after installation.

## Service behaviour

At the default `info` log level, an idle service only listens on TCP/5901. It does **not** create a virtual monitor while no viewer is connected.

After an authenticated client connects:

1. RA2r authentication completes;
2. a Mutter virtual monitor is created in the current Wayland session;
3. PipeWire capture starts;
4. the RFB stream is relayed to the client;
5. after disconnect the layout is saved, capture stops and the virtual monitor disappears.

## Service commands

```bash
make status-service
make logs-service
make restart-service
make stop-service
```

Equivalent systemd commands:

```bash
systemctl --user status vnc-monitor.service
systemctl --user restart vnc-monitor.service
systemctl --user stop vnc-monitor.service
journalctl --user -u vnc-monitor.service -f
```

PAM helper status:

```bash
make pam-service-status
```

## Change logging level for the service

Create a user override:

```bash
systemctl --user edit vnc-monitor.service
```

Example:

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

Use `trace` only for short diagnostics; it emits per-update transport information.

## Firewall

The external viewer connects to TCP/5901 by default. The internal LibVNCServer port is bound to `127.0.0.1:5903` and must not be exposed externally.

If a host firewall is enabled, permit TCP/5901 only from the networks/hosts that should reach the viewer service.

## Layout cache

The monitor layout cache is maintained by the existing Mutter layout-cache implementation. During beta stabilization its historical on-disk namespace is preserved for compatibility with already learned layouts.

A first successful session can be arranged in GNOME Settings; subsequent sessions restore the saved layout when `--layout-remember on` is enabled.

Use `--layout-resave on` only when intentionally replacing the cached arrangement.

## Uninstall

Remove only the user daemon/service, preserving identity and layout data:

```bash
make uninstall-service
```

Remove PAM support separately:

```bash
make uninstall-pam-service
```

Remove obsolete redraw/HW-cursor development remnants:

```bash
make cleanup-obsolete-support
```

Delete user configuration, including the RA2 identity, only when explicitly intended:

```bash
make purge-config
```

Changing/deleting the RA2 identity may require the old client to accept a new server key.
