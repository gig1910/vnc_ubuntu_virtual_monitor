# VNC Monitor Server V20

**V20:** native libpipewire capture is now the default for Mutter RecordVirtual.
The capture callback drains to the newest buffer, copies BGRx into private memory,
recycles the `pw_buffer` immediately, and only then publishes to FrameBridge.
Use `--capture-backend gstreamer` for the V19 comparison path. See `V20_TEST.md`.

# VNC Monitor modular RA2r test

## V19 PipeWire buffer ownership fix

V19 fixes the RecordVirtual/PipeWire buffer-starvation path by detaching/copying producer buffers inside `pipewiresrc` before downstream processing. The obsolete forced-redraw GNOME Shell helper has been removed. See `V19_TEST.md`.


Current test architecture:

```
iPad VNC client
    |
    | RFB 3.8 + RA2r / AES-128-EAX
    | Ubuntu username/password
    v
RA2r front-end :5901
    |
    | local auth request
    +----> /run/vnc-monitor-auth.sock
    |          root PAM helper
    |
    | plain RFB on loopback only
    v
LibVNCServer backend 127.0.0.1:5903
    |
    +--> 1024x768 dynamic test framebuffer, max 60 FPS
```

## V16 source-driven publisher / shutdown hardening

V16 keeps the V15 `GstSample` stall diagnostics but changes the real-monitor
VNC publisher to **source-driven** operation. The default `--vnc-fps source`
means that diff/VNC work only happens for a genuinely new FrameBridge sequence;
there is no periodic reprocessing of an unchanged framebuffer. Numeric
`--vnc-fps N` values are now only ceilings for new source frames.

V16 added `--mutter-hardware-cursor auto|enabled|disabled` and verifies the actual startup
environment of the running `gnome-shell`. V19 restores its default to `auto`; the old
`MUTTER_DEBUG_DISABLE_HW_CURSORS=1` workaround is no longer part of the normal setup.
Ctrl+C retains the socket shutdown supervisor and second-Ctrl+C emergency `_exit`.
See `V16_TEST.md`.

## V15 capture-stall diagnostics

V15 added instrumentation at the incoming `GstSample` boundary. Use
`--capture-trace on` for per-sample timing and `--capture-stall-ms N` to choose
the stall threshold (default 500 ms). The server also prints the matching
Mutter DisplayConfig mode/refresh and a capture interval histogram at shutdown.
See `V15_TEST.md`.


## Modules

- `src/ra2.c` — RA2r handshake, EAX record layer and re-key.
- `src/auth_client.c` — client of the privileged PAM helper.
- `src/rfb_proxy.c` — encrypted RA2r ↔ local plain-RFB relay.
- `src/rfb_backend.c` — loopback-only LibVNCServer backend.
- `src/test_pattern.c` — synthetic 1024×768 60 FPS test pattern.
- `src/io.c` — basic exact socket I/O.
- `auth-helper/` — already-tested root PAM helper and systemd socket units.

The LibVNCServer backend remains strict server-side view-only:
keyboard, pointer/touch and client clipboard callbacks are no-op, file transfer is disabled.

## Build

```bash
make -j20
```

Expected dependencies are the same ones already tested:

- libvncserver-dev
- OpenSSL development files
- Nettle development files
- pthreads

The PAM helper is separate and is not linked into the VNC process.

## PAM/systemd authentication helper

Build/render it for the current user without changing the system:

```bash
make pam-service
```

Install the helper, `/etc/pam.d/vnc-monitor`, and the systemd socket/service, then enable the socket:

```bash
make install-pam-service
```

The target invokes `sudo` only where root access is needed. Do not run the whole Make process with `sudo`, because the generated socket owner is derived from the invoking user. Override explicitly if necessary with `AUTH_SOCKET_USER=... AUTH_SOCKET_GROUP=...`.

Status:

```bash
make pam-service-status
```

Install the runtime support component (PAM authentication):

```bash
make install-support
```

V19 has no redraw-helper support component.

## Run

