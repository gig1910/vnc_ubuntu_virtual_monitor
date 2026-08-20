# Hybrid VNC + WebRTC architecture

This branch adds a browser-native WebRTC transport without removing the proven VNC path.

The security model is shared by both transports. There is exactly one machine-wide authenticated display session at a time, regardless of whether the client is VNC or WebRTC.

## Non-negotiable invariants

1. Only the currently active local `seat0` Wayland `Class=user` logind session is attachable.
2. A connection is bound to the exact logind Session ID + UID selected at session creation time.
3. GDM/greeter, remote sessions and inactive graphical sessions are never targets.
4. VNC and WebRTC share one broker-owned client slot. They are mutually exclusive.
5. Lock, Switch User, logout, broker loss or active-session change revokes the session immediately.
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

The current `active` boolean will evolve into an explicit state machine:

```text
IDLE
  |
  +-- VNC TCP accepted --------------------> AUTH_VNC
  |                                             |
  |                                      RA2/PAM success
  |                                             v
  |                                         ACTIVE_VNC
  |
  +-- Web POST /api/login -----------------> AUTH_WEB
                                                |
                                         PAM success
                                                v
                                           ACTIVE_WEBRTC
```

`AUTH_VNC`, `AUTH_WEB`, `ACTIVE_VNC` and `ACTIVE_WEBRTC` all own the same global slot. A second VNC connection or second web login attempt is rejected while any of those states is occupied.

Authentication phases must have finite monotonic deadlines so a silent or trickle-slow client cannot monopolize the only slot indefinitely.

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
    -> PAM success: broker issues a short-lived random web session token
    -> state becomes ACTIVE_WEBRTC
```

The password is never stored and is not placed in URLs, logs, cookies or WebRTC signalling.

The login page itself does not reserve the display slot. Only an authentication attempt does.

## Signalling

After authentication the browser opens a WSS endpoint using the authenticated web session.

The broker forwards signalling messages over the already authenticated/bound Unix control channel:

- SDP offer;
- SDP answer;
- ICE candidates;
- browser display-size/capability messages;
- session close.

The broker does not forward media frames.

## WebRTC media path

The agent builds a GStreamer pipeline around the existing capture source and `webrtcbin`.

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

For VNC the broker currently owns a duplicate of the client TCP fd and can revoke the session with `shutdown()`.

WebRTC media uses independent ICE/DTLS sockets, so revocation cannot rely on closing the HTTPS/WSS connection alone.

Protocol v2 therefore treats the broker-agent control channel as the authoritative lifetime guard. On any policy revocation the broker must explicitly terminate the control session; the agent must then synchronously:

1. close `webrtcbin` / ICE transports;
2. stop the capture pipeline;
3. stop Mutter RemoteDesktop;
4. remove the virtual monitor;
5. clear all per-session authentication/signalling state.

The same control-channel-loss rule should also remain a fail-safe if the broker crashes or restarts.

## Broker protocol v2

Protocol v2 introduces an explicit transport discriminator:

```text
VNC_BROKER_TRANSPORT_VNC
VNC_BROKER_TRANSPORT_WEBRTC
```

VNC handoff keeps the existing accepted TCP fd via `SCM_RIGHTS`.

WebRTC handoff intentionally carries no browser fd because HTTPS/WSS terminates in the broker. Subsequent WebRTC authentication/signalling messages will extend the broker-agent control protocol rather than tunnelling browser TLS through the user agent.

## Planned implementation order

1. Transport-aware broker protocol without changing existing VNC behaviour. **Started in this branch.**
2. Replace broker `active` boolean with one transport-neutral global session state machine.
3. Add broker HTTPS listener, TLS configuration and static login page.
4. Add broker-agent PAM request/reply messages for web authentication.
5. Add authenticated WSS signalling and explicit broker `REVOKE` control message.
6. Add agent-side `webrtcbin` session skeleton and SDP/ICE exchange.
7. Feed the existing Mutter/PipeWire virtual monitor into the WebRTC video pipeline.
8. Add browser display sizing/orientation negotiation.
9. Exercise VNC-vs-WebRTC mutual exclusion and all existing Fast User Switching / GDM revocation scenarios.

## Compatibility rule

Until the WebRTC path reaches the same security and lifecycle validation level as VNC, the existing VNC behaviour remains the reference implementation and must not be weakened to accommodate the browser transport.
