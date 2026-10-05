# Hybrid VNC + WebRTC architecture

This branch adds a browser-native WebRTC transport without removing the proven VNC path.

The security model is shared by both transports. There is exactly one machine-wide display-session slot at a time, regardless of whether the client is VNC or WebRTC.

## Non-negotiable invariants

1. Only the currently active local `seat0` Wayland `Class=user` logind session is attachable.
2. A connection is bound to the exact logind Session ID + UID selected at session creation time.
3. GDM/greeter, remote sessions and inactive graphical sessions are never targets.
4. VNC and WebRTC share one broker-owned client slot. They are mutually exclusive.
5. Switch User, logout, broker loss or active-session change revokes the session immediately. Screen-lock policy is tracked separately and is not claimed by this checkpoint.
6. The display remains view-only. Browser keyboard, pointer, clipboard and file-transfer input are not accepted.
7. Authentication is PAM-backed and the authenticated Unix username must equal the active session owner.

## Target topology

```text
                              machine scope

 VNC viewer                         Browser
     |                                |
     | TCP :5901                      | HTTPS/WSS :8443
     |                                |
     +----------------+---------------+
                      |
                      v
              vnc-monitor-broker
              root system service
                      |
                      | logind seat0 policy
                      | one global client slot
                      |
            +---------+----------+
            |                    |
            | VNC                | WebRTC control/signalling
            | accepted TCP fd    | no browser fd handoff
            | SCM_RIGHTS         |
            v                    v
                   active user's
                vnc-monitor --agent
                      |
              +-------+--------+
              |                |
              | VNC backend    | WebRTC backend
              | RA2r + RFB     | GStreamer webrtcbin
              |                |
              +-------+--------+
                      |
              Mutter virtual monitor
                      |
                   PipeWire
```

## Why HTTPS terminates in the broker

The browser-facing TLS private key is machine policy material and should not be readable by every unprivileged desktop user agent.

Therefore the broker owns:

- HTTPS listener;
- login page and HTTP API;
- TLS certificate/private key;
- authenticated web session cookie/token;
- WebSocket signalling connection;
- the global single-client state.

The user agent owns:

- PAM request through the existing privileged auth helper;
- Mutter/RemoteDesktop session;
- PipeWire capture;
- `webrtcbin` and media pipeline;
- SDP/ICE processing received from the broker control channel.

The HTTPS certificate protects login and signalling. WebRTC media is independently protected by its normal DTLS-SRTP transport; it does not need to reuse the HTTPS certificate.

## Broker state machine

The broker now uses one transport-neutral state machine instead of the old `active` / `revoked` booleans:

```text
IDLE
  |
  +-- VNC TCP accepted --------------------> AUTH_VNC
  |                                             |
  |                                      FD handoff succeeds
  |                                             v
  |                                         ACTIVE_VNC
  |
  +-- Web POST /api/login -----------------> AUTH_WEB
                                                |
                                         PAM success
                                                v
                                           ACTIVE_WEBRTC
                                                |
                                                v
                                            REVOKING
                                                |
                                                v
                                              IDLE
```

`AUTH_VNC`, `AUTH_WEB`, `ACTIVE_VNC`, `ACTIVE_WEBRTC` and `REVOKING` all own the same global slot. A second VNC connection or second web authentication attempt is rejected while any of those states is occupied.

### VNC state semantics

`ACTIVE_VNC` means that the broker-owned global slot and accepted VNC socket have been handed to the selected user agent. It does **not** mean the root broker independently observed RA2/PAM success.

That distinction is deliberate. In the proven beta.3 security boundary, RA2/PAM remains entirely in the unprivileged active-user agent. The root broker selects the eligible logind session, passes the socket, keeps its duplicate for revocation and enforces the exact Session ID + UID binding. It does not duplicate VNC authentication logic.

The existing VNC handshake timeout in the agent still prevents an unauthenticated slow client from occupying the slot indefinitely.

For WebRTC, `AUTH_WEB` is a real broker-visible authentication phase because the browser login endpoint itself terminates in the broker.

## Web authentication flow

```text
GET /
    -> static login page

POST /api/login
    -> broker verifies that seat0 has an eligible active local Wayland user
    -> broker reserves the global slot as AUTH_WEB
    -> broker opens the active user's agent control socket
    -> credentials are sent only over the local broker-agent channel
    -> agent invokes the existing PAM helper as that Unix user
    -> username must equal the bound active-session owner
    -> PAM success: broker creates a 256-bit opaque one-time attach token
    -> token is returned only as Secure + HttpOnly + SameSite=Strict cookie
    -> state becomes ACTIVE_WEBRTC
    -> browser has 15 seconds to attach authenticated WSS /ws
    -> successful WSS bind consumes the token and cancels the attach timeout
```

The password is never stored by the application and is not placed in URLs, logs, cookies or WebRTC signalling. The broker forwards it once over the bound local SOCK_SEQPACKET control channel and clears request-side decoded buffers after dispatch.

