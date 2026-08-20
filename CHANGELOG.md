# Changelog

All notable project milestones are summarized here. Development commits before the beta checkpoint were intentionally experimental and are condensed by behavior rather than reproduced commit-by-commit.

## 0.1.0-beta.3 — 2026-08-20

System-broker / per-user-agent security architecture.

### Added

- `vnc-monitor-broker`, a small root system service that owns the machine-wide public VNC listener;
- logind `seat0.ActiveSession` routing: only the current active local `Class=user`, `Type=wayland`, `Remote=false` session is attachable;
- exact VNC connection binding to logind **Session ID + UID**, not merely a Unix username;
- Unix `SOCK_SEQPACKET` broker/agent control channel under `$XDG_RUNTIME_DIR/vnc-monitor/agent.sock`;
- zero-copy TCP ownership handoff from broker to agent through `SCM_RIGHTS`;
- broker-held duplicate TCP descriptor used solely as a policy-revocation handle;
- agent control guard: broker death/restart closes an already handed-off VNC session rather than allowing it to continue without active-session policy enforcement;
- package-level global `graphical-session.target` Wants link so future graphical logins automatically start their user agent;
- `RuntimeDirectory=vnc-monitor` for the hardened user service;
- broker-aware `.deb` verification for binaries, units, Wants link, conffiles, private-key absence and runtime dependencies.

### Security policy

- GDM/greeter is deliberately not served;
- Fast User Switching never transfers an existing VNC connection to another login session;
- logoff or any change away from the bound logind Session ID permanently disconnects the VNC client and tears down its virtual monitor;
- returning to the same Unix user still requires a new VNC connection and new RA2/PAM authentication;
- while Bob is active, an inactive `gig` session is not selectable even if its user manager remains alive;
- after broker routing, the existing PAM helper independently requires the RA2 username to equal the Unix account owning the active user agent (`SO_PEERCRED`);
- source and package installs now use a generic local PAM socket because multi-user agents must reach it; the privileged helper's same-account `SO_PEERCRED` check remains the authorization boundary.

### Changed

- production `vnc-monitor.service` now runs `/usr/bin/vnc-monitor --agent` (or the source-install equivalent) and no longer binds the public TCP port;
- the public `[network] port` is machine-wide broker policy read only from `/etc/vnc-monitor/config.ini` in broker mode;
- per-user config still controls the active user's capture/display/transport/RA2 settings but cannot move the machine-wide public listener;
- system broker keeps strong single-connect ownership and immediately resets additional clients;
- beta.2 TCP keepalive, `TCP_USER_TIMEOUT` and monotonic handshake deadline remain in the handed-off user-agent socket path;
- source upgrade order restarts beta.2 standalone as `--agent` before starting the broker, preventing a deterministic TCP/5901 collision;
- package broker retries are not permanently start-limited during beta.2 -> beta.3 handoff, and package configuration does not fail merely because the already-running beta.2 user daemon still temporarily owns the public port;
- standalone direct-listener mode remains available only for development/foreground diagnostics.

### Debian packaging

- executable `build-deb.sh` builds with `make -j"$(nproc)"` by default and performs compiler/library/`dpkg` preflight;
- runtime shared-library dependencies are generated from all compiled ELF files through `dpkg-shlibdeps`;
- no compiler, `pkg-config`, `dpkg-dev`, `debhelper` or `*-dev` package is propagated into binary `Depends`;
- package layout uses `/usr/bin`, `/usr/libexec`, `/usr/lib/systemd/system` and `/usr/lib/systemd/user`;
- `/etc/vnc-monitor/config.ini` is a package conffile and system baseline;
- effective agent config precedence remains `built-ins < /etc/vnc-monitor/config.ini < user/--config < CLI`;
- package installation keeps RA2 identity/layout/user overrides in the user's home rather than creating home files from root maintainer scripts;
- package version conversion uses Debian prerelease ordering, e.g. `0.1.0-beta.3` -> `0.1.0~beta.3-1`.

## 0.1.0-beta.2 — 2026-08-20

Configuration, dynamic-display and single-session hardening release.

### Added

- persistent strict INI configuration at `~/.config/vnc-monitor/config.ini`;
- repository config template `config/vnc-monitor.conf`;
- precedence `built-in defaults < config < CLI`;
- `--config FILE` and `--show-config`;
- owned storage for configurable paths/addresses rather than pointers into parser-temporary data;
- `[display] mode=auto|fixed`;
- client-driven RFB resize through LibVNCServer `setDesktopSizeHook` for viewers supporting `ExtendedDesktopSize` / `SetDesktopSize`;
- transactional Mutter/PipeWire + FrameBridge + adaptive-transport + RFB framebuffer resize;
- CopyRect reference reallocation/invalidation after resize while preserving negotiated JPEG21 capability;
- dimension-specific layout-cache refresh after a successful resize;
- effective-config output at the end of `install.sh`;
- explicit **strong single-connect** session ownership: one active viewer owns the virtual monitor and additional connections are immediately reset/rejected;
- external-client TCP liveness protection using `SO_KEEPALIVE`, `TCP_KEEPIDLE`, `TCP_KEEPINTVL`, `TCP_KEEPCNT` and `TCP_USER_TIMEOUT` when available;
- configurable dead-peer timing under `[network]` with defaults `15s / 5s / 3 probes / 20000ms`;
- `client-handshake-timeout-ms` (default 60000) as one monotonic deadline for the bounded unauthenticated RA2/auth-helper `io_*` phase, preventing silent or trickle-slow clients from monopolizing the single slot;
- thread-local monotonic `io_*` deadline support that participates directly in the wrapper's `poll()` waits.