The PAM socket/helper should be active:

```bash
make pam-service-status
```

Then:

```bash
./vnc-monitor-test
```

Connect the iPad to the laptop's LAN IP on port `5901`.

Expected log milestones:

```
Internal LibVNCServer backend: 127.0.0.1:5903
RA2r VNC front-end listening on TCP port 5901
External client connected: ...
Security type selected: 13 (RA2r)
RA2r credentials received for user "gig"
RA2r + auth-helper handshake complete for "gig"
Encrypted ClientInit received (shared=1)
RA2r <-> LibVNCServer stream relay started
```

At that point the iPad should show the same dynamic test picture, but the externally visible RFB session is RA2r-encrypted and authenticated using the Ubuntu account.

## Security boundary

The backend is bound to `127.0.0.1` only and uses RFB security type `None` internally. It is intentionally not reachable from the LAN.

The privileged helper checks `SO_PEERCRED` and only permits a local process to authenticate the Unix username that owns that process.

## Next step

Once this test is confirmed on the iPad, replace only `test_pattern` / framebuffer production with the Mutter + PipeWire + GStreamer appsink source. The RA2r, PAM helper, proxy and LibVNCServer view-only layers remain separate modules.


## v2: persistent RA2 identity + explicit colour format

This revision fixes two observations from the first integrated test.

### Stable RA2 server signature

The first version generated a new RSA server key for every TCP connection.
That makes an RA2-capable VNC Viewer see a different server signature on
reconnect.

The key is now generated only once and stored in the current directory:

```text
./ra2-server-key.pem
```

The file is created with mode `0600` and loaded on subsequent connections.
Do not delete it unless you intentionally want to change the server identity.

The first connection after upgrading to this revision may still warn because
the previous test used an ephemeral key. After the new key is accepted once,
future reconnects should see the same signature.

### Colour diagnostic

The LibVNCServer source framebuffer format is now explicitly fixed to:

```text
32 bpp, depth 24, little endian
R shift 16, G shift 8, B shift 0
```

The synthetic test image is intentionally high-saturation RGB bars plus a
moving coloured grid square. This makes any colour-format problem visually
obvious.

The old iPad client may still temporarily request:

```text
8 bpp, depth 6
```

and later switch to:

```text
32 bpp, depth 24
```

LibVNCServer is expected to translate both from the fixed 32-bit server
framebuffer format.


## v3: old-client RA2 stream compatibility diagnostic

The persistent server identity from v2 is retained.

The remaining reconnect loop occurs only after the encrypted RFB data stream
has started, so v3 narrows the test to the RA2 transport layer.

For normal post-auth RFB traffic, the proxy now limits each RA2 encrypted
record payload to 4096 bytes:

```text
RA2_STREAM_RECORD_MAX = 4096
```

The RFB/RA2 specification allows arbitrary independent RA2 record boundaries,
but this old iPad VNC client may have a smaller practical encrypted-record
buffer than the protocol's U16 maximum. 4096 is deliberately conservative for
this compatibility test.

The relay also logs the exact direction/reason when it stops and record/byte
counters. If the client still reconnects, preserve the final lines such as:

```text
RA2 relay stopped while receiving from client ...
EAX authentication tag mismatch ...
RA2 relay stopped while sending to client ...
LibVNCServer backend closed the relay ...
```

If 4096-byte records make the connection stable, we can later benchmark
8192/16384/32768 and keep the largest stable value.


## v4: runtime configuration

Most test/runtime parameters are now command-line options. Recompilation is
not required to change transport sizes or compatibility switches.

Show all options:

```bash
./vnc-monitor-test --help
```

Show resolved defaults:

```bash
./vnc-monitor-test --show-config
```

Useful transport tests:

```bash
./vnc-monitor-test --ra2-record-size 1024
./vnc-monitor-test --ra2-record-size 2048
./vnc-monitor-test --ra2-record-size 8192
```

Useful RFB compatibility tests:

