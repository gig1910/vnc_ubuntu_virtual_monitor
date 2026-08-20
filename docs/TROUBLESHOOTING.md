# Troubleshooting

Beta.3 has separate broker, user-agent and PAM logs. Start with all three:

```bash
systemctl status vnc-monitor-broker.service --no-pager -l
journalctl -u vnc-monitor-broker.service -n 100 --no-pager

systemctl --user status vnc-monitor.service --no-pager -l
journalctl --user -u vnc-monitor.service -n 100 --no-pager

systemctl status vnc-monitor-auth.socket --no-pager -l
```

## Public TCP port is not listening

Production broker mode requires the **system** broker to own the public port:

```bash
systemctl status vnc-monitor-broker.service --no-pager -l
ss -ltnp | grep ':5901'
```

The broker reads `[network] port` only from:

```text
/etc/vnc-monitor/config.ini
```

A user override does not change the machine-wide listener.

After beta.2 -> beta.3 package upgrade, the already-running old beta.2 user service may temporarily still own the port. Run in that graphical account:

```bash
systemctl --user daemon-reload
systemctl --user restart vnc-monitor.service
sudo systemctl restart vnc-monitor-broker.service
```

The broker unit is configured not to exhaust its start-limit during this migration handoff.

If another unrelated process owns the configured port, resolve that conflict rather than changing a per-user config.

## Broker is running but every connection is immediately rejected

Check the broker journal. A normal refusal without a routable desktop looks like:

```text
Broker rejected ...: no active local Wayland user session on seat0
```

This is expected while GDM/greeter is active.

Inspect logind:

```bash
loginctl seat-status seat0
loginctl list-sessions
```

For the intended target session, confirm it is a local active Wayland user session.

Beta.3 deliberately rejects:

- GDM/greeter;
- inactive sessions left behind by Fast User Switching;
- remote sessions;
- non-Wayland sessions.

## Active desktop exists but broker says agent unavailable

The active user's session agent should expose:

```text
/run/user/UID/vnc-monitor/agent.sock
```

As that user:

```bash
systemctl --user status vnc-monitor.service --no-pager -l
ls -ld "$XDG_RUNTIME_DIR/vnc-monitor"
ls -l "$XDG_RUNTIME_DIR/vnc-monitor/agent.sock"
```

The package unit uses:

```text
ExecStart=/usr/bin/vnc-monitor --agent
RuntimeDirectory=vnc-monitor
```

For a package installed into an already-running graphical session:

```bash
systemctl --user daemon-reload
systemctl --user restart vnc-monitor.service
```

Future graphical logins get the agent automatically through the packaged `graphical-session.target.wants` link.

## Wrong user credentials are rejected

This is intentional.

The broker selects the current active logind session before RA2 authentication. The agent then authenticates only its own Unix username through the PAM helper's `SO_PEERCRED` check.

Example:

```text
active desktop: bob
broker target:  agent uid=bob
RA2 username:   gig
result:         denied
```

To connect as `gig`, `gig` must first be the active local graphical session, then the viewer must reconnect and authenticate as `gig`.

## Switch User does not preserve VNC

Also intentional.

The VNC connection is bound to the exact logind Session ID active at handoff. When seat0 leaves that Session ID, the broker shuts down its duplicate of the TCP socket. Normal agent teardown then removes the virtual monitor.

Expected sequence:

```text
Broker routed ... logind-session=N
...
Broker disconnecting ...: bound session N is no longer active
...
Client disconnected
```

The connection must not move to GDM or another user. Switching back to the original account still requires a new VNC connection and new RA2/PAM authentication.

## New connection at GDM is rejected

Expected. Beta.3 does not serve the greeter/login screen.

The system broker may still be listening on TCP/5901, but it resets the client because there is no eligible active `Class=user` Wayland target.

## Broker crashes while VNC is connected

The user-agent keeps a control-channel guard. Loss of the broker control channel shuts down the handed-off VNC socket as a fail-closed policy.

After systemd restarts the broker, the old VNC connection does not resume. Reconnect normally.

## A second viewer fails immediately

Expected strong single-connect behaviour.

The machine-wide broker reserves one external viewer slot. Additional clients are accepted only to be immediately reset, so they do not wait in the TCP backlog.

