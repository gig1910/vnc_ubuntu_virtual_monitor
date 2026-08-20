# Installation and service operation

This document describes the `0.1.0-beta.3` broker/agent deployment model for GNOME Wayland.

## Runtime roles

VNC Monitor has three privilege/lifecycle roles:

1. `vnc-monitor-broker.service` — **system service**, owns the machine-wide public TCP listener and queries logind `seat0.ActiveSession`;
2. `vnc-monitor.service` — **systemd user service**, runs `vnc-monitor --agent` inside each GNOME Wayland login session and owns RA2/Mutter/PipeWire/RFB work;
3. `vnc-monitor-auth.socket` + `vnc-monitor-auth@.service` — **system PAM helper**, authenticates only the Unix account owning the calling user-agent process.

The capture/RA2 daemon remains unprivileged; do not convert it into a root system service.

## Session-selection policy

A new external connection is routed only when `seat0` currently selects a local active Wayland `Class=user` session.

GDM/greeter is not served.

The connection is bound to that exact logind Session ID + UID. `Switch User`, lock-to-greeter, logoff or another seat0 transition terminates the connection and removes its virtual monitor. It is never moved to GDM or another user's desktop.

To see a different user's active desktop, reconnect and authenticate again as that user.

## Configuration layers

The user agent reads:

```text
built-in defaults
    < /etc/vnc-monitor/config.ini
    < ~/.config/vnc-monitor/config.ini
    < CLI
```

An explicit `--config FILE` replaces the normal per-user override while preserving the `/etc` baseline.

The machine-wide broker port is special: `[network] port` is read **only** from `/etc/vnc-monitor/config.ini` by `vnc-monitor-broker`.

Per-user configuration may override capture/display/transport/RA2 behaviour but cannot move the public machine listener.

## Build dependencies

Source and `.deb` builders preflight:

- `make`, `cc`, `pkg-config`, `nproc`;
- `libvncserver`, OpenSSL, nettle, GLib/GIO;
- GStreamer base/app/video;
- PipeWire;
- libjpeg compile/link;
- PAM compile/link.

Typical Ubuntu build packages:

```bash
sudo apt install \
  build-essential pkg-config dpkg-dev \
  libvncserver-dev libssl-dev nettle-dev libglib2.0-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libpipewire-0.3-dev libjpeg-dev libpam0g-dev
```

These are **build-machine dependencies only**. The compiled `.deb` derives runtime shared-library dependencies from its ELF files with `dpkg-shlibdeps`.

## Build binary `.deb`

From the checked-out source tree:

```bash
git pull --ff-only
./build-deb.sh --clean
```

The builder uses `make -j"$(nproc)"` by default and runs `tools/verify-deb.sh` automatically.

Generated package name follows:

```text
dist/vnc-monitor_0.1.0~beta.3-<revision>_amd64.deb
```

A valid build ends with:

```text
===== VNC MONITOR: PACKAGE VERIFY =====
...
Result:       OK
```

The package contains the broker, user agent, PAM helper, system/user units, `/etc/vnc-monitor/config.ini` and documentation. It never contains a RA2 private key.

## Package install

Install the generated package, for example:

```bash
sudo apt install ./dist/vnc-monitor_0.1.0~beta.3-3_amd64.deb
```

The exact Debian revision may be newer; use the file produced by `build-deb.sh`.

The package:

- enables/restarts the PAM socket;
- enables the system broker;
- ships a global user-unit Wants link under `graphical-session.target.wants`, so future graphical login sessions automatically start their agent;
- does not write into arbitrary users' home directories.

### Already-running graphical session

A root package maintainer script cannot safely reload every existing user's `systemd --user` manager. Therefore after first install/upgrade, in the currently logged-in GNOME account run:

```bash
systemctl --user daemon-reload
systemctl --user restart vnc-monitor.service
sudo systemctl restart vnc-monitor-broker.service
```

This is especially important for beta.2 -> beta.3 because the old beta.2 user daemon owned the public TCP port itself.

## Migrating an old source installation to `.deb`

Old source installs place higher-priority unit files in user/system override paths. Remove those overrides before installing the package while preserving user state:

```bash
make uninstall-service
make uninstall-broker-service 2>/dev/null || true
make uninstall-pam-service
sudo apt install ./dist/vnc-monitor_0.1.0~beta.3-<revision>_amd64.deb
```