The login page itself does not reserve the display slot. Only an authentication attempt does.

## Signalling

After authentication the browser opens `/ws` on the same HTTPS origin. The
broker validates the one-time HttpOnly cookie during the WebSocket handshake,
then consumes the token on the first successful bind. A second socket cannot
reuse it. Closing the authenticated WebSocket immediately revokes the bound
agent control session and releases the global slot.

The WSS transport is now authenticated and lifetime-bound; SDP/ICE forwarding
is the next checkpoint. Once enabled, the broker forwards signalling messages
over the already authenticated/bound Unix control channel:

- SDP offer;
- SDP answer;
- ICE candidates;
- browser display-size/capability messages;
- session close.

The broker does not forward media frames.

## Legacy Safari media path

Safari on iOS 9 predates browser WebRTC support, so the browser transport has a
separate compatibility path. It keeps the authenticated WSS connection and the
same broker-owned global viewer slot, but carries bounded JPEG video frames.

After the one-time WSS bind succeeds:

1. the broker sends `MEDIA_START` over the bound private
   `SOCK_SEQPACKET` control channel;
2. the active user's agent creates the normal Mutter virtual monitor and
   PipeWire capture;
3. the agent consumes the existing BGRx `FrameBridge`, encodes JPEG at a
   conservative legacy-browser rate, and sends each frame as
   `BEGIN / CHUNK* / END` control records;
4. the root broker reassembles at most one bounded frame and forwards the
   complete JPEG as one binary WSS message;
5. the ES5 client displays it through Blob/ObjectURL.

The control channel remains the authoritative lifetime guard. WSS close, seat
switch, logout, broker loss, or management disconnect stops capture and removes
the virtual monitor. Browser input remains disabled.

## WebRTC media path

For modern browsers, the agent will build a GStreamer pipeline around the existing capture source and `webrtcbin`.

Initial video target:

```text
PipeWire video
    -> video conversion as required
    -> low-latency encoder
    -> RTP payloader
    -> webrtcbin
    -> browser <video>
```

The first implementation should prefer a widely supported browser codec and low-latency settings. Encoder selection should remain replaceable so hardware encoding can be added without changing broker/session policy.

## Session revocation

For VNC the broker owns a duplicate of the client TCP fd and revokes the session with `shutdown()`.

WebRTC media uses independent ICE/DTLS sockets, so revocation cannot rely on closing the HTTPS/WSS connection alone.

The broker-agent control channel therefore becomes the authoritative lifetime guard for WebRTC. The state machine already contains the no-client-fd revocation branch: if a future WebRTC session has no external TCP descriptor, revocation shuts down the bound broker-agent control channel. The agent must then synchronously:

1. close `webrtcbin` / ICE transports;
2. stop the capture pipeline;
3. stop Mutter RemoteDesktop;
4. remove the virtual monitor;
5. clear all per-session authentication/signalling state.

The same control-channel-loss rule remains a fail-safe if the broker crashes or restarts.

## Transport discriminator and wire compatibility

The existing fixed-size broker handoff remains **wire version 1** for compatibility with beta.3 processes during upgrade.

The old 16-bit reserved field is repurposed as a transport discriminator:

```text
0 = legacy VNC handoff from beta.3
1 = VNC_BROKER_TRANSPORT_VNC
2 = VNC_BROKER_TRANSPORT_WEBRTC
```

A new agent treats both `0` and `1` as VNC. An old beta.3 agent still ignores this field and therefore continues to accept a new broker's VNC handoff. This avoids creating an avoidable broker/agent upgrade-order dependency.

VNC handoff keeps the existing accepted TCP fd via `SCM_RIGHTS`.

WebRTC handoff intentionally carries no browser fd because HTTPS/WSS terminates in the broker. Later authentication/signalling messages will require an extended broker-agent message layer, but that extension should be introduced without sacrificing legacy VNC handoff compatibility.

## Planned implementation order

1. Transport-aware broker handoff without changing existing VNC behaviour or wire compatibility. **Done.**
2. Replace broker `active` / `revoked` booleans with one transport-neutral global session state machine. **Done.**
3. Add broker HTTPS listener, TLS configuration and static login page. **Done.**
4. Add broker-agent PAM request/reply messages and the asynchronous broker-backed web authentication lifecycle. **Done.**
5. Add opaque session token, authenticated WSS binding and explicit broker `REVOKE` lifecycle. **Done.**
6. Add authenticated legacy Safari WSS/JPEG video on the existing browser slot. **Done.**
7. Add SDP/ICE message forwarding and the agent-side `webrtcbin` session skeleton for modern browsers.
8. Feed the existing Mutter/PipeWire virtual monitor into the WebRTC video pipeline.
9. Add browser display sizing/orientation negotiation.
10. Exercise VNC-vs-browser mutual exclusion and all existing Fast User Switching / GDM revocation scenarios.

## Compatibility rule

Until the WebRTC path reaches the same security and lifecycle validation level as VNC, the existing VNC behaviour remains the reference implementation and must not be weakened to accommodate the browser transport.


