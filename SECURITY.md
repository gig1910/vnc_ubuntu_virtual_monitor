# Security

## Beta.3 security posture

`0.1.0-beta.3` is intentionally a **display-only** service with a machine-wide broker and per-user session agents.

The following are hard runtime invariants rather than user-selectable defaults:

- keyboard input is discarded;
- pointer/touch input is discarded;
- client clipboard input is disabled;
- file transfer is disabled.

The external listener uses RFB 3.8 with RA2r (security type 13). The internal LibVNCServer endpoint uses security type None but is bound only to `127.0.0.1` and exists only inside an authenticated session.

## Privilege separation

Production has three security domains:

```text
vnc-monitor-broker.service        root/system
vnc-monitor --agent               unprivileged graphical user
vnc-monitor-auth@.service         root PAM helper
```

The broker owns only the public TCP listener and active-session policy. It does **not** process RA2 passwords, PipeWire frames or Mutter state.

The user agent performs RA2, PAM requests, Mutter RemoteDesktop/ScreenCast, PipeWire capture and RFB transport inside the active user's graphical session.

Do not move the complete capture/RFB daemon into a root system service.

## Active-session boundary

The broker routes only to the current `seat0.ActiveSession` when that logind session is:

```text
Active=true
Remote=false
Class=user
Type=wayland
```

GDM/greeter is not served.

Every accepted VNC connection is bound to the exact logind **Session ID + UID** selected at handoff. If seat0 leaves that Session ID, including Switch User, lock-to-greeter or logoff, the broker shuts down the connection. The connection is never transferred to another user's desktop and is not restored when the original user becomes active again.

A new connection and new RA2/PAM authentication are required after every such transition.

## Broker -> agent IPC authentication

The agent socket is:

```text
$XDG_RUNTIME_DIR/vnc-monitor/agent.sock
```

The broker verifies that the socket peer UID equals the UID of the active logind session before handing off the external TCP descriptor with `SCM_RIGHTS`.

The agent independently verifies that the control peer is the system broker.

A hardened systemd user service may run in a user namespace that maps only the desktop user's UID. In that case host UID 0 is exposed through `SO_PEERCRED` as an overflow/unmapped UID rather than literal `0`.

Therefore the agent must **not** trust overflow UID by itself. Namespace-mapped root is accepted only when the immutable `SO_PEERCRED` PID belongs exactly to:

```text
/system.slice/vnc-monitor-broker.service
```

Direct `SO_PEERCRED.uid == 0` remains the normal fast path where host root is directly visible. Any other peer is rejected.

The broker-agent control channel stays open for the complete VNC session. If the broker dies or restarts, the agent treats control-channel EOF as policy revocation and shuts down the handed-off VNC connection.

## PAM authentication boundary

Package paths:

```text
/usr/libexec/vnc-monitor-auth-helper
/etc/pam.d/vnc-monitor
/usr/lib/systemd/system/vnc-monitor-auth.socket
/usr/lib/systemd/system/vnc-monitor-auth@.service
/run/vnc-monitor-auth.sock
```

Source installations use `/usr/local/libexec` and `/etc/systemd/system` equivalents.

The PAM socket is intentionally reachable by local user agents in multi-user mode. `SocketMode=0666` is **not** the authorization boundary.

The privileged helper obtains `SO_PEERCRED` from the connecting agent and requires:

```text
requested RA2/PAM username == Unix account owning the agent process
```

Thus an agent running as `guest` cannot authenticate or probe credentials for `gig`, even if `gig` has an inactive graphical session still resident after Fast User Switching.

## RA2 server identity

Per-user server identity:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

Required permissions:

```text
directory: 0700
private key: 0600
```

Deleting or rotating the key changes server identity and may require client-side re-acceptance.

The binary `.deb` verifier explicitly rejects packages containing `*.pem`; private RA2 identities are never shipped in the package.

## Historical private-key exposure

A development RA2 private key was committed in earlier Git history. Removing it from the current tree and ignoring `*.pem` does **not** remove that object from historical refs.

That historical key must be treated as compromised if the repository or its history has been publicly accessible.

Required remediation before relying on a clean public history:

1. purge the historical private-key object from every published/reachable Git ref;
2. verify the sensitive blob/path is no longer reachable from rewritten history;
3. rotate every RA2 server identity derived from or equal to the exposed development key;
4. re-accept/verify the new identity on the target viewer;
5. never reuse the exposed development key.

History rewriting changes commit IDs and should be performed as a deliberate repository-maintenance operation, not mixed casually into transport/runtime commits.

## Network exposure

Default externally reachable service:

```text
TCP/5901 -> vnc-monitor-broker
```

Default session-local backend:

```text
127.0.0.1:5903
```

TCP/5903 exists only after successful RA2/PAM authentication and is removed on disconnect.

Host firewall policy should restrict TCP/5901 to trusted networks/hosts where practical. Do not expose TCP/5903 outside loopback.

## Service hardening

The user-agent unit uses hardening including:

- `NoNewPrivileges=yes`;
- `PrivateTmp=yes`;
- `ProtectSystem=strict`;
- `ProtectHome=read-only` with explicit state write paths;
- a private runtime directory for the broker control socket.

The broker uses a separate hardened system unit. It cannot access `/home` or `/root`, while `/run/user` is visible read-only so it can connect to the selected user's agent socket.

Sandboxing must preserve the minimum required access to session D-Bus/PipeWire for the agent and system D-Bus/logind plus `/run/user` agent sockets for the broker.

## Obsolete diagnostic components

Old redraw/HW-cursor diagnostic helpers/extensions are not part of the beta runtime. Use:

```bash
make cleanup-obsolete-support
```

to remove remnants from earlier installations while keeping the production broker, user agent and PAM components.

Do not restore the historical `MUTTER_DEBUG_DISABLE_HW_CURSORS=1` workaround or obsolete redraw helper as a production requirement.

## Reporting a security problem

Do not publish credentials, private keys, authentication traces containing secrets, or other sensitive host data in an issue. Keep reproduction logs minimally scoped and redact secrets before sharing.
