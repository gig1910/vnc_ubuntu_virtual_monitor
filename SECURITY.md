# Security

## Beta security posture

`0.1.0-beta.1` is intentionally a display-only service.

The following are hard runtime invariants rather than user-selectable defaults:

- keyboard input is discarded;
- pointer/touch input is discarded;
- client clipboard input is disabled;
- file transfer is disabled.

The public listener uses RFB 3.8 with RA2r (security type 13). The internal LibVNCServer endpoint uses security type None but is bound only to `127.0.0.1` and is reached through the authenticated/encrypted frontend.

## Authentication boundary

The main daemon is unprivileged and runs as a systemd user service in the active GNOME session.

PAM authentication is delegated to:

```text
/usr/local/libexec/vnc-monitor-auth-helper
/etc/pam.d/vnc-monitor
/etc/systemd/system/vnc-monitor-auth.socket
/etc/systemd/system/vnc-monitor-auth@.service
/run/vnc-monitor-auth.sock
```

The privileged helper should remain narrowly scoped to authentication. Do not move the entire VNC daemon into a root system service.

## RA2 server identity

Current beta path:

```text
~/.config/vnc-monitor/ra2-server-key.pem
```

Required permissions:

```text
directory: 0700
private key: 0600
```

The key identifies the server to the legacy client. Deleting or rotating it changes server identity and may require client-side re-acceptance.

## Mandatory work before any public release

A development RA2 private key was committed in earlier Git history. Removing it from the current working tree and adding `*.pem` to `.gitignore` is **not sufficient** for a public release.

Before changing repository visibility or publishing an archive derived from Git history:

1. purge the historical private-key object from every reachable Git ref/history that will be published;
2. verify the sensitive blob/path is no longer reachable from the rewritten repository;
3. rotate/generate a new RA2 server identity;
4. re-accept/verify that new identity on the target viewer;
5. do not reuse the exposed development key anywhere else.

History rewriting is intentionally deferred until the beta runtime checkpoint is confirmed, because it changes commit IDs and should not be mixed with transport stabilization.

## Network exposure

Default externally reachable service:

```text
TCP/5901
```

Default local-only backend:

```text
127.0.0.1:5903
```

Host firewall policy should restrict TCP/5901 to trusted networks/hosts where practical.

Do not expose TCP/5903 outside loopback.

## Service hardening

The systemd user unit enables:

- `NoNewPrivileges=yes`;
- `PrivateTmp=yes`;
- `ProtectSystem=strict`;
- `ProtectHome=read-only` with explicit write exceptions for VNC Monitor state.

The service still requires access to session D-Bus and PipeWire runtime sockets; sandboxing must not isolate those away.

## Obsolete diagnostic components

Old redraw/HW-cursor diagnostic helpers/extensions are not part of the beta runtime. Use:

```bash
make cleanup-obsolete-support
```

to remove remnants from earlier installations while keeping the current PAM authentication components.

Do not restore the historical `MUTTER_DEBUG_DISABLE_HW_CURSORS=1` workaround or obsolete redraw helper as a production requirement.

## Reporting a security problem

Until a dedicated disclosure channel is defined, do not publish credentials, private keys, authentication traces containing secrets, or other sensitive host data in a public issue. Keep reproduction logs minimally scoped and redact secrets before sharing.