### Changed

- `display.width` / `display.height` are initial/fallback dimensions in `auto` mode and exact dimensions in `fixed` mode;
- the internal LibVNCServer backend is now session-scoped: loopback TCP/5903 exists only after RA2r/PAM authentication and is removed on disconnect;
- client-requested size changes are session-local and do not mutate the persistent fallback used by the next viewer;
- the public listener remains responsive during the active session solely to reject extra clients immediately; this does not enable multi-client streaming;
- a lost/half-open active TCP peer now tears itself down through kernel liveness detection instead of potentially holding the only session slot indefinitely;
- the single-client slot is reserved from `accept()`, so a second client is rejected even while the first is negotiating authentication;
- the handshake I/O deadline is cleared after successful authentication, so a healthy long-lived/static VNC session has no application-idle timeout;
- daemon shutdown waits for the only active client worker to finish before shared framebuffer/statistics state is destroyed;
- systemd user service starts the daemon with the persistent config instead of hard-coding log level arguments;
- installer creates the default config once and preserves local edits on later upgrades;
- installer continues to use `nproc` parallelism and dependency preflight before build/install.

### RFB sizing semantics

- clients advertising `ExtendedDesktopSize` can request a single-screen framebuffer size in `auto` mode;
- `fixed` mode rejects `SetDesktopSize` with `ResizeProhibited`;
- clients supporting only `NewFBSize` can receive size changes but cannot tell the server what size they prefer, so they use the configured fallback dimensions.

## 0.1.0-beta.1 — 2026-08-20

First beta/stabilization release.

### Productionized

- current GNOME Wayland session virtual monitor using Mutter RemoteDesktop + ScreenCast `RecordVirtual`;
- lazy virtual-monitor lifecycle: create after authenticated viewer connection, remove on disconnect;
- native PipeWire capture as default backend;
- GStreamer capture retained as fallback;
- PipeWire `SPA_META_Cursor` cursor path;
- RFB 3.8 RA2r frontend with persistent 2048-bit RSA identity;
- AES-128-EAX encrypted RFB relay;
- PAM username/password authentication through privileged socket helper;
- strict view-only security invariant;
- latest-only/backpressure publisher;
- JPEG21 Q30 active transport;
- 32x32 progressive lossless ZRLE repair;
- CopyRect acceleration with byte-exact translation verification and JPEG residual fallback;
- systemd user service for unattended GNOME-session startup;
- single `--verbose` logging hierarchy: error/info/debug/trace;
- automatic Makefile header dependency tracking (`-MMD -MP`);
- organized installation, architecture, troubleshooting and security documentation.

### Removed from the beta production surface

- synthetic test-pattern source;
- benchmark/square-sweep subsystem;
- old independent frame/capture/latency trace switches;
- obsolete periodic/drop keyframe diagnostic policy;
- old hardware-cursor diagnostic switch;
- temporary split CopyRect experiment wrapper/chunks;
- old `vnc-monitor-test` production binary name.

### Known beta issues

- full-screen high-motion content remains limited by the old VNC viewer's JPEG/display/request path;
- an earlier stale 32x32 progressive-repair tile race remains under observation;
- only one external viewer is served at a time;
- Git history still requires secret-history purge and RA2 identity rotation before any public release.

## 0.0.25 — adaptive JPEG / CopyRect development line

- added client-advertised JPEG encoding 21 extension;
- moved active updates to JPEG Q30;
- added progressive exact ZRLE repair;
- eliminated small live ZRLE updates stealing framebuffer requests from pending JPEG;
- confirmed target viewer advertises standard CopyRect encoding 1;
- implemented CopyRect + JPEG residual experimental transport;
- target testing showed a substantial subjective GUI-motion improvement and typical CopyRect coverage around 90% for large moved windows.

## 0.0.24 — backpressure/latest-only stabilization

- stopped consuming FrameBridge while transport was backpressured;
- resumed at low-water from newest source state;
- removed false full-frame repair/resync cycle caused by dropped intermediate frames.

## 0.0.23 — cursor metadata

- switched production cursor handling to PipeWire `SPA_META_Cursor`;
- supported cursor-only PipeWire buffers with empty video payload;
- composited cached premultiplied RGBA cursor over the exact server base framebuffer.

## 0.0.22 — PipeWire empty/corrupted buffer handling

- stopped copying `chunk->size == 0` and `SPA_CHUNK_FLAG_CORRUPTED` video payloads;
- preserved independent cursor metadata processing.

## 0.0.20 — native PipeWire capture

- native libpipewire became the default capture implementation;
- GStreamer retained for comparison/fallback;
- PipeWire buffers are copied and immediately recycled rather than retained across callbacks.

## 0.0.19 and earlier — architecture bring-up

- established Mutter virtual monitor in the current GNOME Wayland session;
- established old-client RA2r interoperability;
- added privileged PAM authentication helper/socket;
- removed obsolete forced-redraw/HW-cursor diagnostic architecture from the required runtime path.
