# VNC Monitor for GNOME Wayland

`vnc-monitor` turns a VNC-capable tablet into a view-only extended monitor for the **currently active local GNOME Wayland login session**.

Current beta: **`0.1.0-beta.3`**.

## Production architecture

Beta.3 separates machine-wide connection policy from per-user display/capture work:

```text
VNC viewer
    |
    | TCP :5901
    v
vnc-monitor-broker              root system service
    |
    | logind seat0.ActiveSession gate
    | SCM_RIGHTS: pass accepted TCP fd
    v
vnc-monitor --agent             active user's systemd --user service
    |
    | RA2r + PAM as that same Unix user
    v
Mutter virtual monitor -> PipeWire -> adaptive RFB
```

The broker owns only the public listener and session-selection policy. RA2 credentials, PAM authentication, Mutter, PipeWire and framebuffer processing remain in the unprivileged user agent.

The virtual monitor exists only while an authenticated VNC client is connected and is removed on disconnect or policy revocation.

## Session security policy

The broker routes a new connection only when `seat0` has an active **local Wayland `Class=user` session**. GDM/greeter, remote sessions and non-Wayland targets are rejected.

Each accepted connection is bound to the exact logind **Session ID + UID** active at handoff time. It is never moved to another login session.

```text
active session = gig
VNC connects -> agent(gig) -> RA2 username must be gig
```

If the machine switches away from that login session or the session disappears:

```text
Switch User / logoff / seat0 leaves bound Session ID
        -> broker shutdown(TCP)
        -> agent RFB session exits
        -> virtual monitor removed
        -> old VNC connection is permanently dead
```

Returning to the same Unix user does **not** restore the old connection. To see Bob's active desktop, reconnect while Bob's graphical session is active and authenticate as Bob.

The PAM helper independently verifies `SO_PEERCRED` and authenticates only the Unix account owning the calling agent. An inactive user's agent is never selected merely because its user manager still exists after Fast User Switching.

GDM is deliberately **not served**.

## Runtime validation

Beta.3 has been exercised on the target Ubuntu 26.04 GNOME Wayland host with the legacy iPad VNC client. Confirmed scenarios include:

- normal RA2r/PAM connection to the active `gig` session;
- virtual monitor creation on connect and teardown on disconnect;
- immediate rejection of a second simultaneous viewer;
- automatic teardown when the client disappears from Wi-Fi / powers off;
- Switch User or lock transition to GDM immediately revokes the existing VNC connection;
- new VNC connections are rejected while GDM/greeter is active;
- after logging into another local account, the broker routes only to that account's active agent;
- credentials for a different/inactive Unix account are rejected;
- switching back to the original account requires a new VNC connection and fresh authentication.

## Strong single-connect

There is one machine-wide external VNC session at a time. The broker keeps the public listener responsive while a session exists solely to reject additional clients immediately instead of leaving them in the TCP backlog.

Lost-peer protection:

```ini
[network]
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
client-handshake-timeout-ms=60000
```

TCP keepalive/user-timeout detect a vanished network peer; they are not application-idle timers. The handshake timeout is one monotonic deadline for the unauthenticated RA2/PAM phase.

## Configuration

Agent configuration is layered:

```text
built-in defaults
    < /etc/vnc-monitor/config.ini
    < ~/.config/vnc-monitor/config.ini
    < CLI
```

An explicit `--config FILE` replaces the normal per-user override path but still loads `/etc/vnc-monitor/config.ini` first.

### Machine-wide broker port

`[network] port` is special in broker mode. The system broker reads its public port **only** from:

```text
/etc/vnc-monitor/config.ini
```

A per-user config cannot move the machine-wide public listener. Other capture/display/transport/RA2 settings remain per-user through the normal layered agent configuration.

Default display sizing:

```ini
[display]
mode=auto
width=1024
height=768
```

Older viewers that cannot send RFB `ExtendedDesktopSize` use the configured fallback dimensions.

## Build a binary `.deb`

```bash
./build-deb.sh --clean
```

The builder uses `make -j"$(nproc)"` by default, runs dependency preflight, derives runtime ELF dependencies with `dpkg-shlibdeps`, and validates the package with `tools/verify-deb.sh`.

Generated package name follows:

```text
dist/vnc-monitor_0.1.0~beta.3-<revision>_amd64.deb
```

The binary package contains:

```text
/usr/bin/vnc-monitor
/usr/libexec/vnc-monitor-broker
/usr/libexec/vnc-monitor-auth-helper
/usr/lib/systemd/system/vnc-monitor-broker.service
/usr/lib/systemd/system/vnc-monitor-auth.socket
/usr/lib/systemd/system/vnc-monitor-auth@.service
/usr/lib/systemd/user/vnc-monitor.service
/etc/vnc-monitor/config.ini
/etc/pam.d/vnc-monitor
```

The user agent is package-wanted by `graphical-session.target`, so future graphical login sessions start their own agent without package scripts writing into arbitrary home directories.

For a graphical session already running during install/upgrade:

```bash
systemctl --user daemon-reload
systemctl --user restart vnc-monitor.service
sudo systemctl restart vnc-monitor-broker.service
```

## Source install / upgrade

Run as the logged-in GNOME desktop user, not root:

```bash
git pull --ff-only
./install.sh --clean
```

Source installation creates/preserves both:

```text
/etc/vnc-monitor/config.ini
~/.config/vnc-monitor/config.ini
```

and preserves the user's RA2 identity/layout state.

## Runtime services

```bash
systemctl status vnc-monitor-broker.service --no-pager -l
systemctl status vnc-monitor-auth.socket --no-pager -l
systemctl --user status vnc-monitor.service --no-pager -l
```

Logs:

```bash
journalctl -u vnc-monitor-broker.service -f
journalctl --user -u vnc-monitor.service -f
```

Idle state:

```text
broker owns public TCP listener
user agents wait on $XDG_RUNTIME_DIR/vnc-monitor/agent.sock
no virtual monitor
no PipeWire capture
no internal LibVNCServer listener
```

Connected state:

```text
broker validates active seat0 Session ID/UID
    -> passes TCP fd to matching user agent
    -> RA2/PAM
    -> Mutter virtual monitor
    -> PipeWire BGRx + cursor metadata
    -> per-session LibVNCServer backend
    -> adaptive JPEG21 / CopyRect / lossless repair
```

## Existing transport/security properties

Beta.3 retains the proven transport path:

- native PipeWire capture, GStreamer fallback;
- PipeWire `SPA_META_Cursor` cursor metadata;
- RA2r with persistent per-user RSA identity;
- PAM authentication;
- strict view-only input boundary;
- latest-only/backpressure handling;
- adaptive JPEG21 active transport;
- standard CopyRect motion reuse;
- progressive lossless ZRLE repair;
- client-driven framebuffer resize for viewers supporting `ExtendedDesktopSize`.

The server remains display-only: keyboard, pointer/touch, clipboard input and file transfer are disabled.

## Standalone developer mode

Running the binary without `--agent` still starts the direct public listener for focused development/diagnostics:

```bash
./vnc-monitor --verbose debug
```

Production systemd units do **not** use standalone mode.

## Documentation

- [`docs/INSTALL.md`](docs/INSTALL.md)
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md)
- [`SECURITY.md`](SECURITY.md)
- [`CHANGELOG.md`](CHANGELOG.md)