```bash
./vnc-monitor-test --zrle off --raw on
./vnc-monitor-test --cursor off
./vnc-monitor-test --newfbsize off
./vnc-monitor-test --zrle off --raw on --cursor off --newfbsize off
```

Framebuffer/runtime tests:

```bash
./vnc-monitor-test --fps 30
./vnc-monitor-test --width 1024 --height 768 --fps 60
```

Security-related defaults remain conservative:

```text
view-only=on
clipboard=off
file-transfer=off
backend-bind=127.0.0.1
```

They are exposed as switches for controlled testing, but the defaults should
remain the production defaults.


## v5: reliable Ctrl-C / SIGTERM shutdown

The previous versions used a process flag while the main thread could remain
blocked in `accept()` and the active proxy could remain blocked in `select()`.
In a multi-threaded process this was not a reliable shutdown mechanism.

v5 adds a dedicated `shutdown_signal` module using the self-pipe pattern:

```text
SIGINT / SIGTERM
      |
      v
async-signal-safe write()
      |
      v
self-pipe
      |
      +--> listener select()
      |
      +--> active relay select()
```

Therefore a single Ctrl-C wakes either state immediately:

- waiting for a new VNC connection;
- relaying an active RA2r session.

Expected shutdown log:

```text
^C
Shutdown requested; stopping active RA2 relay
External client disconnected
Stopping VNC monitor test...
Stopped.
```

No repeated Ctrl-C should be necessary.


## v6: benchmark subsystem

A separate `benchmark` module now records per-interval workload, LibVNCServer
processing and RA2 transport metrics. See `BENCHMARK.md`.

Quick start:

```bash
./vnc-monitor-test \
  --ra2-record-size 32768 \
  --benchmark suite \
  --benchmark-step 5 \
  --benchmark-csv bench-32768.csv
```


## v7: RA2 stream coalescing

The benchmark remains a normal module of the main project and is disabled by
default. Enable it only with `--benchmark ...`.

The server-to-client path now uses `ra2_stream_coalescer` between the local
LibVNCServer backend and `ra2_send_record()`.

Defaults:

```text
--ra2-record-size 32768
--ra2-coalesce on
--ra2-coalesce-us 500
```

A/B tests require no rebuild:

```bash
./vnc-monitor-test \
  --ra2-record-size 32768 \
  --ra2-coalesce off
```

```bash
./vnc-monitor-test \
  --ra2-record-size 32768 \
  --ra2-coalesce on \
  --ra2-coalesce-us 0
```

```bash
./vnc-monitor-test \
  --ra2-record-size 32768 \
  --ra2-coalesce on \
  --ra2-coalesce-us 500
```

`0 us` still drains all data already queued in the loopback socket; it simply
does not add a deliberate aggregation window.

Benchmark output now includes:

```text
bread=.../s     backend reads per second
bavg=...B       average backend read size
rec=.../s       RA2 records per second
avg=...B        average RA2 record payload
coal=...x       backend-read / RA2-record coalescing ratio
```

A healthy coalescer should move `avg` from the previous ~8 bytes toward KBs
while dramatically reducing `rec`.


## v8: real GNOME Wayland virtual monitor

The default source is now the real virtual monitor in the *current* GNOME
Wayland session:

```bash
./vnc-monitor-test
```

Lifecycle:

```text
no VNC client
    -> no Mutter virtual monitor

authenticated VNC client
    -> RemoteDesktop.CreateSession
    -> ScreenCast.CreateSession(remote-desktop-session-id=...)
    -> RecordVirtual(cursor-mode=1, is-platform=true)
    -> RemoteDesktop.Session.Start
    -> PipeWireStreamAdded(node_id)
    -> resolve node_id -> object.serial
    -> GStreamer pipewiresrc
    -> BGRx 1024x768
    -> FrameBridge
    -> LibVNCServer
    -> RA2r
    -> iPad

client disconnect
    -> stop GStreamer
    -> RemoteDesktop.Session.Stop
    -> virtual monitor disappears
```

For the old synthetic source:

