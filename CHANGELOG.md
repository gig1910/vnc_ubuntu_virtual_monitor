# Changelog

All notable project milestones are summarized here. Development commits before the beta checkpoint were intentionally experimental and are condensed by behavior rather than reproduced commit-by-commit.

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