## Wi-Fi disappears but monitor remains

The handed-off client socket still uses the beta.2 TCP liveness policy:

```ini
[network]
client-keepalive-idle=15
client-keepalive-interval=5
client-keepalive-probes=3
client-user-timeout-ms=20000
```

These are transport-liveness settings, not image/activity timers. A healthy static VNC session can remain indefinitely.

When a vanished peer is detected, the user-agent exits the RFB relay, sends `DONE` to the broker and removes the virtual monitor.

## Client connects but stalls before authentication

The active agent applies:

```ini
client-handshake-timeout-ms=60000
```

This is one monotonic deadline across the unauthenticated RA2/PAM I/O phase, not a per-byte timeout. A silent or trickle-slow peer cannot reserve the only machine slot indefinitely.

## PAM socket problems

Check:

```bash
systemctl status vnc-monitor-auth.socket --no-pager -l
ls -l /run/vnc-monitor-auth.sock
```

Multi-user beta.3 intentionally uses a generic local socket (`0666`). Security is enforced by the privileged helper using `SO_PEERCRED`: it only authenticates the Unix username that owns the connecting agent.

If authentication worked in beta.2 but not beta.3, make sure an old source-installed socket unit with `SocketUser=`/`SocketMode=0600` is not overriding the generic beta.3 unit:

```bash
systemctl cat vnc-monitor-auth.socket
systemctl show vnc-monitor-auth.socket -p FragmentPath
```

## Confirm package paths

Package install should resolve to:

```bash
systemctl show vnc-monitor-broker.service -p FragmentPath
systemctl --user show vnc-monitor.service -p FragmentPath -p ExecStart
systemctl show vnc-monitor-auth.socket -p FragmentPath
command -v vnc-monitor
```

Expected package paths:

```text
/usr/lib/systemd/system/vnc-monitor-broker.service
/usr/lib/systemd/user/vnc-monitor.service
/usr/lib/systemd/system/vnc-monitor-auth.socket
/usr/bin/vnc-monitor
```

Source-install overrides use `/etc/systemd/system`, `~/.config/systemd/user`, `/usr/local/libexec` and `~/.local/bin` and take precedence when present.

## Check layered config

```bash
/usr/bin/vnc-monitor --show-config
```

Agent precedence is:

```text
built-ins < /etc/vnc-monitor/config.ini < user/--config < CLI
```

Remember that the displayed effective per-user `public port` is relevant to standalone developer mode; the production broker's actual public port comes only from `/etc/vnc-monitor/config.ini`.

## Authentication succeeds but no monitor appears

The selected user agent must run inside the active GNOME Wayland login session with access to that session's D-Bus and PipeWire.

Check:

```bash
systemctl --user status pipewire.service --no-pager
journalctl --user -u vnc-monitor.service -n 100 --no-pager
```

Mutter RemoteDesktop/ScreenCast and PipeWire failures are logged by the agent, not the broker.

## Why TCP/5903 is absent while idle

Intentional. The internal LibVNCServer backend is session-scoped and starts only after successful RA2/PAM authentication. It stops again on disconnect or policy revocation.

## Client cannot request another framebuffer size

A viewer must advertise RFB `ExtendedDesktopSize` (`-308`) and send `SetDesktopSize`. A viewer supporting only `NewFBSize` (`-223`) can receive a server resize but cannot tell the server its preferred dimensions.

Such clients use the configured fallback width/height.

## GUI movement is jerky

Transport behaviour is unchanged from beta.2. At `trace`, inspect JPEG21/CopyRect records. Window moves that are exact translations should often use standard CopyRect; other changes safely fall back to JPEG21 and later progressive lossless repair.

## Brief stale 32x32 repair tile

This remains a tracked beta limitation from the existing progressive repair scheduler and is unrelated to broker routing.

## Standalone diagnostics

The binary still supports direct-listener standalone mode for focused development:

```bash
systemctl --user stop vnc-monitor.service
sudo systemctl stop vnc-monitor-broker.service
./vnc-monitor --verbose debug
```

Do not run standalone mode while the production broker owns the same public port.

Restore production afterwards:

```bash
systemctl --user start vnc-monitor.service
sudo systemctl start vnc-monitor-broker.service
```