```bash
./vnc-monitor-test --source test
```

The current real-screen path deliberately marks a whole 1024x768 frame dirty
when a new PipeWire frame arrives. This is the simplest correctness-first
implementation. Damage propagation and zero-copy/copy reduction are deferred
until the end-to-end real monitor is validated.

Current compatibility defaults:

```text
--source mutter
--width 1024
--height 768
--ra2-record-size 16384
--ra2-coalesce on
--ra2-coalesce-us 500
```

Build dependencies are taken from pkg-config:

```text
libvncserver
openssl
nettle
glib-2.0
gio-2.0
gstreamer-1.0
gstreamer-app-1.0
gstreamer-video-1.0
libpipewire-0.3
```


## v9: bounded-latency real monitor

The v8 real-screen path proved that the GNOME Wayland extended monitor works,
but it could accumulate tens of seconds of stale encoded VNC traffic.

v9 treats this as a **frame age / queueing problem**, not a source FPS problem:

```text
Mutter/PipeWire
    -> capture as fast as available
    -> FrameBridge keeps only the newest frame
    -> VNC publishes at an independent bounded rate
    -> bounded TCP queues propagate backpressure early
```

New runtime controls:

```text
--vnc-fps 10
--external-send-buffer 65536
--backend-recv-buffer 65536
```

The important distinction is:

```text
--fps       source/backend loop ceiling
--vnc-fps   how often a fresh real frame may be offered to VNC
```

For an old client, freshness is more important than queueing 60 old frames.


## v10: tile damage + remembered GNOME layout

### Real-frame damage

The real source no longer has to mark 1024x768 as changed for every captured
frame. By default:

```text
--diff-detect on
--diff-tile-size 32
```

`frame_diff` keeps the existing VNC framebuffer as the old frame, compares
the latest PipeWire frame in 32x32 tiles, copies only changed tiles, merges
adjacent tile runs, and calls `rfbMarkRectAsModified()` only for those regions.

Disable for diagnostics:

```bash
./vnc-monitor-test --diff-detect off
```

### Monitor identity / layout

Mutter's `RecordVirtual` D-Bus method does **not** expose properties for a
custom monitor name/vendor/model/serial. The supported virtual-recording
properties are cursor mode and `is-platform`.

Therefore v10 solves the actual layout problem one layer higher:
`monitor_layout_cache` stores the GNOME `monitors.xml` configuration while the
virtual monitor exists and restores that file immediately before the next
virtual-monitor hotplug.

Default:

```text
--layout-remember on
--layout-resave off
```

First use:

1. connect the iPad;
2. arrange the 4-monitor layout once in GNOME Settings and apply it;
3. disconnect the iPad normally.

The program saves:

```text
$XDG_CONFIG_HOME/vnc-monitor-server/monitors-1024x768.xml
```

On later connections it restores that cached GNOME configuration before
creating the virtual monitor.

A conservative one-time backup of the pre-VNC GNOME file is kept as:

```text
$XDG_CONFIG_HOME/vnc-monitor-server/physical-before-vnc.xml
```

To intentionally replace an existing cached layout:

```bash
./vnc-monitor-test --layout-resave on
```

Arrange the displays, then disconnect normally.


## v11: pipeline diagnostics

The old `damage` wording was removed from the real-frame CLI/module:

```text
frame_diff.c/.h
--diff-detect on|off
--diff-tile-size 32
```

New always-available diagnostics:

```text
--frame-stats on            one pipeline summary per second
--frame-trace off           one line per VNC publish when enabled
--frame-stats-interval-ms 1000
```

Example summary:

```text
[PIPE] cap=59.9fps pub=10.0fps skip=49.9fps
       rect=5.2/frame changed=3.7%
       diff=0.42/0.78ms rfb=1.20/8.40ms
       backend=2.10Mbit/s reads=30.0/s
       RA2=2.10Mbit/s rec=30.0/s avg=8750B
       queues ext=0/16384B backend=0/32768B
       unacked=1/4 rtt=3.20/8.10ms
```