## Editable browser settings

The authenticated management dashboard edits the browser edge config at:

```text
/etc/vnc-monitor/web.ini
```

The page can change the HTTPS port, certificate-chain path and private-key path.
The VNC/general config at `/etc/vnc-monitor/config.ini` remains outside this
write path.

On Save, the broker validates the new port, checks that it does not collide
with the VNC listener, loads the certificate/private-key pair, and then replaces
`web.ini` using GLib's consistent durable atomic file update. Only after the
successful HTTP response does the broker exit with status 75; systemd restarts
it through the existing `Restart=on-failure` policy.

The broker systemd sandbox permits atomic replacement under
`/etc/vnc-monitor`, while explicitly retaining read-only mounts for
`/etc/vnc-monitor/config.ini` and the recommended
`/etc/vnc-monitor/tls` subtree.


## Management status versus settings lifecycle

The management page intentionally separates live state from persistent
configuration:

- `GET /api/manage/status` contains only viewer/session and active-desktop
  state and is polled every two seconds.
- `GET /api/manage/settings` reads the persistent browser-edge settings only
  when the dashboard is opened or the operator explicitly chooses
  **Reload settings**.
- `POST /api/manage/settings` validates and atomically persists a settings
  update.

Polling live session state never rewrites the settings form. Unsaved settings
therefore remain stable while viewer/seat status continues to update.


## Legacy Safari WSS Content Security Policy

The HTTPS response policy keeps `connect-src 'self'` for same-origin HTTP
requests and also emits an explicit `wss://<request-host>` source. Older
WebKit/Safari releases do not consistently treat `'self'` as matching the
equivalent WSS endpoint. The Host value is syntax-validated before it is
included, and the policy does not permit scheme-wide `wss:` access.


### Legacy browser viewer presentation

Once the first JPEG frame arrives, the login page switches to a dedicated
full-viewport viewing mode. The framebuffer is aspect-fit against the Safari
viewport and the authentication card collapses to a small top-right overlay
containing only connection status and Disconnect. Returning to a disconnected
state restores the normal login layout.

Broker shutdown is also state-aware: an already-closing libsoup WebSocket is
not closed a second time during server teardown.


### iOS 9 JPEG decode compatibility

Legacy Safari receives WebSocket binary messages as ArrayBuffer where possible and always normalizes each frame through a Blob explicitly typed image/jpeg. The client pre-decodes a candidate frame with an off-screen Image before replacing the visible framebuffer. This avoids typeless WebSocket Blob objects and revoking the previous ObjectURL while Safari is still decoding it. At most one candidate frame is decoded at a time; additional frames are dropped until that decode completes. The UI enters live-view mode only after Image.onload, and a failed first decode is reported explicitly in both status and Web Inspector console.


## Experimental iOS 9 HLS/H.264 path

The legacy iOS path now tries an HLS/H.264 transport before WSS/JPEG. After the
authenticated WSS bind and MEDIA_START, the user agent keeps the existing
Mutter/PipeWire raw capture but feeds the latest BGRx frame into a GStreamer
appsrc at 15 fps. The test encoder is x264 baseline, ultrafast/zerolatency,
2500 kbit/s, one-second GOPs. hlssink2 writes a three-segment live MPEG-TS
playlist under $XDG_RUNTIME_DIR/vnc-monitor/hls.

This first experiment intentionally uses software H.264 encoding to isolate the
browser transport and the iOS native HLS decoder. Once latency/compatibility is
measured on the iPad, the encoder can be replaced with VA/QSV without changing
the browser protocol. If the required GStreamer HLS/x264 plugins are missing,
the agent falls back to the existing bounded WSS/JPEG path.


### Broker-served authenticated HLS

The HLS files are not a public static directory. The root broker exposes only
/live/index.m3u8 and strictly named /live/segmentNNNNN.ts paths over the
existing HTTPS listener. Every request must carry the same Secure HttpOnly
viewer cookie and that token must still belong to the currently attached WSS
viewer. WSS replay remains blocked by the one-attached-WebSocket invariant;
teardown invalidates the token and clears the HLS root.

The browser waits for HLS_READY from the user agent before assigning the
playlist to a native video element. iOS 9 may still require a tap on Play
because its autoplay policy predates modern muted autoplay.


### First live HLS latency measurement

The first successful iPad 3 / iOS 9.3.6 run produced native H.264/HLS playback with roughly 1-2 seconds of end-to-end delay. The generated playlist also showed alternating normal one-second fragments and one-frame (~0.067 s) fragments. That pattern came from combining x264's fixed one-second GOP with hlssink2's own keyframe requests.

The test profile now disables hlssink2 keyframe requests and relies on the encoder's regular key-int-max=fps cadence. The HLS target remains one second, which is the smallest non-zero integer target exposed by hlssink2. This should make the MPEG-TS fragments regular and gives a cleaner latency baseline before considering a custom sub-second segmenter.