Use the actual generated filename in place of `<revision>`.

Preserved state includes:

```text
~/.config/vnc-monitor/config.ini
~/.config/vnc-monitor/ra2-server-key.pem
~/.config/vnc-monitor-server/
~/.cache/vnc-monitor/
```

Then reload/restart the current user agent and broker as shown above.

## Source install / upgrade

Run as the logged-in GNOME desktop user, not root:

```bash
./install.sh --clean
```

`install.sh` invokes `sudo` only for system broker/PAM files.

The beta.2 -> beta.3 source upgrade order is deliberate:

```text
build everything
  -> replace user binary/unit
  -> restart old standalone daemon as --agent (releases TCP port)
  -> install/start system broker
```

Source installation creates `/etc/vnc-monitor/config.ini` only if absent and creates the user's `~/.config/vnc-monitor/config.ini` only if absent. Both are preserved later.

## PAM socket in multi-user mode

The PAM socket is intentionally reachable by local user agents:

```text
/run/vnc-monitor-auth.sock
SocketMode=0666
```

This is not the authorization boundary. The root helper obtains `SO_PEERCRED` from the connecting process and refuses any requested username different from that process' Unix account.

Thus a `guest`/`bob` agent can authenticate only its own Unix account; it cannot test/authenticate `gig` credentials.

## Broker/agent peer verification

The agent socket is private to each runtime directory:

```text
/run/user/UID/vnc-monitor/agent.sock
```

The broker validates the agent socket UID against the active logind UID.

The agent validates the broker through `SO_PEERCRED`. On systems where the hardened user service maps only its own UID in a user namespace, host root may appear as an overflow UID. Overflow UID is not trusted by itself; the peer PID must belong exactly to:

```text
/system.slice/vnc-monitor-broker.service
```

## Service status

System broker:

```bash
systemctl status vnc-monitor-broker.service --no-pager -l
journalctl -u vnc-monitor-broker.service -n 100 --no-pager
```

Current user agent:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
journalctl --user -u vnc-monitor.service -n 100 --no-pager
```

PAM layer:

```bash
systemctl status vnc-monitor-auth.socket --no-pager -l
ls -l /run/vnc-monitor-auth.sock
```

Source installation aggregate:

```bash
make status-support
```

## Expected idle state

```text
vnc-monitor-broker: listening on system [network] port
vnc-monitor --agent: waiting on /run/user/UID/vnc-monitor/agent.sock
no virtual monitor
no PipeWire capture
no internal LibVNCServer listener
```

## Expected connection state

```text
broker accepts external client
  -> validates current logind seat0 Session ID/UID
  -> passes TCP fd with SCM_RIGHTS
  -> matching user agent performs RA2/PAM
  -> virtual monitor/capture starts
  -> internal LibVNCServer starts
```

Second external clients are rejected immediately.

## Confirmed beta.3 session tests

The broker/agent lifecycle has been tested with:

1. normal VNC connection to active `gig`;
2. Switch User / lock transition to GDM, which immediately disconnects the old VNC session;
3. VNC attempt while GDM is active, which is rejected;
4. login as another local account (`guest`) and successful routing only to that account's agent;
5. wrong/inactive account credentials rejected;
6. switching back to `gig`, requiring a fresh VNC connection and authentication;
7. second-client immediate rejection and dead Wi-Fi client cleanup.

The old connection must never resume even if the machine later switches back to the same account.

## Display sizing

Default:

```ini
[display]
mode=auto
width=1024
height=768
```

`auto` uses width/height as initial/fallback dimensions and allows clients supporting RFB `ExtendedDesktopSize` to request another size. Older clients supporting only `NewFBSize` cannot communicate preferred dimensions and use the fallback.

`fixed` forces the configured dimensions and rejects client resize.

## Standalone developer mode

For focused diagnostics only, running without `--agent` retains a direct public listener:

```bash
./vnc-monitor --verbose debug
```

Do not run standalone mode at the same time as the production broker on the same public port.

## Removal

Binary package:

```bash
sudo apt remove vnc-monitor
```

Source stack:

```bash
make uninstall-support
```

User config, RA2 identity and layout/cache remain unless intentionally purged.