Per-publish trace:

```bash
./vnc-monitor-test --frame-trace on
```

produces:

```text
[FRAME] source_delta=6 skipped=5 rects=5 changed=28672 px (3.65%)
        diff=0.410ms rfb=1.240ms
        outq=0B backend_inq=0B unacked=1 rtt=3.20ms
```

This makes the 30-60 second delay observable stage-by-stage:
capture rate, frame skipping, diff cost/area, LibVNCServer cost, internal
backend throughput, RA2 throughput, Linux TCP send queue, backend receive
queue, unacknowledged TCP segments and RTT.


## v12: explicit capture refresh + live Mutter layout restore

### Capture negotiation

The PipeWire/GStreamer caps are now explicitly fixed to the requested virtual
monitor refresh rate:

```text
video/x-raw,format=BGRx,width=1024,height=768,framerate=60/1
```

The first received sample prints the *actual* negotiated caps:

```text
[CAPTURE] negotiated caps: video/x-raw, ...
```

This is intentionally independent from `--vnc-fps`: Mutter/PipeWire can
produce at 60 Hz while the VNC side publishes only the newest frame at a lower
rate.

### Layout

The previous `monitors.xml` copy mechanism was removed. Mutter already has its
monitor configuration in memory, so replacing the XML file at runtime is not a
reliable way to change the active layout.

v12 uses the live D-Bus API instead:

```text
org.gnome.Mutter.DisplayConfig.GetCurrentState
org.gnome.Mutter.DisplayConfig.ApplyMonitorsConfig
```

First successful run:

1. connect the iPad;
2. arrange the four displays in GNOME Settings;
3. Apply;
4. disconnect normally.

The live logical layout is serialized to:

```text
$XDG_CONFIG_HOME/vnc-monitor-server/layout-1024x768.ini
```

On later connections the virtual monitor is created first; then the saved
geometry is applied to the *current* connector/mode IDs through
`ApplyMonitorsConfig`.

Exact connector matching is preferred. If a virtual connector name changes,
the cache falls back to Mutter's vendor/product/serial monitor spec.


## v13 capture negotiation fix

v12 used an exact GStreamer `framerate=N/1` caps field. On the tested Mutter
RecordVirtual stream that can make the PipeWire pipeline fail to enter PLAYING.

v13 follows the working RecordVirtual pattern and requests an upper bound:

```text
video/x-raw,format=BGRx,width=1024,height=768,max-framerate=60/1
```

It also prints:

```text
[CAPTURE] pipeline: ...
Capture source: Mutter RecordVirtual stream=... node_id=... object.serial=...
[CAPTURE] negotiated caps: ...
```

and dumps GStreamer bus ERROR/WARNING messages if startup fails.

The capture source is the PipeWire node emitted by that specific
`RecordVirtual` stream; it is not a composite capture of all physical
monitors.

A failed capture startup no longer writes a monitor-layout cache. v13 uses
`layout-v2-WIDTHxHEIGHT.ini`, intentionally ignoring the cache accidentally
created by v12 after its failed startup.


## v14: diagnose Mutter damage + silent layout restore

### Mutter cursor mode

The ScreenCast API defines three RecordVirtual cursor modes:

```text
0 hidden
1 embedded
2 metadata
```

v13 used `embedded`. v14 makes this explicit:

```text
--mutter-cursor hidden|embedded|metadata
```

Default is now `hidden` for the first Ubuntu 26.04/Mutter damage test.

This is not the VNC cursor setting. It controls whether Mutter itself includes
the desktop cursor in the PipeWire virtual-monitor stream.

### Layout confirmation dialog

`ApplyMonitorsConfig` methods are:

```text
0 verify
1 temporary
2 persistent
```

The previous implementation used `2`, so GNOME correctly showed its
"keep this display configuration?" confirmation UI.

v14 uses method `1` instead. Our own layout cache is the persistence layer, so
the transient virtual-monitor layout can be applied immediately and silently
on every connection.
