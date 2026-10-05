#define _GNU_SOURCE

#include "web_server.h"
#include "broker_protocol.h"
#include "config.h"
#include "log.h"
#include "tls_pair.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/random.h>
#include <unistd.h>

#define WEB_DEFAULT_PORT       8443
#define WEB_DEFAULT_CERT_FILE  "/etc/vnc-monitor/tls/server.crt"
#define WEB_DEFAULT_KEY_FILE   "/etc/vnc-monitor/tls/server.key"
#define WEB_LOGIN_BODY_MAX      8192
#define WEB_WS_MESSAGE_MAX      8192
#define WEB_WS_DIAGNOSTIC_MAX       512
#define WEB_WS_DIAGNOSTIC_RATE       32
#define WEB_SESSION_COOKIE          "vnc-monitor-session"
#define WEB_SESSION_MAX_AGE_S       31536000
#define WEB_DEVICE_COOKIE           "__Host-vnc-monitor-device"
#define WEB_DEVICE_MAX_AGE_S        31536000
#define WEB_DISPLAY_MIN_INTERVAL_US (2 * G_USEC_PER_SEC)
#define WEB_MANAGEMENT_COOKIE       "vnc-monitor-management"
#define WEB_CONTROL_HEADER          "X-VNC-Monitor-Control"
#define WEB_WS_TOKEN_DATA_KEY       "vnc-monitor-ws-token"
#define WEB_MANAGEMENT_MAX_AGE_S    WEB_SESSION_MAX_AGE_S
#define WEB_SETTINGS_BODY_MAX       8192
#define WEB_HLS_PLAYLIST_MAX         (64u * 1024u)
#define WEB_HLS_SEGMENT_MAX          (8u * 1024u * 1024u)

struct WebServer {
    SoupServer *server;
    GTlsCertificate *certificate;
    SoupWebsocketConnection *websocket;
    gboolean websocket_protocol_ready;
    gint64 websocket_diagnostic_window_us;
    guint websocket_diagnostic_count;
    gint64 websocket_display_last_us;
    guint32 websocket_display_generation;
    gint64 started_us;
    guint port;
    char *config_file;
    char *certificate_file;
    char *private_key_file;
    char *hls_root;
    WebServerHooks hooks;
    gpointer user_data;
};

static const char login_page[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0,maximum-scale=1.0\">\n"
    "  <meta name=\"apple-mobile-web-app-capable\" content=\"yes\">\n"
    "  <meta name=\"apple-mobile-web-app-status-bar-style\" content=\"default\">\n"
    "  <title>VNC Monitor</title>\n"
    "  <style>\n"
    "    html { -webkit-text-size-adjust: 100%; background: #eef2f6; }\n"
    "    body { margin: 0; padding: 0; background: #eef2f6; color: #24313d; font-family: -apple-system, 'Helvetica Neue', Helvetica, Arial, sans-serif; font-size: 16px; }\n"
    "    * { -webkit-box-sizing: border-box; box-sizing: border-box; }\n"
    "    .page { padding: 42px 20px; }\n"
    "    .card { width: 100%; max-width: 520px; margin: 0 auto; overflow: hidden; background: #ffffff; border: 1px solid #d8e0e7; border-radius: 14px; -webkit-box-shadow: 0 12px 32px rgba(28,45,61,.12); box-shadow: 0 12px 32px rgba(28,45,61,.12); }\n"
    "    .head { padding: 30px 30px 24px; border-bottom: 1px solid #e7ecf0; }\n"
    "    .mark { width: 50px; height: 50px; margin-bottom: 18px; border-radius: 12px; background: #1769c2; color: #ffffff; font-size: 24px; font-weight: bold; line-height: 50px; text-align: center; }\n"
    "    h1 { margin: 0 0 7px; color: #17212b; font-size: 30px; line-height: 36px; font-weight: 600; }\n"
    "    .sub { margin: 0; color: #697784; font-size: 16px; line-height: 23px; }\n"
    "    .body { padding: 26px 30px 30px; }\n"
    "    .badge { display: inline-block; margin: 0 6px 20px 0; padding: 5px 9px; border: 1px solid #dbe5ee; border-radius: 12px; background: #f7fafc; color: #536472; font-size: 12px; line-height: 16px; }\n"
    "    .field { display: block; margin: 0 0 18px; }\n"
    "    .field span { display: block; margin: 0 0 7px; color: #344452; font-size: 14px; font-weight: 600; }\n"
    "    input { display: block; width: 100%; height: 50px; margin: 0; padding: 10px 13px; border: 1px solid #b9c5cf; border-radius: 8px; background: #ffffff; color: #17212b; font-family: inherit; font-size: 18px; line-height: 28px; outline: none; -webkit-appearance: none; }\n"
    "    input:focus { border-color: #1769c2; -webkit-box-shadow: 0 0 0 2px rgba(23,105,194,.14); box-shadow: 0 0 0 2px rgba(23,105,194,.14); }\n"
    "    input[disabled] { background: #f2f5f7; color: #73808a; }\n"
    "    button { display: block; width: 100%; height: 50px; margin: 4px 0 0; padding: 0 16px; border: 0; border-radius: 8px; background: #1769c2; color: #ffffff; font-family: inherit; font-size: 17px; font-weight: 600; line-height: 50px; text-align: center; -webkit-appearance: none; cursor: pointer; }\n"
    "    button:active { background: #12579f; }\n"
    "    button[disabled] { background: #9db8d2; color: #f4f7fa; }\n"
    "    .secondary { margin-top: 10px; border: 1px solid #b9c5cf; background: #ffffff; color: #344452; }\n"
    "    .secondary:active { background: #f0f3f5; }\n"
    "    .status { margin: 22px 0 0; padding: 13px 14px; border: 1px solid #dbe4ea; border-radius: 8px; background: #f7f9fb; color: #526270; font-size: 14px; line-height: 20px; }\n"
    "    .status-dot { display: inline-block; width: 9px; height: 9px; margin: 0 8px 1px 0; border-radius: 5px; background: #7d8b96; vertical-align: middle; }\n"
    "    .status-working { border-color: #c9dcec; background: #f2f8fd; color: #315d7d; }\n"
    "    .status-working .status-dot { background: #2b7fc3; }\n"
    "    .status-ok { border-color: #b9d9c5; background: #f1f8f3; color: #2f6843; }\n"
    "    .status-ok .status-dot { background: #3a965a; }\n"
    "    .status-error { border-color: #e4c1c1; background: #fff5f5; color: #8a4040; }\n"
    "    .status-error .status-dot { background: #c25757; }\n"
    "    .viewer { display: none; width: 100%; max-width: 1024px; margin: 18px auto 0; overflow: hidden; border: 1px solid #273542; border-radius: 10px; background: #111820; -webkit-box-shadow: 0 12px 32px rgba(28,45,61,.16); box-shadow: 0 12px 32px rgba(28,45,61,.16); }\n"
    "    .viewer img, .viewer video { display: block; width: 100%; height: auto; margin: 0; }\n"
    "    .viewer img.jpeg-frame { display: none; }\n"
    "    .viewer img.jpeg-frame-active { display: block; }\n"
    "    .viewer video { display: none; background: #000000; }\n"
    "    body.streaming { overflow: hidden; background: #000000; }\n"
    "    body.streaming .page { padding: 0; }\n"
    "    body.streaming .viewer { display: block; position: fixed; z-index: 1; left: 0; top: 0; width: 100%; height: 100%; max-width: none; margin: 0; border: 0; border-radius: 0; background: #000000; -webkit-box-shadow: none; box-shadow: none; }\n"
    "    body.streaming .viewer img, body.streaming .viewer video { position: absolute; left: 50%; top: 50%; width: auto; height: auto; max-width: 100%; max-height: 100%; -webkit-transform: translate(-50%, -50%); transform: translate(-50%, -50%); }\n"
    "    body.streaming .card { position: fixed; z-index: 2; width: 220px; margin: 0; border-radius: 8px; background: rgba(255,255,255,.94); -webkit-box-shadow: 0 4px 18px rgba(0,0,0,.22); box-shadow: 0 4px 18px rgba(0,0,0,.22); }\n"
    "    body.streaming .card.control-anchor-tl { top: 10px; left: 10px; right: auto; bottom: auto; -webkit-transform: none; transform: none; }\n"
    "    body.streaming .card.control-anchor-tc { top: 10px; left: 50%; right: auto; bottom: auto; -webkit-transform: translateX(-50%); transform: translateX(-50%); }\n"
    "    body.streaming .card.control-anchor-tr { top: 10px; left: auto; right: 10px; bottom: auto; -webkit-transform: none; transform: none; }\n"
    "    body.streaming .card.control-anchor-ml { top: 50%; left: 10px; right: auto; bottom: auto; -webkit-transform: translateY(-50%); transform: translateY(-50%); }\n"
    "    body.streaming .card.control-anchor-mr { top: 50%; left: auto; right: 10px; bottom: auto; -webkit-transform: translateY(-50%); transform: translateY(-50%); }\n"
    "    body.streaming .card.control-anchor-bl { top: auto; left: 10px; right: auto; bottom: 10px; -webkit-transform: none; transform: none; }\n"
    "    body.streaming .card.control-anchor-bc { top: auto; left: 50%; right: auto; bottom: 10px; -webkit-transform: translateX(-50%); transform: translateX(-50%); }\n"
    "    body.streaming .card.control-anchor-br { top: auto; left: auto; right: 10px; bottom: 10px; -webkit-transform: none; transform: none; }\n"
    "    body.streaming .head, body.streaming .badge, body.streaming .field, body.streaming .hint, body.streaming #connect { display: none; }\n"
    "    body.streaming .body { padding: 8px; }\n"
    "    .stream-controls { display: none; }\n"
    "    body.streaming .stream-controls { display: block; }\n"
    "    body.streaming .stream-controls button { height: 36px; min-height: 36px; margin: 0 0 7px; padding: 0 10px; line-height: 36px; font-size: 13px; }\n"
    "    .control-toggle { display: none; }\n"
    "    body.streaming .control-toggle { display: block; width: 100%; }\n"
    "    .control-position-label { margin: 1px 0 4px; color: #6d7b86; font-size: 10px; line-height: 14px; text-align: center; }\n"
    "    .anchor-picker { text-align: center; white-space: nowrap; }\n"
    "    body.streaming .anchor-picker .control-anchor-option { display: inline-block; width: 29px; min-width: 0; height: 29px; min-height: 29px; margin: 2px; padding: 0; border: 1px solid #b9c5cf; border-radius: 5px; background: #fff; color: #526270; line-height: 27px; font-size: 12px; }\n"
    "    body.streaming .anchor-picker .control-anchor-option.active { border-color: #1769c2; background: #1769c2; color: #fff; }\n"
    "    body.streaming #disconnect { display: block !important; height: 38px; margin: 0; line-height: 38px; font-size: 14px; }\n"
    "    body.streaming .status { margin: 7px 0 0; padding: 6px 8px; font-size: 11px; line-height: 15px; }\n"
    "    body.streaming .status-dot { width: 7px; height: 7px; margin-right: 6px; }\n"
    "    body.streaming .card.control-collapsed { width: 44px; height: 44px; overflow: hidden; border: 0; border-radius: 8px; background: rgba(255,255,255,.96); }\n"
    "    body.streaming .card.control-collapsed .body { padding: 0; }\n"
    "    body.streaming .card.control-collapsed form, body.streaming .card.control-collapsed .status, body.streaming .card.control-collapsed .stream-control-details { display: none !important; }\n"
    "    body.streaming .card.control-collapsed .control-toggle { display: block !important; width: 44px; height: 44px; min-height: 44px; margin: 0; padding: 0; border: 0; line-height: 44px; font-size: 22px; }\n"
    "    .hint { margin: 18px 0 0; color: #84919c; font-size: 12px; line-height: 18px; text-align: center; }\n"
    "    .protocol { margin: 10px 0 0; color: #84919c; font-size: 11px; line-height: 16px; text-align: center; }\n"
    "    .service-info { margin: 8px 0 0; color: #677783; font-size: 11px; line-height: 16px; text-align: center; }\n"
    "    body.streaming .protocol, body.streaming .service-info { display: none; }\n"
    "    @media only screen and (max-width: 600px) {\n"
    "      .page { padding: 16px 10px; }\n"
    "      .card { border-radius: 10px; }\n"
    "      .head { padding: 22px 20px 19px; }\n"
    "      .body { padding: 20px; }\n"
    "      .mark { width: 44px; height: 44px; margin-bottom: 14px; border-radius: 10px; font-size: 21px; line-height: 44px; }\n"
    "      h1 { font-size: 26px; line-height: 32px; }\n"
    "    }\n"
    "  </style>\n"
    "  <script src=\"/client.js?protocol=" VNC_WEB_PROTOCOL_VERSION_TEXT "\" defer></script>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"page\">\n"
    "    <div id=\"control-card\" class=\"card control-anchor-tr\">\n"
    "      <div class=\"head\">\n"
    "        <div class=\"mark\">V</div>\n"
    "        <h1>VNC Monitor</h1>\n"
    "        <p class=\"sub\">Secure browser connection to the active GNOME desktop.</p>\n"
    "      </div>\n"
    "      <div class=\"body\">\n"
    "        <span class=\"badge\">HTTPS</span><span class=\"badge\">View only</span><span class=\"badge\">Active user only</span>\n"
    "        <div id=\"stream-controls\" class=\"stream-controls\">\n"
    "          <button id=\"control-toggle\" class=\"secondary control-toggle\" type=\"button\">Collapse</button>\n"
    "          <div id=\"stream-control-details\" class=\"stream-control-details\">\n"
    "            <button id=\"fullscreen\" class=\"secondary\" type=\"button\">Fullscreen</button>\n"
    "            <div class=\"control-position-label\">Panel position</div>\n"
    "            <div id=\"control-position\" class=\"anchor-picker\">\n"
    "              <button class=\"control-anchor-option\" type=\"button\" data-anchor=\"tl\">TL</button><button class=\"control-anchor-option\" type=\"button\" data-anchor=\"tc\">TC</button><button class=\"control-anchor-option\" type=\"button\" data-anchor=\"tr\">TR</button><br>\n"
    "              <button class=\"control-anchor-option\" type=\"button\" data-anchor=\"ml\">ML</button><span>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;</span><button class=\"control-anchor-option\" type=\"button\" data-anchor=\"mr\">MR</button><br>\n"
    "              <button class=\"control-anchor-option\" type=\"button\" data-anchor=\"bl\">BL</button><button class=\"control-anchor-option\" type=\"button\" data-anchor=\"bc\">BC</button><button class=\"control-anchor-option\" type=\"button\" data-anchor=\"br\">BR</button>\n"
    "            </div>\n"
    "          </div>\n"
    "        </div>\n"
    "        <form id=\"login\" method=\"post\" action=\"/api/login\" autocomplete=\"on\">\n"
    "          <label class=\"field\"><span>Username</span><input id=\"username\" name=\"username\" type=\"text\" autocomplete=\"username\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\" required></label>\n"
    "          <label class=\"field\"><span>Password</span><input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"current-password\" required></label>\n"
    "          <button id=\"connect\" type=\"submit\">Connect</button>\n"
    "          <button id=\"disconnect\" class=\"secondary\" type=\"button\" style=\"display:none\">Disconnect</button>\n"
    "          <button id=\"logout\" class=\"secondary\" type=\"button\" style=\"display:none\">Logout</button>\n"
    "        </form>\n"
    "        <div id=\"status\" class=\"status\"><span class=\"status-dot\"></span><span id=\"status-text\">Ready to connect.</span></div>\n"
    "        <p class=\"hint\">Authentication is bound to the currently active local GNOME Wayland user.<br><a href=\"/manage\">Manage sessions and settings</a></p>\n"
    "        <p id=\"protocol-info\" class=\"protocol\">Protocol client/server: <span id=\"protocol-client\">-</span> / <span id=\"protocol-server\">-</span><br>Build client/server: <span id=\"build-client\">-</span> / <span id=\"build-server\">-</span></p>\n"
    "        <p class=\"service-info\">Backend: <span id=\"service-health\">checking...</span><br><span id=\"service-telemetry\">telemetry unavailable</span></p>\n"
    "      </div>\n"
    "    </div>\n"
    "    <div id=\"viewer\" class=\"viewer\"><img id=\"video-frame-a\" class=\"jpeg-frame jpeg-frame-active\" alt=\"Remote desktop\"><img id=\"video-frame-b\" class=\"jpeg-frame\" alt=\"Remote desktop\"><video id=\"hls-video\" controls preload=\"auto\" webkit-playsinline></video></div>\n"
    "  </div>\n"
    "</body>\n"
    "</html>\n";

static const char protocol_worker_js[] =
    "(function () {\n"
    "  'use strict';\n"
    "  var socket = null;\n"
    "  var socketOpened = false;\n"
    "  var protocolConfirmed = false;\n"
    "  var framePending = false;\n"
    "  var frameSeq = 0;\n"
    "  var pendingSeq = 0;\n"
    "  var pendingDisplayState = null;\n"
    "  var clientProtocol = 0;\n"
    "\n"
    "  function emit(type, fields) {\n"
    "    var message = { type: type };\n"
    "    if (fields) { for (var k in fields) { if (Object.prototype.hasOwnProperty.call(fields, k)) message[k] = fields[k]; } }\n"
    "    self.postMessage(message);\n"
    "  }\n"
    "  function safeJson(text) { try { return JSON.parse(text || '{}'); } catch (e) { return {}; } }\n"
    "  function sendText(text) {\n"
    "    if (!socket || socket.readyState !== 1) return false;\n"
    "    try { socket.send(text); return true; } catch (e) { return false; }\n"
    "  }\n"
    "  function boundedUInt(value) {\n"
    "    var number = parseInt(value, 10);\n"
    "    if (!isFinite(number) || number < 0) return 0;\n"
    "    if (number > 4294967295) return 4294967295;\n"
    "    return Math.floor(number);\n"
    "  }\n"
    "  function diagnosticToken(value, pattern, fallback) {\n"
    "    var text = value == null ? '' : String(value);\n"
    "    return pattern.test(text) ? text : fallback;\n"
    "  }\n"
    "  function sendDiagnostic(kind, level, eventName, seq, bytes, observed, checksum, elapsed, mime) {\n"
    "    if (!protocolConfirmed) return false;\n"
    "    kind = diagnosticToken(kind, /^(log|telemetry)$/, '');\n"
    "    level = diagnosticToken(level, /^(info|warn|error)$/, '');\n"
    "    eventName = diagnosticToken(eventName, /^[a-z0-9-]{1,47}$/, '');\n"
    "    mime = diagnosticToken(mime, /^[A-Za-z0-9+.\\/-]{1,47}$/, 'unknown');\n"
    "    if (!kind || !level || !eventName) return false;\n"
    "    var sequence = boundedUInt(seq);\n"
    "    return sendText('{\"type\":\"client-' + kind + '\",\"level\":\"' + level + '\",\"event\":\"' + eventName + '\",\"seq\":' + String(sequence) + ',\"bytes\":' + String(boundedUInt(bytes)) + ',\"observed\":' + String(boundedUInt(observed)) + ',\"checksum\":' + String(boundedUInt(checksum)) + ',\"elapsed\":' + String(boundedUInt(elapsed)) + ',\"mime\":\"' + mime + '\"}');\n"
    "  }\n"
    "  function adler32Bytes(bytes) {\n"
    "    var a = 1;\n"
    "    var b = 0;\n"
    "    for (var i = 0; i < bytes.length; i++) {\n"
    "      a = (a + bytes[i]) % 65521;\n"
    "      b = (b + a) % 65521;\n"
    "    }\n"
    "    return (((b << 16) | a) >>> 0);\n"
    "  }\n"
    "\n"
    "  function displayToken(value, pattern) {\n"
    "    var text = value == null ? '' : String(value);\n"
    "    return pattern.test(text) ? text : '';\n"
    "  }\n"
    "  function flushDisplayState() {\n"
    "    if (!pendingDisplayState || framePending || !protocolConfirmed) return;\n"
    "    var state = pendingDisplayState;\n"
    "    pendingDisplayState = null;\n"
    "    var generation = boundedUInt(state.generation);\n"
    "    var width = boundedUInt(state.width);\n"
    "    var height = boundedUInt(state.height);\n"
    "    var mode = displayToken(state.mode, /^(window|fullscreen)$/);\n"
    "    var orientation = displayToken(state.orientation, /^(portrait|landscape)$/);\n"
    "    if (!generation || width < 64 || height < 64 || width > 4096 || height > 4096 || !mode || !orientation) return;\n"
    "    sendText('{\"type\":\"display-state\",\"generation\":' + String(generation) + ',\"mode\":\"' + mode + '\",\"orientation\":\"' + orientation + '\",\"width\":' + String(width) + ',\"height\":' + String(height) + '}');\n"
    "  }\n"
    "\n"
    "  function closeSocket() { if (socket) { try { socket.close(); } catch (e) {} } }\n"
    "\n"
    "  function handleText(text) {\n"
    "    var message = safeJson(text);\n"
    "    if (message.type === 'hello') {\n"
    "      var serverProtocol = parseInt(message.protocol, 10);\n"
    "      emit('hello', { protocol: serverProtocol, build: message.build || '' });\n"
    "      if (serverProtocol !== clientProtocol) {\n"
    "        emit('protocol-mismatch', { clientProtocol: clientProtocol, serverProtocol: serverProtocol, serverBuild: message.build || '' });\n"
    "        closeSocket();\n"
    "        return;\n"
    "      }\n"
    "      protocolConfirmed = true;\n"
    "      if (!sendText('{\"type\":\"protocol-ready\",\"protocol\":' + String(clientProtocol) + '}')) {\n"
    "        emit('error', { message: 'Could not confirm browser protocol version.' });\n"
    "        closeSocket();\n"
    "        return;\n"
    "      }\n"
    "      emit('protocol-ready', { protocol: serverProtocol, build: message.build || '' });\n"
    "      return;\n"
    "    }\n"
    "    if (!protocolConfirmed) { emit('error', { message: 'Server sent signalling before protocol handshake.' }); closeSocket(); return; }\n"
    "    if (message.type === 'ready') emit('ready', { state: message.state || '', media: message.media || '' });\n"
    "    else if (message.type === 'media-ready' && message.media === 'hls') emit('media-ready', { media: 'hls', url: message.url || '/live/index.m3u8' });\n"
    "    else if (message.type === 'telemetry') emit('telemetry', { event: message.event || '', seq: message.seq || 0, bytes: message.bytes || 0, sha256: message.sha256 || '', checksum: message.adler32 || 0, failures: message.failures || 0 });\n"
    "    else if (message.type === 'display-state-applied') emit('display-state-applied', { generation: message.generation || 0, width: message.width || 0, height: message.height || 0, mode: message.mode || '', orientation: message.orientation || '' });\n"
    "    else if (message.type === 'display-state-rejected') emit('display-state-rejected', { generation: message.generation || 0, reason: message.reason || 'rejected', width: message.width || 0, height: message.height || 0, mode: message.mode || '', orientation: message.orientation || '' });\n"
    "    else if (message.error) emit('error', { message: String(message.error) });\n"
    "  }\n"
    "\n"
    "  function handleBinary(data) {\n"
    "    if (!protocolConfirmed) { emit('error', { message: 'Binary media arrived before protocol handshake.' }); closeSocket(); return; }\n"
    "    if (framePending) { emit('error', { message: 'Second JPEG arrived before decode result.' }); closeSocket(); return; }\n"
    "    frameSeq += 1;\n"
    "    if (!data || typeof data.size !== 'number') {\n"
    "      sendDiagnostic('log', 'error', 'worker-blob-read-failed', frameSeq, 0, 0, 0, 0, 'unknown');\n"
    "      sendText('{\"type\":\"frame-nack\"}');\n"
    "      emit('frame-rejected', { seq: frameSeq, bytes: 0, reason: 'not-blob' });\n"
    "      return;\n"
    "    }\n"
    "    var length = data.size;\n"
    "    if (frameSeq <= 3) sendDiagnostic('telemetry', 'info', 'worker-frame-received', frameSeq, length, length, 0, 0, data.type || 'unknown');\n"
    "    var buffer;\n"
    "    var bytes;\n"
    "    try {\n"
    "      if (!self.FileReaderSync) throw new Error('FileReaderSync unavailable');\n"
    "      buffer = new FileReaderSync().readAsArrayBuffer(data);\n"
    "      bytes = new Uint8Array(buffer);\n"
    "    } catch (e) {\n"
    "      sendDiagnostic('log', 'error', 'worker-blob-read-failed', frameSeq, length, length, 0, 0, data.type || 'unknown');\n"
    "      sendText('{\"type\":\"frame-nack\"}');\n"
    "      emit('frame-rejected', { seq: frameSeq, bytes: length, reason: 'blob-read-failed' });\n"
    "      return;\n"
    "    }\n"
    "    var checksum = adler32Bytes(bytes);\n"
    "    var soi = length >= 2 && bytes[0] === 255 && bytes[1] === 216;\n"
    "    var eoi = length >= 2 && bytes[length - 2] === 255 && bytes[length - 1] === 217;\n"
    "    if (!soi || !eoi) {\n"
    "      sendDiagnostic('log', 'error', 'worker-jpeg-envelope-bad', frameSeq, length, bytes.byteLength, checksum, 0, data.type || 'unknown');\n"
    "      sendText('{\"type\":\"frame-nack\"}');\n"
    "      emit('frame-rejected', { seq: frameSeq, bytes: length, reason: 'incomplete-jpeg', soi: soi, eoi: eoi });\n"
    "      return;\n"
    "    }\n"
    "    var frameBlob = data;\n"
    "    try { frameBlob = data.slice(0, length, 'image/jpeg'); } catch (e) {}\n"
    "    if (frameSeq <= 3) sendDiagnostic('telemetry', 'info', 'worker-jpeg-envelope-ok', frameSeq, length, frameBlob.size, checksum, 0, frameBlob.type || 'unknown');\n"
    "    framePending = true;\n"
    "    pendingSeq = frameSeq;\n"
    "    emit('frame', { seq: frameSeq, bytes: length, checksum: checksum, data: frameBlob });\n"
    "  }\n"
    "\n"
    "  function connect(message) {\n"
    "    closeSocket();\n"
    "    clientProtocol = parseInt(message.protocol, 10) || 0;\n"
    "    protocolConfirmed = false; framePending = false; frameSeq = 0; pendingSeq = 0; socketOpened = false;\n"
    "    if (!self.WebSocket) { emit('socket-error', { message: 'WebSocket is unavailable in this worker.' }); return; }\n"
    "    try { socket = new WebSocket(message.url); }\n"
    "    catch (e) { socket = null; emit('socket-error', { message: 'WebSocket constructor failed.' }); return; }\n"
    "    try { socket.binaryType = 'blob'; } catch (e) {}\n"
    "    socket.onopen = function () { socketOpened = true; emit('socket-open'); };\n"
    "    socket.onmessage = function (event) { if (typeof event.data === 'string') handleText(event.data); else handleBinary(event.data); };\n"
    "    socket.onerror = function () { emit('socket-error', { message: socketOpened ? 'Secure WebSocket error.' : 'Secure WebSocket network/TLS handshake failed before open.' }); };\n"
    "    socket.onclose = function () {\n"
    "      var wasOpen = socketOpened; socket = null; socketOpened = false; protocolConfirmed = false; framePending = false; pendingSeq = 0; pendingDisplayState = null;\n"
    "      emit('closed', { wasOpen: wasOpen });\n"
    "    };\n"
    "  }\n"
    "\n"
    "  self.onmessage = function (event) {\n"
    "    var message = event.data || {};\n"
    "    if (message.type === 'connect') { connect(message); return; }\n"
    "    if (message.type === 'close') { closeSocket(); return; }\n"
    "    if (message.type === 'display-state') { pendingDisplayState = message; flushDisplayState(); return; }\n"
    "    if (message.type === 'client-diagnostic') {\n"
    "      sendDiagnostic(message.kind, message.level, message.event, message.seq, message.bytes, message.observed, message.checksum, message.elapsed, message.mime);\n"
    "      return;\n"
    "    }\n"
    "    if (message.type === 'frame-result') {\n"
    "      if (!framePending || message.seq !== pendingSeq) return;\n"
    "      sendText(message.ok ? '{\"type\":\"frame-ack\"}' : '{\"type\":\"frame-nack\"}');\n"
    "      framePending = false; pendingSeq = 0;\n"
    "      flushDisplayState();\n"
    "    }\n"
    "  };\n"
    "\n"
    "})();\n";

static const char client_js[] =
    "(function () {\n"
    "  'use strict';\n"
    "  var form = document.getElementById('login');\n"
    "  var username = document.getElementById('username');\n"
    "  var password = document.getElementById('password');\n"
    "  var connectButton = document.getElementById('connect');\n"
    "  var disconnectButton = document.getElementById('disconnect');\n"
    "  var logoutButton = document.getElementById('logout');\n"
    "  var browserAuthenticated = false;\n"
    "  var controlCard = document.getElementById('control-card');\n"
    "  var controlToggle = document.getElementById('control-toggle');\n"
    "  var fullscreenButton = document.getElementById('fullscreen');\n"
    "  var controlAnchorButtons = document.getElementsByClassName('control-anchor-option');\n"
    "  var statusBox = document.getElementById('status');\n"
    "  var statusText = document.getElementById('status-text');\n"
    "  var protocolClient = document.getElementById('protocol-client');\n"
    "  var protocolServer = document.getElementById('protocol-server');\n"
    "  var buildClient = document.getElementById('build-client');\n"
    "  var buildServer = document.getElementById('build-server');\n"
    "  var serviceHealth = document.getElementById('service-health');\n"
    "  var serviceTelemetry = document.getElementById('service-telemetry');\n"
    "  var publicStatusTimer = null;\n"
    "  var CLIENT_PROTOCOL_VERSION = " VNC_WEB_PROTOCOL_VERSION_TEXT ";\n"
    "  var CLIENT_BUILD = '" VNC_MONITOR_VERSION "';\n"
    "  var viewer = document.getElementById('viewer');\n"
    "  var videoFrameA = document.getElementById('video-frame-a');\n"
    "  var videoFrameB = document.getElementById('video-frame-b');\n"
    "  var activeVideoFrame = videoFrameA;\n"
    "  var stagingVideoFrame = videoFrameB;\n"
    "  var hlsVideo = document.getElementById('hls-video');\n"
    "  var objectUrlApi = window.URL || window.webkitURL;\n"
    "  var frameUrl = null;\n"
    "  var framePending = false;\n"
    "  var jpegRenderMode = 'probe';\n"
    "  var hlsLiveSeekDone = false;\n"
    "  var protocolWorker = null;\n"
    "  var workerConnected = false;\n"
    "  var protocolFatal = false;\n"
    "  var displaySyncReady = false;\n"
    "  var displayTimer = null;\n"
    "  var displayGeneration = 0;\n"
    "  var displayAppliedGeneration = 0;\n"
    "  var DISPLAY_STATE_HYSTERESIS_MS = 2500;\n"
    "  var CONTROL_STATE_KEY = 'vnc-monitor-control-v1';\n"
    "  var controlAnchor = 'tr';\n"
    "  var controlCollapsed = false;\n"
    "\n"
    "  function controlAnchorValid(value) { return /^(tl|tc|tr|ml|mr|bl|bc|br)$/.test(String(value || '')); }\n"
    "  function saveControlState() {\n"
    "    try { if (window.localStorage) window.localStorage.setItem(CONTROL_STATE_KEY, JSON.stringify({ anchor: controlAnchor, collapsed: controlCollapsed ? true : false })); } catch (e) {}\n"
    "  }\n"
    "  function applyControlState() {\n"
    "    if (!controlCard) return;\n"
    "    controlCard.className = 'card control-anchor-' + controlAnchor + (controlCollapsed ? ' control-collapsed' : '');\n"
    "    if (controlToggle) { controlToggle.innerHTML = controlCollapsed ? '+' : 'Collapse'; controlToggle.title = controlCollapsed ? 'Open controls' : 'Collapse controls'; }\n"
    "    for (var i = 0; i < controlAnchorButtons.length; i++) {\n"
    "      var anchor = controlAnchorButtons[i].getAttribute('data-anchor');\n"
    "      controlAnchorButtons[i].className = 'control-anchor-option' + (anchor === controlAnchor ? ' active' : '');\n"
    "    }\n"
    "  }\n"
    "  function loadControlState() {\n"
    "    try {\n"
    "      if (!window.localStorage) return;\n"
    "      var raw = window.localStorage.getItem(CONTROL_STATE_KEY);\n"
    "      if (!raw) return;\n"
    "      var state = safeJson(raw);\n"
    "      if (controlAnchorValid(state.anchor)) controlAnchor = state.anchor;\n"
    "      if (state.collapsed === true || state.collapsed === false) controlCollapsed = state.collapsed;\n"
    "    } catch (e) {}\n"
    "  }\n"
    "  function setControlAnchor(anchor) {\n"
    "    if (!controlAnchorValid(anchor)) return;\n"
    "    controlAnchor = anchor; saveControlState(); applyControlState();\n"
    "  }\n"
    "  function toggleControls() { controlCollapsed = !controlCollapsed; saveControlState(); applyControlState(); }\n"
    "  function bindControlAnchors() {\n"
    "    for (var i = 0; i < controlAnchorButtons.length; i++) {\n"
    "      (function (button) { button.onclick = function () { setControlAnchor(button.getAttribute('data-anchor')); }; })(controlAnchorButtons[i]);\n"
    "    }\n"
    "  }\n"
    "  function browserFullscreenElement() { return document.fullscreenElement || document.webkitFullscreenElement || null; }\n"
    "  function updateFullscreenButton() {\n"
    "    if (!fullscreenButton) return;\n"
    "    fullscreenButton.innerHTML = browserFullscreenElement() || hlsVideo.webkitDisplayingFullscreen ? 'Exit fullscreen' : 'Fullscreen';\n"
    "  }\n"
    "  function toggleFullscreen() {\n"
    "    if (browserFullscreenElement()) {\n"
    "      try {\n"
    "        if (document.exitFullscreen) document.exitFullscreen();\n"
    "        else if (document.webkitExitFullscreen) document.webkitExitFullscreen();\n"
    "        else if (document.webkitCancelFullScreen) document.webkitCancelFullScreen();\n"
    "      } catch (e) {}\n"
    "      return;\n"
    "    }\n"
    "    if (hlsVideo.webkitDisplayingFullscreen && hlsVideo.webkitExitFullscreen) { try { hlsVideo.webkitExitFullscreen(); } catch (e) {} return; }\n"
    "    var request = viewer.requestFullscreen || viewer.webkitRequestFullscreen || viewer.webkitRequestFullScreen;\n"
    "    if (request) {\n"
    "      try { request.call(viewer); return; } catch (e) {}\n"
    "    }\n"
    "    if (hlsVideo.style.display !== 'none' && hlsVideo.webkitEnterFullscreen) { try { hlsVideo.webkitEnterFullscreen(); return; } catch (e) {} }\n"
    "    if (navigator.standalone) setStatus('Already running as a standalone full-screen web app.', 'ok');\n"
    "    else setStatus('Fullscreen is unavailable in this Safari. Use Add to Home Screen, then open VNC Monitor from the Home Screen.', 'error');\n"
    "  }\n"
    "\n"    "  function currentDisplayMode() {\n"
    "    if (document.fullscreenElement || document.webkitFullscreenElement || navigator.standalone) return 'fullscreen';\n"
    "    return 'window';\n"
    "  }\n"
    "  function currentViewport() {\n"
    "    var width = document.documentElement && document.documentElement.clientWidth ? document.documentElement.clientWidth : window.innerWidth;\n"
    "    var height = document.documentElement && document.documentElement.clientHeight ? document.documentElement.clientHeight : window.innerHeight;\n"
    "    width = Math.max(64, Math.min(4096, Math.round(width || 0)));\n"
    "    height = Math.max(64, Math.min(4096, Math.round(height || 0)));\n"
    "    return { width: width, height: height };\n"
    "  }\n"
    "  function currentOrientation(viewport) {\n"
    "    if (typeof window.orientation === 'number' && (window.orientation === 90 || window.orientation === -90)) return 'landscape';\n"
    "    if (typeof window.orientation === 'number' && (window.orientation === 0 || window.orientation === 180 || window.orientation === -180)) return 'portrait';\n"
    "    return viewport.width >= viewport.height ? 'landscape' : 'portrait';\n"
    "  }\n"
    "  function sendDisplayState() {\n"
    "    displayTimer = null;\n"
    "    if (!displaySyncReady || !protocolWorker) return;\n"
    "    var viewport = currentViewport();\n"
    "    displayGeneration += 1;\n"
    "    try { protocolWorker.postMessage({ type: 'display-state', generation: displayGeneration, mode: currentDisplayMode(), orientation: currentOrientation(viewport), width: viewport.width, height: viewport.height }); } catch (e) {}\n"
    "  }\n"
    "  function scheduleDisplayState() {\n"
    "    if (displayTimer) window.clearTimeout(displayTimer);\n"
    "    displayTimer = window.setTimeout(sendDisplayState, DISPLAY_STATE_HYSTERESIS_MS);\n"
    "  }\n"
    "  function resetDisplaySync() {\n"
    "    displaySyncReady = false;\n"
    "    if (displayTimer) window.clearTimeout(displayTimer);\n"
    "    displayTimer = null;\n"
    "  }\n"
    "\n"
    "  function setStatus(text, kind) {\n"
    "    statusText.innerHTML = '';\n"
    "    statusText.appendChild(document.createTextNode(text));\n"
    "    statusBox.className = 'status' + (kind ? ' status-' + kind : '');\n"
    "  }\n"
    "\n"
    "  function setNodeText(node, value) {\n"
    "    if (!node) return;\n"
    "    node.innerHTML = '';\n"
    "    node.appendChild(document.createTextNode(String(value)));\n"
    "  }\n"
    "\n"
    "  function schedulePublicStatus(delay) {\n"
    "    if (publicStatusTimer) window.clearTimeout(publicStatusTimer);\n"
    "    publicStatusTimer = window.setTimeout(loadPublicStatus, delay);\n"
    "  }\n"
    "  function loadPublicStatus() {\n"
    "    publicStatusTimer = null;\n"
    "    var xhr;\n"
    "    try { xhr = new XMLHttpRequest(); } catch (e) { schedulePublicStatus(5000); return; }\n"
    "    xhr.onreadystatechange = function () {\n"
    "      if (xhr.readyState !== 4) return;\n"
    "      if (xhr.status === 200) {\n"
    "        var publicStatus = safeJson(xhr.responseText);\n"
    "        setNodeText(serviceHealth, publicStatus.service === 'ready' ? 'online' : 'unknown');\n"
    "        if (publicStatus.protocol) setNodeText(protocolServer, publicStatus.protocol);\n"
    "        if (publicStatus.build) setNodeText(buildServer, publicStatus.build);\n"
    "        var telemetry = publicStatus.telemetry || {};\n"
    "        var viewerStatus = publicStatus.viewer || {};\n"
    "        var seconds = Math.floor((publicStatus.uptimeMs || 0) / 1000);\n"
    "        setNodeText(serviceTelemetry, 'state=' + String(publicStatus.slot || 'unknown') + ' transport=' + String(viewerStatus.transport || 'none') + ' uptime=' + String(seconds) + 's frames=' + String(telemetry.framesForwarded || 0) + '/' + String(telemetry.framesAcked || 0) + '/' + String(telemetry.framesNacked || 0));\n"
    "      } else { setNodeText(serviceHealth, 'offline/unreachable'); }\n"
    "      schedulePublicStatus(5000);\n"
    "    };\n"
    "    try { xhr.open('GET', '/api/status?ts=' + String(new Date().getTime()), true); xhr.send(null); }\n"
    "    catch (e) { setNodeText(serviceHealth, 'offline/unreachable'); schedulePublicStatus(5000); }\n"
    "  }\n"
    "\n"
    "  function updateProtocolInfo(serverVersion, serverBuild) {\n"
    "    setNodeText(protocolClient, CLIENT_PROTOCOL_VERSION);\n"
    "    setNodeText(buildClient, CLIENT_BUILD);\n"
    "    setNodeText(protocolServer, serverVersion == null ? '-' : serverVersion);\n"
    "    setNodeText(buildServer, serverBuild || '-');\n"
    "  }\n"
    "\n"
    "  function protocolReloadMarker(version) { return 'protocol-reload=' + encodeURIComponent(String(version)); }\n"
    "  function protocolReloadAlreadyAttempted(version) { return window.location.search.indexOf(protocolReloadMarker(version)) >= 0; }\n"
    "  function reloadForProtocol(version) {\n"
    "    var search = window.location.search || '';\n"
    "    var separator = search ? '&' : '?';\n"
    "    window.location.replace(window.location.pathname + search + separator + protocolReloadMarker(version) + '&reload=' + String(new Date().getTime()));\n"
    "  }\n"
    "  function handleProtocolMismatch(clientVersion, serverVersion) {\n"
    "    protocolFatal = true;\n"
    "    var prefix = 'Protocol mismatch: client=' + String(clientVersion) + ', server=' + String(serverVersion) + '. ';\n"
    "    if (!protocolReloadAlreadyAttempted(serverVersion)) {\n"
    "      setStatus(prefix + 'Reloading page...', 'error');\n"
    "      stopProtocolWorker();\n"
    "      window.setTimeout(function () { reloadForProtocol(serverVersion); }, 250);\n"
    "    } else {\n"
    "      setStatus(prefix + 'Automatic reload already attempted; client assets are still stale.', 'error');\n"
    "      stopProtocolWorker();\n"
    "    }\n"
    "  }\n"
    "\n"
    "  function setAuthenticated(authenticated) {\n"
    "    browserAuthenticated = authenticated ? true : false;\n"
    "    logoutButton.style.display = browserAuthenticated ? 'block' : 'none';\n"
    "    username.disabled = browserAuthenticated;\n"
    "    password.disabled = browserAuthenticated;\n"
    "    if (!workerConnected) connectButton.innerHTML = browserAuthenticated ? 'Reconnect' : 'Connect';\n"
    "  }\n"
    "  function setFormBusy(busy) {\n"
    "    username.disabled = busy || browserAuthenticated;\n"
    "    password.disabled = busy || browserAuthenticated;\n"
    "    connectButton.disabled = busy;\n"
    "    connectButton.innerHTML = busy ? 'Connecting...' : (browserAuthenticated ? 'Reconnect' : 'Connect');\n"
    "  }\n"
    "\n"
    "  function setConnected(connected) {\n"
    "    username.disabled = connected || browserAuthenticated;\n"
    "    password.disabled = connected || browserAuthenticated;\n"
    "    connectButton.style.display = connected ? 'none' : 'block';\n"
    "    disconnectButton.style.display = connected ? 'block' : 'none';\n"
    "    logoutButton.style.display = browserAuthenticated ? 'block' : 'none';\n"
    "    if (!connected) { connectButton.disabled = false; connectButton.innerHTML = browserAuthenticated ? 'Reconnect' : 'Connect'; }\n"
    "  }\n"
    "\n"
    "  function safeJson(text) {\n"
    "    try { return JSON.parse(text || '{}'); }\n"
    "    catch (e) { return {}; }\n"
    "  }\n"
    "\n"
    "  function clearVideoFrame() {\n"
    "    if (frameUrl && objectUrlApi && frameUrl.indexOf('blob:') === 0) {\n"
    "      try { objectUrlApi.revokeObjectURL(frameUrl); } catch (e) {}\n"
    "    }\n"
    "    frameUrl = null;\n"
    "    framePending = false;\n"
    "    jpegRenderMode = 'probe';\n"
    "    hlsLiveSeekDone = false;\n"
    "    videoFrameA.onload = null; videoFrameA.onerror = null; videoFrameA.removeAttribute('src');\n"
    "    videoFrameB.onload = null; videoFrameB.onerror = null; videoFrameB.removeAttribute('src');\n"
    "    videoFrameA.className = 'jpeg-frame jpeg-frame-active';\n"
    "    videoFrameB.className = 'jpeg-frame';\n"
    "    activeVideoFrame = videoFrameA;\n"
    "    stagingVideoFrame = videoFrameB;\n"
    "    try { hlsVideo.pause(); } catch (e) {}\n"
    "    hlsVideo.removeAttribute('src');\n"
    "    hlsVideo.style.display = 'none';\n"
    "    try { hlsVideo.load(); } catch (e) {}\n"
    "    viewer.style.display = 'none';\n"
    "    document.body.className = '';\n"
    "  }\n"
    "\n"
    "  function seekHlsNearLiveEdge(reason) {\n"
    "    if (hlsLiveSeekDone || !hlsVideo.seekable || !hlsVideo.seekable.length) return false;\n"
    "    try {\n"
    "      var index = hlsVideo.seekable.length - 1;\n"
    "      var start = hlsVideo.seekable.start(index);\n"
    "      var end = hlsVideo.seekable.end(index);\n"
    "      var target = end - 0.75;\n"
    "      if (target < start) target = start;\n"
    "      if (!isFinite(target) || target < 0) return false;\n"
    "      hlsVideo.currentTime = target;\n"
    "      hlsLiveSeekDone = true;\n"
    "      if (window.console && console.log) console.log('VNC Monitor: HLS live-edge seek reason=' + reason + ' start=' + start.toFixed(3) + ' end=' + end.toFixed(3) + ' target=' + target.toFixed(3));\n"
    "      return true;\n"
    "    } catch (e) {\n"
    "      if (window.console && console.log) console.log('VNC Monitor: HLS live-edge seek deferred reason=' + reason);\n"
    "      return false;\n"
    "    }\n"
    "  }\n"
    "\n"
    "  function startHlsVideo(url) {\n"
    "    if (!url) return;\n"
    "    if (frameUrl && objectUrlApi) { try { objectUrlApi.revokeObjectURL(frameUrl); } catch (e) {} }\n"
    "    frameUrl = null;\n"
    "    hlsLiveSeekDone = false;\n"
    "    videoFrameA.onload = null; videoFrameA.onerror = null; videoFrameA.removeAttribute('src'); videoFrameA.className = 'jpeg-frame';\n"
    "    videoFrameB.onload = null; videoFrameB.onerror = null; videoFrameB.removeAttribute('src'); videoFrameB.className = 'jpeg-frame';\n"
    "    hlsVideo.style.display = 'block';\n"
    "    viewer.style.display = 'block';\n"
    "    document.body.className = 'streaming';\n"
    "    hlsVideo.onloadedmetadata = function () { seekHlsNearLiveEdge('loadedmetadata'); };\n"
    "    hlsVideo.oncanplay = function () { seekHlsNearLiveEdge('canplay'); };\n"
    "    hlsVideo.onplaying = function () {\n"
    "      seekHlsNearLiveEdge('playing');\n"
    "      setStatus('Connected. H.264/HLS video playing near live edge.', 'ok');\n"
    "    };\n"
    "    hlsVideo.onerror = function () {\n"
    "      if (window.console && console.error) console.error('VNC Monitor: native HLS video error code=' + String(hlsVideo.error ? hlsVideo.error.code : 0));\n"
    "      setStatus('Native HLS video could not be played.', 'error');\n"
    "    };\n"
    "    hlsVideo.src = url + (url.indexOf('?') >= 0 ? '&' : '?') + 'v=' + String(new Date().getTime());\n"
    "    try { hlsVideo.load(); } catch (e) {}\n"
    "    try { hlsVideo.play(); } catch (e) {}\n"
    "    setStatus('H.264/HLS stream ready. Tap Play if Safari does not start automatically.', 'ok');\n"
    "  }\n"
    "\n"
    "  function reportVideoFrame(seq, ok) {\n"
    "    if (!protocolWorker) return;\n"
    "    try { protocolWorker.postMessage({ type: 'frame-result', seq: seq, ok: ok ? true : false }); } catch (e) {}\n"
    "  }\n"
    "\n"
    "  function reportClientDiagnostic(kind, level, eventName, seq, bytes, observed, checksum, elapsed, mime) {\n"
    "    if (!protocolWorker) return;\n"
    "    try { protocolWorker.postMessage({ type: 'client-diagnostic', kind: kind, level: level, event: eventName, seq: seq || 0, bytes: bytes || 0, observed: observed || 0, checksum: checksum || 0, elapsed: elapsed || 0, mime: mime || 'unknown' }); } catch (e) {}\n"
    "  }\n"
    "  function adler32BinaryString(value) {\n"
    "    var a = 1;\n"
    "    var b = 0;\n"
    "    for (var i = 0; i < value.length; i++) {\n"
    "      a = (a + (value.charCodeAt(i) & 255)) % 65521;\n"
    "      b = (b + a) % 65521;\n"
    "    }\n"
    "    return (((b << 16) | a) >>> 0);\n"
    "  }\n"
    "  function diagnoseClonedBlob(seq, bytes, blob) {\n"
    "    if (seq > 3 || !blob || !window.FileReader) return;\n"
    "    try {\n"
    "      var reader = new FileReader();\n"
    "      reader.onload = function () {\n"
    "        var value = typeof reader.result === 'string' ? reader.result : '';\n"
    "        reportClientDiagnostic('telemetry', 'info', 'main-blob-integrity', seq, bytes, blob.size || 0, adler32BinaryString(value), 0, blob.type || 'unknown');\n"
    "      };\n"
    "      reader.onerror = function () { reportClientDiagnostic('log', 'error', 'main-blob-read-failed', seq, bytes, blob.size || 0, 0, 0, blob.type || 'unknown'); };\n"
    "      reader.readAsBinaryString(blob);\n"
    "    } catch (e) {\n"
    "      reportClientDiagnostic('log', 'error', 'main-blob-read-failed', seq, bytes, blob.size || 0, 0, 0, blob.type || 'unknown');\n"
    "    }\n"
    "  }\n"
    "\n"
    "  function renderVideoFrame(frame) {\n"
    "    var blob = frame ? frame.data : null;\n"
    "    var seq = frame ? frame.seq : 0;\n"
    "    var expectedBytes = frame && frame.bytes ? frame.bytes : 0;\n"
    "    if (!objectUrlApi || !window.Blob) { setStatus('This browser cannot display the binary video stream.', 'error'); return; }\n"
    "    if (framePending) return;\n"
    "    framePending = true;\n"
    "    hlsVideo.style.display = 'none';\n"
    "    stagingVideoFrame.onload = null;\n"
    "    stagingVideoFrame.onerror = null;\n"
    "    stagingVideoFrame.removeAttribute('src');\n"
    "    stagingVideoFrame.className = 'jpeg-frame';\n"
    "    var nextUrl = null;\n"
    "    var previousUrl = frameUrl;\n"
    "    var byteLength = expectedBytes || (blob && typeof blob.size === 'number' ? blob.size : 0);\n"
    "    var decodeStarted = new Date().getTime();\n"
    "    var decodeMode = jpegRenderMode === 'data' ? 'data' : 'blob';\n"
    "\n"
    "    function finishSuccess() {\n"
    "      stagingVideoFrame.onload = null;\n"
    "      stagingVideoFrame.onerror = null;\n"
    "      var oldActive = activeVideoFrame;\n"
    "      activeVideoFrame = stagingVideoFrame;\n"
    "      stagingVideoFrame = oldActive;\n"
    "      activeVideoFrame.className = 'jpeg-frame jpeg-frame-active';\n"
    "      stagingVideoFrame.className = 'jpeg-frame';\n"
    "      stagingVideoFrame.onload = null;\n"
    "      stagingVideoFrame.onerror = null;\n"
    "      stagingVideoFrame.removeAttribute('src');\n"
    "      frameUrl = nextUrl;\n"
    "      viewer.style.display = 'block';\n"
    "      document.body.className = 'streaming';\n"
    "      framePending = false;\n"
    "      if (previousUrl && previousUrl !== frameUrl && previousUrl.indexOf('blob:') === 0) {\n"
    "        try { objectUrlApi.revokeObjectURL(previousUrl); } catch (e) {}\n"
    "      }\n"
    "      if (decodeMode === 'data') {\n"
    "        if (jpegRenderMode !== 'data') reportClientDiagnostic('telemetry', 'info', 'img-data-url-decode-ok', seq, byteLength, blob.size || 0, frame.checksum || 0, new Date().getTime() - decodeStarted, blob.type || 'unknown');\n"
    "        jpegRenderMode = 'data';\n"
    "      } else if (jpegRenderMode === 'probe') {\n"
    "        jpegRenderMode = 'blob';\n"
    "      }\n"
    "      reportVideoFrame(seq, true);\n"
    "      setStatus('Connected. Live browser video.', 'ok');\n"
    "    }\n"
    "\n"
    "    function finishFailure(eventName) {\n"
    "      stagingVideoFrame.onload = null;\n"
    "      stagingVideoFrame.onerror = null;\n"
    "      stagingVideoFrame.removeAttribute('src');\n"
    "      stagingVideoFrame.className = 'jpeg-frame';\n"
    "      if (nextUrl && nextUrl.indexOf('blob:') === 0) {\n"
    "        try { objectUrlApi.revokeObjectURL(nextUrl); } catch (e) {}\n"
    "      }\n"
    "      framePending = false;\n"
    "      reportClientDiagnostic('log', 'error', eventName, seq, byteLength, blob && blob.size ? blob.size : 0, frame.checksum || 0, new Date().getTime() - decodeStarted, blob && blob.type ? blob.type : 'unknown');\n"
    "      reportVideoFrame(seq, false);\n"
    "      if (window.console && console.error) console.error('VNC Monitor: Safari rejected JPEG frame seq=' + String(seq) + ' mode=' + decodeMode + ' bytes=' + String(blob && blob.size ? blob.size : 0) + ' inputBytes=' + String(byteLength) + '; dropped after fallback');\n"
    "      if (!frameUrl) setStatus('JPEG decode failed through Blob URL and data URL; dropped one frame.', 'working');\n"
    "    }\n"
    "\n"
    "    function tryDataUrlFallback() {\n"
    "      if (!window.FileReader) { finishFailure('img-data-url-read-failed'); return; }\n"
    "      decodeMode = 'data';\n"
    "      var reader;\n"
    "      try { reader = new FileReader(); }\n"
    "      catch (e) { finishFailure('img-data-url-read-failed'); return; }\n"
    "      reader.onload = function () {\n"
    "        if (typeof reader.result !== 'string' || reader.result.indexOf('data:image/jpeg') !== 0) {\n"
    "          finishFailure('img-data-url-read-failed');\n"
    "          return;\n"
    "        }\n"
    "        nextUrl = reader.result;\n"
    "        stagingVideoFrame.onload = finishSuccess;\n"
    "        stagingVideoFrame.onerror = function () { finishFailure('img-data-url-decode-error'); };\n"
    "        stagingVideoFrame.src = nextUrl;\n"
    "      };\n"
    "      reader.onerror = function () { finishFailure('img-data-url-read-failed'); };\n"
    "      try { reader.readAsDataURL(blob); }\n"
    "      catch (e) { finishFailure('img-data-url-read-failed'); }\n"
    "    }\n"
    "\n"
    "    try {\n"
    "      if (!blob || typeof blob.size !== 'number') throw new Error('worker frame is not a Blob');\n"
    "      if (seq <= 3) reportClientDiagnostic('telemetry', 'info', 'main-frame-received', seq, byteLength, blob.size, frame.checksum || 0, 0, blob.type || 'unknown');\n"
    "      diagnoseClonedBlob(seq, byteLength, blob);\n"
    "      if (jpegRenderMode === 'data') {\n"
    "        tryDataUrlFallback();\n"
    "        return;\n"
    "      }\n"
    "      nextUrl = objectUrlApi.createObjectURL(blob);\n"
    "    } catch (e) {\n"
    "      framePending = false;\n"
    "      reportClientDiagnostic('log', 'error', 'frame-prepare-error', seq, byteLength, blob && blob.size ? blob.size : 0, 0, 0, blob && blob.type ? blob.type : 'unknown');\n"
    "      reportVideoFrame(seq, false);\n"
    "      setStatus('Could not prepare the browser video frame.', 'error');\n"
    "      return;\n"
    "    }\n"
    "\n"
    "    stagingVideoFrame.onload = finishSuccess;\n"
    "    stagingVideoFrame.onerror = function () {\n"
    "      reportClientDiagnostic('log', 'warn', 'img-blob-decode-error', seq, byteLength, blob.size || 0, frame.checksum || 0, new Date().getTime() - decodeStarted, blob.type || 'unknown');\n"
    "      if (nextUrl && nextUrl.indexOf('blob:') === 0) {\n"
    "        try { objectUrlApi.revokeObjectURL(nextUrl); } catch (e) {}\n"
    "      }\n"
    "      stagingVideoFrame.onload = null;\n"
    "      stagingVideoFrame.onerror = null;\n"
    "      stagingVideoFrame.removeAttribute('src');\n"
    "      tryDataUrlFallback();\n"
    "    };\n"
    "    stagingVideoFrame.src = nextUrl;\n"
    "  }\n"
    "\n"
    "  function stopProtocolWorker() {\n"
    "    if (!protocolWorker) return;\n"
    "    try { protocolWorker.postMessage({ type: 'close' }); } catch (e) {}\n"
    "  }\n"
    "\n"
    "  function destroyProtocolWorker() {\n"
    "    if (!protocolWorker) return;\n"
    "    try { protocolWorker.terminate(); } catch (e) {}\n"
    "    protocolWorker = null;\n"
    "    workerConnected = false;\n"
    "  }\n"
    "\n"
    "  function startProtocolWorker() {\n"
    "    destroyProtocolWorker();\n"
    "    protocolFatal = false;\n"
    "    updateProtocolInfo(null, null);\n"
    "    try { protocolWorker = new Worker('/protocol-worker.js?protocol=' + String(CLIENT_PROTOCOL_VERSION)); }\n"
    "    catch (e) {\n"
    "      protocolWorker = null;\n"
    "      setFormBusy(false);\n"
    "      setConnected(false);\n"
    "      setStatus('Protocol worker could not be started.', 'error');\n"
    "      return;\n"
    "    }\n"
    "    protocolWorker.onmessage = function (event) {\n"
    "      var message = event.data || {};\n"
    "      if (message.type === 'socket-open') {\n"
    "        workerConnected = true;\n"
    "        setFormBusy(false);\n"
    "        setConnected(true);\n"
    "        setStatus('Connected. Verifying browser protocol...', 'working');\n"
    "      } else if (message.type === 'hello') {\n"
    "        updateProtocolInfo(message.protocol, message.build);\n"
    "      } else if (message.type === 'protocol-mismatch') {\n"
    "        updateProtocolInfo(message.serverProtocol, message.serverBuild);\n"
    "        handleProtocolMismatch(message.clientProtocol, message.serverProtocol);\n"
    "      } else if (message.type === 'protocol-ready') {\n"
    "        updateProtocolInfo(message.protocol, message.build);\n"
    "        displaySyncReady = true;\n"
    "        scheduleDisplayState();\n"
    "        setStatus('Connected. Protocol verified; preparing browser media...', 'ok');\n"
    "      } else if (message.type === 'ready') {\n"
    "        setStatus('Connected. Preparing browser media...', 'ok');\n"
    "      } else if (message.type === 'media-ready' && message.media === 'hls') {\n"
    "        startHlsVideo(message.url || '/live/index.m3u8');\n"
    "      } else if (message.type === 'frame') {\n"
    "        renderVideoFrame(message);\n"
    "      } else if (message.type === 'telemetry') {\n"
    "        if (window.console && console.log) console.log('VNC Monitor telemetry event=' + String(message.event || '') + ' seq=' + String(message.seq || 0) + ' bytes=' + String(message.bytes || 0) + ' checksum=' + String(message.checksum || 0) + ' failures=' + String(message.failures || 0));\n"
    "      } else if (message.type === 'display-state-applied') {\n"
    "        if (message.generation >= displayAppliedGeneration) { displayAppliedGeneration = message.generation; if (window.console && console.log) console.log('VNC Monitor display state applied generation=' + String(message.generation) + ' size=' + String(message.width) + 'x' + String(message.height) + ' mode=' + String(message.mode) + ' orientation=' + String(message.orientation)); }\n"
    "      } else if (message.type === 'display-state-rejected') {\n"
    "        if (message.reason === 'rate-limit' || message.reason === 'not-accepted') scheduleDisplayState();\n"
    "        else if (message.reason === 'runtime') {\n"
    "          if (window.console && console.error) console.error('VNC Monitor display resize rejected generation=' + String(message.generation) + ' keeping=' + String(message.width) + 'x' + String(message.height));\n"
    "          setStatus('Connected. Display resize was rejected; keeping ' + String(message.width) + 'x' + String(message.height) + '.', 'working');\n"
    "        }\n"
    "      } else if (message.type === 'frame-rejected') {\n"
    "        if (window.console && console.error) console.error('VNC Monitor worker rejected JPEG frame seq=' + String(message.seq || 0) + ' bytes=' + String(message.bytes || 0) + ' reason=' + String(message.reason || 'unknown'));\n"
    "        setStatus('Invalid JPEG frame dropped; waiting for the next frame.', 'working');\n"
    "      } else if (message.type === 'socket-error') {\n"
    "        if (!protocolFatal) setStatus(message.message || 'Secure WebSocket error.', 'error');\n"
    "      } else if (message.type === 'error') {\n"
    "        if (!protocolFatal) setStatus('Server/protocol message: ' + String(message.message || 'unknown error'), 'error');\n"
    "      } else if (message.type === 'closed') {\n"
    "        var wasOpen = workerConnected || message.wasOpen;\n"
    "        resetDisplaySync();\n"
    "        destroyProtocolWorker();\n"
    "        setFormBusy(false);\n"
    "        setConnected(false);\n"
    "        clearVideoFrame();\n"
    "        if (!protocolFatal) {\n"
    "          if (wasOpen) setStatus('Disconnected. Ready to connect again.', '');\n"
    "          else if (statusBox.className.indexOf('status-error') < 0) setStatus('Connection closed before it was ready.', 'error');\n"
    "        }\n"
    "      }\n"
    "    };\n"
    "    protocolWorker.onerror = function () { if (!protocolFatal) setStatus('Protocol worker failed.', 'error'); };\n"
    "    protocolWorker.postMessage({ type: 'connect', url: 'wss://' + window.location.host + '/ws', protocol: CLIENT_PROTOCOL_VERSION });\n"
    "  }\n"
    "\n"
    "  function resumeSession(automatic) {\n"
    "    if (protocolWorker) stopProtocolWorker();\n"
    "    setFormBusy(true);\n"
    "    if (!automatic) setStatus('Reconnecting authenticated session...', 'working');\n"
    "    var xhr = new XMLHttpRequest();\n"
    "    xhr.open('POST', '/api/resume', true);\n"
    "    xhr.setRequestHeader('X-VNC-Monitor-Control', '1');\n"
    "    xhr.onreadystatechange = function () {\n"
    "      if (xhr.readyState !== 4) return;\n"
    "      var result = safeJson(xhr.responseText);\n"
    "      if (xhr.status === 200 && result.ok) {\n"
    "        setAuthenticated(true);\n"
    "        setStatus('Authenticated session restored. Opening secure control channel...', 'working');\n"
    "        startProtocolWorker();\n"
    "        return;\n"
    "      }\n"
    "      setFormBusy(false);\n"
    "      if (xhr.status === 401) { setAuthenticated(false); if (!automatic) setStatus('Authentication expired. Enter your password again.', 'error'); }\n"
    "      else if (xhr.status === 409) setStatus('Another VNC or WebRTC session is already active.', 'error');\n"
    "      else if (!automatic) setStatus('Reconnect failed. HTTP ' + xhr.status + '.', 'error');\n"
    "    };\n"
    "    xhr.onerror = function () { setFormBusy(false); if (!automatic) setStatus('Network error while reconnecting.', 'error'); };\n"
    "    xhr.send(null);\n"
    "  }\n"
    "  function logoutBrowser() {\n"
    "    if (protocolWorker) stopProtocolWorker();\n"
    "    var xhr = new XMLHttpRequest();\n"
    "    xhr.open('POST', '/api/logout', true);\n"
    "    xhr.setRequestHeader('X-VNC-Monitor-Control', '1');\n"
    "    xhr.onreadystatechange = function () {\n"
    "      if (xhr.readyState !== 4) return;\n"
    "      if (xhr.status === 200) {\n"
    "        setAuthenticated(false);\n"
    "        setConnected(false);\n"
    "        clearVideoFrame();\n"
    "        username.value = ''; password.value = '';\n"
    "        setStatus('Logged out.', '');\n"
    "      } else setStatus('Logout failed. HTTP ' + xhr.status + '.', 'error');\n"
    "    };\n"
    "    xhr.send(null);\n"
    "  }\n"
    "\n"    "  function login(event) {\n"
    "    if (event && event.preventDefault) event.preventDefault();\n"
    "    if (protocolWorker) stopProtocolWorker();\n"
    "    if (browserAuthenticated) { resumeSession(false); return false; }\n"
    "\n"
    "    var userValue = username.value || '';\n"
    "    var passwordValue = password.value || '';\n"
    "    if (!userValue || !passwordValue) {\n"
    "      setStatus('Enter your username and password.', 'error');\n"
    "      return false;\n"
    "    }\n"
    "\n"
    "    setFormBusy(true);\n"
    "    setStatus('Authenticating...', 'working');\n"
    "\n"
    "    var xhr = new XMLHttpRequest();\n"
    "    xhr.open('POST', '/api/login', true);\n"
    "    xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');\n"
    "    xhr.onreadystatechange = function () {\n"
    "      if (xhr.readyState !== 4) return;\n"
    "\n"
    "      password.value = '';\n"
    "      passwordValue = '';\n"
    "      var result = safeJson(xhr.responseText);\n"
    "\n"
    "      if (xhr.status === 200 && result.ok) {\n"
    "        setAuthenticated(true);\n"
    "        setStatus('Authenticated. Opening secure control channel...', 'working');\n"
    "        startProtocolWorker();\n"
    "        return;\n"
    "      }\n"
    "\n"
    "      setFormBusy(false);\n"
    "      if (xhr.status === 401) setStatus('Authentication failed.', 'error');\n"
    "      else if (xhr.status === 409) setStatus('Another VNC or WebRTC session is already active.', 'error');\n"
    "      else if (xhr.status === 503) setStatus('The active GNOME session is not available.', 'error');\n"
    "      else if (xhr.status === 403) setStatus('This browser request was rejected.', 'error');\n"
    "      else setStatus('Connection failed. HTTP ' + xhr.status + '.', 'error');\n"
    "    };\n"
    "    xhr.onerror = function () {\n"
    "      password.value = '';\n"
    "      passwordValue = '';\n"
    "      setFormBusy(false);\n"
    "      setStatus('Network error while authenticating.', 'error');\n"
    "    };\n"
    "\n"
    "    xhr.send('username=' + encodeURIComponent(userValue) + '&password=' + encodeURIComponent(passwordValue));\n"
    "    return false;\n"
    "  }\n"
    "\n"
    "  if (window.addEventListener) {\n"
    "    window.addEventListener('resize', scheduleDisplayState, false);\n"
    "    window.addEventListener('orientationchange', scheduleDisplayState, false);\n"
    "    document.addEventListener('fullscreenchange', scheduleDisplayState, false);\n"
    "    document.addEventListener('webkitfullscreenchange', scheduleDisplayState, false);\n"
    "  }\n"
    "\n"
    "  loadControlState();\n"
    "  applyControlState();\n"
    "  bindControlAnchors();\n"
    "  updateFullscreenButton();\n"
    "  controlToggle.onclick = toggleControls;\n"
    "  fullscreenButton.onclick = toggleFullscreen;\n"
    "  if (window.addEventListener) {\n"
    "    document.addEventListener('fullscreenchange', updateFullscreenButton, false);\n"
    "    document.addEventListener('webkitfullscreenchange', updateFullscreenButton, false);\n"
    "    hlsVideo.addEventListener('webkitbeginfullscreen', updateFullscreenButton, false);\n"
    "    hlsVideo.addEventListener('webkitendfullscreen', updateFullscreenButton, false);\n"
    "  }\n"
    "  loadPublicStatus();\n"
    "  form.onsubmit = login;\n"
    "  disconnectButton.onclick = function () {\n"
    "    setStatus('Disconnecting viewer; login remains active...', 'working');\n"
    "    stopProtocolWorker();\n"
    "  };\n"
    "  logoutButton.onclick = logoutBrowser;\n"
    "\n"
    "  updateProtocolInfo(null, null);\n"
    "  if (!window.Worker || !window.XMLHttpRequest || !window.JSON || !window.Blob || !objectUrlApi) {\n"
    "    connectButton.disabled = true;\n"
    "    setStatus('This browser is too old for the secure browser connection.', 'error');\n"
    "  } else {\n"
    "    resumeSession(true);\n"
    "  }\n"
    "})();\n";

static const char management_page[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1.0,maximum-scale=1.0\">\n"
    "  <meta name=\"apple-mobile-web-app-capable\" content=\"yes\">\n"
    "  <title>VNC Monitor Management</title>\n"
    "  <style>\n"
    "    html { -webkit-text-size-adjust: 100%; background: #eef2f6; }\n"
    "    body { margin: 0; padding: 0; background: #eef2f6; color: #24313d; font-family: -apple-system, 'Helvetica Neue', Helvetica, Arial, sans-serif; font-size: 16px; }\n"
    "    * { -webkit-box-sizing: border-box; box-sizing: border-box; }\n"
    "    .page { padding: 32px 14px; }\n"
    "    .shell { width: 100%; max-width: 760px; margin: 0 auto; }\n"
    "    .top { margin-bottom: 14px; }\n"
    "    .top a { color: #1769c2; text-decoration: none; font-size: 14px; }\n"
    "    .card { margin-bottom: 16px; overflow: hidden; background: #fff; border: 1px solid #d8e0e7; border-radius: 12px; -webkit-box-shadow: 0 8px 24px rgba(28,45,61,.08); box-shadow: 0 8px 24px rgba(28,45,61,.08); }\n"
    "    .head { padding: 24px 26px 20px; border-bottom: 1px solid #e7ecf0; }\n"
    "    .body { padding: 22px 26px 26px; }\n"
    "    h1 { margin: 0 0 5px; color: #17212b; font-size: 28px; line-height: 34px; }\n"
    "    h2 { margin: 0; color: #17212b; font-size: 20px; line-height: 26px; }\n"
    "    .sub { margin: 0; color: #6c7a86; font-size: 14px; line-height: 20px; }\n"
    "    .field { display: block; margin: 0 0 16px; }\n"
    "    .field span { display: block; margin-bottom: 6px; font-size: 14px; font-weight: 600; }\n"
    "    input { display: block; width: 100%; height: 48px; padding: 9px 12px; border: 1px solid #b9c5cf; border-radius: 8px; background: #fff; font-size: 17px; -webkit-appearance: none; }\n"
    "    button { display: inline-block; min-height: 44px; padding: 0 18px; border: 0; border-radius: 8px; background: #1769c2; color: #fff; font-family: inherit; font-size: 15px; font-weight: 600; line-height: 44px; -webkit-appearance: none; }\n"
    "    button[disabled] { opacity: .55; }\n"
    "    .danger { background: #b64040; }\n"
    "    .secondary { border: 1px solid #b9c5cf; background: #fff; color: #344452; }\n"
    "    .row { padding: 10px 0; border-bottom: 1px solid #edf0f2; }\n"
    "    .row:last-child { border-bottom: 0; }\n"
    "    .key { display: inline-block; width: 38%; color: #71808c; vertical-align: top; }\n"
    "    .value { display: inline-block; width: 60%; color: #26343f; font-weight: 600; word-break: break-all; vertical-align: top; }\n"
    "    .actions { padding-top: 18px; }\n"
    "    .actions button { margin: 0 8px 8px 0; }\n"
    "    .notice { margin-top: 14px; padding: 11px 12px; border: 1px solid #dae3ea; border-radius: 8px; background: #f7f9fb; color: #5d6d79; font-size: 13px; line-height: 19px; }\n"
    "    .error { border-color: #e5c2c2; background: #fff4f4; color: #8b4040; }\n"
    "    .hidden { display: none; }\n"
    "    @media only screen and (max-width: 600px) {\n"
    "      .page { padding: 12px 8px; }\n"
    "      .head { padding: 20px; }\n"
    "      .body { padding: 18px 20px 22px; }\n"
    "      .key, .value { display: block; width: 100%; }\n"
    "      .key { margin-bottom: 3px; }\n"
    "      button { width: 100%; margin-right: 0; }\n"
    "    }\n"
    "  </style>\n"
    "  <script src=\"/manage.js\" defer></script>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"page\"><div class=\"shell\">\n"
    "    <div class=\"top\"><a href=\"/\">&larr; Viewer</a></div>\n"
    "    <div class=\"card\">\n"
    "      <div class=\"head\"><h1>VNC Monitor</h1><p class=\"sub\">Session management and server settings</p></div>\n"
    "      <div id=\"login-panel\" class=\"body\">\n"
    "        <form id=\"manage-login\" autocomplete=\"on\">\n"
    "          <label class=\"field\"><span>Username</span><input id=\"manage-user\" type=\"text\" autocomplete=\"username\" autocapitalize=\"off\" autocorrect=\"off\" required></label>\n"
    "          <label class=\"field\"><span>Password</span><input id=\"manage-password\" type=\"password\" autocomplete=\"current-password\" required></label>\n"
    "          <button id=\"manage-unlock\" type=\"submit\">Unlock management</button>\n"
    "        </form>\n"
    "        <div id=\"manage-message\" class=\"notice\">Authenticate as the currently active local GNOME user. Management does not occupy the viewer slot.</div>\n"
    "      </div>\n"
    "    </div>\n"
    "    <div id=\"dashboard\" class=\"hidden\">\n"
    "      <div class=\"card\"><div class=\"head\"><h2>Current viewer session</h2></div><div class=\"body\">\n"
    "        <div class=\"row\"><span class=\"key\">State</span><span id=\"v-state\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">Transport</span><span id=\"v-transport\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">Peer</span><span id=\"v-peer\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">Local user</span><span id=\"v-user\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">logind session</span><span id=\"v-session\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">WebSocket</span><span id=\"v-ws\" class=\"value\">-</span></div>\n"
    "        <div class=\"actions\"><button id=\"disconnect-viewer\" class=\"danger\" type=\"button\">Disconnect viewer</button></div>\n"
    "      </div></div>\n"
    "      <div class=\"card\"><div class=\"head\"><h2>Active desktop</h2></div><div class=\"body\">\n"
    "        <div class=\"row\"><span class=\"key\">User</span><span id=\"a-user\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">UID</span><span id=\"a-uid\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">logind session</span><span id=\"a-session\" class=\"value\">-</span></div>\n"
    "      </div></div>\n"
    "      <div class=\"card\"><div class=\"head\"><h2>Server settings</h2><p class=\"sub\">HTTPS/WSS settings are stored in /etc/vnc-monitor/web.ini.</p></div><div class=\"body\">\n"
    "        <div class=\"row\"><span class=\"key\">HTTPS endpoint</span><span id=\"s-endpoint\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">Config file</span><span id=\"s-config\" class=\"value\">-</span></div>\n"
    "        <div class=\"row\"><span class=\"key\">VNC port</span><span id=\"s-vnc\" class=\"value\">-</span></div>\n"
    "        <form id=\"settings-form\">\n"
    "          <label class=\"field\"><span>HTTPS port</span><input id=\"settings-port\" type=\"text\" inputmode=\"numeric\" autocomplete=\"off\" required></label>\n"
    "          <label class=\"field\"><span>Certificate chain</span><input id=\"settings-cert\" type=\"text\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\" autocomplete=\"off\" required></label>\n"
    "          <label class=\"field\"><span>Private key</span><input id=\"settings-key\" type=\"text\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\" autocomplete=\"off\" required></label>\n"
    "          <button id=\"settings-save\" type=\"submit\">Save settings and restart broker</button>\n"
    "        </form>\n"
    "        <div id=\"settings-message\" class=\"notice\">The certificate/key pair and new port are validated before saving. The update is atomic. Restarting the broker disconnects any active viewer session.</div>\n"
    "        <div class=\"actions\"><button id=\"reload-settings\" class=\"secondary\" type=\"button\">Reload settings</button><button id=\"manage-logout\" class=\"secondary\" type=\"button\">Log out</button></div>\n"
    "      </div></div>\n"
    "    </div>\n"
    "  </div></div>\n"
    "</body>\n"
    "</html>\n";

static const char management_js[] =
    "(function () {\n"
    "  'use strict';\n"
    "  var loginPanel = document.getElementById('login-panel');\n"
    "  var dashboard = document.getElementById('dashboard');\n"
    "  var form = document.getElementById('manage-login');\n"
    "  var user = document.getElementById('manage-user');\n"
    "  var password = document.getElementById('manage-password');\n"
    "  var unlock = document.getElementById('manage-unlock');\n"
    "  var message = document.getElementById('manage-message');\n"
    "  var settingsForm = document.getElementById('settings-form');\n"
    "  var settingsPort = document.getElementById('settings-port');\n"
    "  var settingsCert = document.getElementById('settings-cert');\n"
    "  var settingsKey = document.getElementById('settings-key');\n"
    "  var settingsSave = document.getElementById('settings-save');\n"
    "  var settingsMessage = document.getElementById('settings-message');\n"
    "  var settingsDirty = false;\n"
    "  var settingsLoaded = false;\n"
    "  var timer = null;\n"
    "  function text(id, value) { var e = document.getElementById(id); e.innerHTML = ''; e.appendChild(document.createTextNode(value == null || value === '' ? '-' : String(value))); }\n"
    "  function safeJson(s) { try { return JSON.parse(s || '{}'); } catch (e) { return {}; } }\n"
    "  function showLogin(msg, error) { dashboard.className = 'hidden'; loginPanel.className = 'body'; if (msg) { message.innerHTML = ''; message.appendChild(document.createTextNode(msg)); message.className = error ? 'notice error' : 'notice'; } }\n"
    "  function showDashboard() { loginPanel.className = 'body hidden'; dashboard.className = ''; }\n"
    "  function stopTimer() { if (timer) { window.clearTimeout(timer); timer = null; } }\n"
    "  function markSettingsDirty() {\n"
    "    if (!settingsLoaded) settingsLoaded = true;\n"
    "    settingsDirty = true;\n"
    "    settingsMessage.className = 'notice';\n"
    "    settingsMessage.innerHTML = 'Unsaved settings. Live session status will continue to refresh without changing these fields.';\n"
    "  }\n"
    "  function request(method, url, body, callback) {\n"
    "    var xhr = new XMLHttpRequest();\n"
    "    xhr.open(method, url, true);\n"
    "    if (method === 'POST') { xhr.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded'); xhr.setRequestHeader('X-VNC-Monitor-Control', '1'); }\n"
    "    xhr.onreadystatechange = function () { if (xhr.readyState === 4) callback(xhr.status, safeJson(xhr.responseText)); };\n"
    "    xhr.onerror = function () { callback(0, {}); };\n"
    "    xhr.send(body || null);\n"
    "  }\n"
    "  function renderStatus(data) {\n"
    "    showDashboard();\n"
    "    text('v-state', data.viewer.state); text('v-transport', data.viewer.transport); text('v-peer', data.viewer.peer); text('v-user', data.viewer.user); text('v-session', data.viewer.session); text('v-ws', data.viewer.websocket ? 'attached' : 'not attached');\n"
    "    text('a-user', data.active.user); text('a-uid', data.active.available ? data.active.uid : '-'); text('a-session', data.active.session);\n"
    "    document.getElementById('disconnect-viewer').disabled = !data.viewer.active;\n"
    "  }\n"
    "  function renderSettings(data) {\n"
    "    text('s-endpoint', window.location.host); text('s-config', data.settings.configFile); text('s-vnc', data.settings.vncPort);\n"
    "    settingsPort.value = String(data.settings.httpsPort); settingsCert.value = data.settings.certificate || ''; settingsKey.value = data.settings.privateKey || '';\n"
    "    settingsDirty = false; settingsLoaded = true;\n"
    "    settingsMessage.className = 'notice'; settingsMessage.innerHTML = 'The certificate/key pair and new port are validated before saving. The update is atomic. Restarting the broker disconnects any active viewer session.';\n"
    "  }\n"
    "  function pollStatus() {\n"
    "    stopTimer();\n"
    "    request('GET', '/api/manage/status', null, function (status, data) {\n"
    "      if (status === 200) { renderStatus(data); timer = window.setTimeout(pollStatus, 2000); }\n"
    "      else if (status === 401) showLogin('Management session is locked.', false);\n"
    "      else showLogin('Could not read management status.', true);\n"
    "    });\n"
    "  }\n"
    "  function loadSettings(force) {\n"
    "    if (settingsDirty && !force) return;\n"
    "    request('GET', '/api/manage/settings', null, function (status, data) {\n"
    "      if (status === 200) renderSettings(data);\n"
    "      else if (status === 401) showLogin('Management session is locked.', false);\n"
    "      else { settingsMessage.className = 'notice error'; settingsMessage.innerHTML = 'Could not load server settings.'; }\n"
    "    });\n"
    "  }\n"
    "  function startDashboard() { loadSettings(false); pollStatus(); }\n"
    "  form.onsubmit = function (event) {\n"
    "    if (event && event.preventDefault) event.preventDefault();\n"
    "    var u = user.value || ''; var p = password.value || '';\n"
    "    if (!u || !p) { showLogin('Enter your username and password.', true); return false; }\n"
    "    unlock.disabled = true; message.className = 'notice'; message.innerHTML = 'Authenticating...';\n"
    "    request('POST', '/api/manage/login', 'username=' + encodeURIComponent(u) + '&password=' + encodeURIComponent(p), function (status) {\n"
    "      password.value = ''; p = ''; unlock.disabled = false;\n"
    "      if (status === 200) startDashboard(); else if (status === 401) showLogin('Authentication failed.', true); else if (status === 409) showLogin('Another management login is already being processed.', true); else showLogin('Management login failed.', true);\n"
    "    });\n"
    "    return false;\n"
    "  };\n"
    "  settingsPort.oninput = markSettingsDirty; settingsPort.onchange = markSettingsDirty;\n"
    "  settingsCert.oninput = markSettingsDirty; settingsCert.onchange = markSettingsDirty;\n"
    "  settingsKey.oninput = markSettingsDirty; settingsKey.onchange = markSettingsDirty;\n"
    "  settingsForm.onsubmit = function (event) {\n"
    "    if (event && event.preventDefault) event.preventDefault();\n"
    "    var port = settingsPort.value || ''; var cert = settingsCert.value || ''; var key = settingsKey.value || '';\n"
    "    if (!port || !cert || !key) { settingsMessage.className = 'notice error'; settingsMessage.innerHTML = 'Fill in HTTPS port, certificate chain and private key.'; return false; }\n"
    "    if (!window.confirm('Save /etc/vnc-monitor/web.ini and restart the broker? Any active viewer session will be disconnected.')) return false;\n"
    "    settingsSave.disabled = true; settingsMessage.className = 'notice'; settingsMessage.innerHTML = 'Validating and saving settings...';\n"
    "    request('POST', '/api/manage/settings', 'port=' + encodeURIComponent(port) + '&certificate=' + encodeURIComponent(cert) + '&privateKey=' + encodeURIComponent(key), function (status, data) {\n"
    "      settingsSave.disabled = false;\n"
    "      if (status === 200 && data.ok) {\n"
    "        stopTimer(); settingsDirty = false; settingsLoaded = false;\n"
    "        settingsMessage.className = 'notice'; settingsMessage.innerHTML = 'Settings saved. Broker is restarting...';\n"
    "        window.setTimeout(function () { var p = String(data.port || port); var suffix = p === '443' ? '' : ':' + p; window.location.href = 'https://' + window.location.hostname + suffix + '/manage'; }, 2500);\n"
    "      } else if (status === 401) { showLogin('Management session expired.', true); }\n"
    "      else { settingsMessage.className = 'notice error'; settingsMessage.innerHTML = data.error ? 'Settings rejected: ' + data.error : 'Settings could not be saved.'; }\n"
    "    });\n"
    "    return false;\n"
    "  };\n"
    "  document.getElementById('reload-settings').onclick = function () {\n"
    "    if (settingsDirty && !window.confirm('Discard unsaved server settings and reload them from /etc/vnc-monitor/web.ini?')) return;\n"
    "    settingsDirty = false; settingsLoaded = false; loadSettings(true);\n"
    "  };\n"
    "  document.getElementById('disconnect-viewer').onclick = function () { if (!window.confirm('Disconnect the current viewer session?')) return; request('POST', '/api/manage/disconnect', '', function (status) { if (status === 200 || status === 409) pollStatus(); else if (status === 401) showLogin('Management session expired.', true); }); };\n"
    "  document.getElementById('manage-logout').onclick = function () { stopTimer(); settingsDirty = false; settingsLoaded = false; request('POST', '/api/manage/logout', '', function () { showLogin('Management session locked.', false); }); };\n"
    "  startDashboard();\n"
    "})();\n";

static gboolean
csp_host_valid(const char *host)
{
    if (!host || !*host || strlen(host) > 255)
        return FALSE;

    for (const unsigned char *p = (const unsigned char *)host; *p; p++) {
        if (g_ascii_isalnum(*p) ||
            *p == '.' || *p == '-' || *p == ':' ||
            *p == '[' || *p == ']')
            continue;
        return FALSE;
    }

    return TRUE;
}

static void
set_security_headers(SoupServerMessage *msg)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
    SoupMessageHeaders *request_headers = soup_server_message_get_request_headers(msg);
    const char *host = soup_message_headers_get_one(request_headers, "Host");

    soup_message_headers_replace(headers, "Cache-Control", "no-store");
    soup_message_headers_replace(headers, "Pragma", "no-cache");
    soup_message_headers_replace(headers, "X-Content-Type-Options", "nosniff");
    soup_message_headers_replace(headers, "Referrer-Policy", "no-referrer");
    soup_message_headers_replace(headers, "X-Frame-Options", "DENY");
    soup_message_headers_replace(headers, "Cross-Origin-Resource-Policy", "same-origin");
    soup_message_headers_replace(headers, "Cross-Origin-Opener-Policy", "same-origin");

    char *csp = NULL;
    if (csp_host_valid(host)) {
        /*
         * Safari/WebKit versions used by legacy iOS do not reliably treat
         * connect-src 'self' as permitting a same-host WSS endpoint. Keep
         * 'self' for XHR and explicitly permit only this request Host for WSS.
         */
        csp = g_strdup_printf(
            "default-src 'none'; style-src 'unsafe-inline'; "
            "script-src 'self'; img-src blob: data:; media-src 'self'; "
            "connect-src 'self' wss://%s; "
            "form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
            host);
    } else {
        csp = g_strdup(
            "default-src 'none'; style-src 'unsafe-inline'; "
            "script-src 'self'; img-src blob:; media-src 'self'; connect-src 'self'; "
            "form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
    }

    soup_message_headers_replace(headers, "Content-Security-Policy", csp);
    g_free(csp);

    soup_message_headers_replace(headers,
                                 "Permissions-Policy",
                                 "camera=(), microphone=(), geolocation=(), usb=()");
}

static void
respond_text(SoupServerMessage *msg,
             guint status,
             const char *content_type,
             const char *body)
{
    if (!body)
        body = "";

    set_security_headers(msg);
    soup_server_message_set_status(msg, status, NULL);
    soup_server_message_set_response(msg,
                                     content_type,
                                     SOUP_MEMORY_COPY,
                                     body,
                                     strlen(body));
}

static void
respond_method_not_allowed(SoupServerMessage *msg, const char *allow)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(headers, "Allow", allow);
    respond_text(msg,
                 SOUP_STATUS_METHOD_NOT_ALLOWED,
                 "text/plain; charset=utf-8",
                 "Method Not Allowed\n");
}

static gboolean
slot_busy(WebServer *web)
{
    return web->hooks.slot_busy ? web->hooks.slot_busy(web->user_data) : FALSE;
}

static const char *
slot_state(WebServer *web)
{
    const char *state = web->hooks.slot_state ?
        web->hooks.slot_state(web->user_data) : NULL;
    return state && *state ? state : "unknown";
}

static gboolean
request_origin_matches(SoupServerMessage *msg, gboolean required)
{
    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *origin = soup_message_headers_get_one(headers, "Origin");
    if (!origin || !*origin)
        return required ? FALSE : TRUE;

    const char *host = soup_message_headers_get_one(headers, "Host");
    if (!host || !*host)
        return FALSE;

    char *expected = g_strdup_printf("https://%s", host);
    gboolean match = g_ascii_strcasecmp(origin, expected) == 0;
    g_free(expected);
    return match;
}

static char *
extract_cookie(SoupServerMessage *msg, const char *name)
{
    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *cookie = soup_message_headers_get_one(headers, "Cookie");
    if (!cookie || !*cookie || !name || !*name)
        return NULL;

    char *prefix = g_strdup_printf("%s=", name);
    char **parts = g_strsplit(cookie, ";", -1);
    char *found = NULL;

    for (char **p = parts; p && *p; p++) {
        char *part = g_strstrip(*p);
        if (!g_str_has_prefix(part, prefix))
            continue;

        if (found) {
            g_free(found);
            found = NULL;
            break;
        }
        found = g_strdup(part + strlen(prefix));
    }

    g_strfreev(parts);
    g_free(prefix);
    return found;
}

static char *
extract_session_cookie(SoupServerMessage *msg)
{
    return extract_cookie(msg, WEB_SESSION_COOKIE);
}

static gboolean
device_id_valid(const char *device_id)
{
    if (!device_id || strlen(device_id) != VNC_BROKER_DEVICE_ID_HEX_LEN)
        return FALSE;

    for (size_t i = 0; i < VNC_BROKER_DEVICE_ID_HEX_LEN; i++) {
        char c = device_id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return FALSE;
    }

    return TRUE;
}

static int
generate_device_id(char out[VNC_BROKER_DEVICE_ID_HEX_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    guint8 random_bytes[VNC_BROKER_DEVICE_ID_HEX_LEN / 2];
    size_t offset = 0;

    while (offset < sizeof(random_bytes)) {
        ssize_t n = getrandom(random_bytes + offset,
                              sizeof(random_bytes) - offset,
                              0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        offset += (size_t)n;
    }

    for (size_t i = 0; i < sizeof(random_bytes); i++) {
        out[i * 2] = hex[random_bytes[i] >> 4];
        out[i * 2 + 1] = hex[random_bytes[i] & 0x0f];
    }
    out[VNC_BROKER_DEVICE_ID_HEX_LEN] = '\0';
    return 0;
}

static void
append_device_cookie(SoupServerMessage *msg, const char *device_id)
{
    if (!msg || !device_id_valid(device_id))
        return;

    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
    char *cookie = g_strdup_printf(
        WEB_DEVICE_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=%d",
        device_id,
        WEB_DEVICE_MAX_AGE_S);
    soup_message_headers_append(headers, "Set-Cookie", cookie);
    g_free(cookie);
}

static void
set_session_cookie(SoupServerMessage *msg, const char *token)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);

    if (!token || !*token) {
        soup_message_headers_replace(
            headers,
            "Set-Cookie",
            WEB_SESSION_COOKIE "=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
        return;
    }

    char *cookie = g_strdup_printf(
        WEB_SESSION_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=%d",
        token,
        WEB_SESSION_MAX_AGE_S);
    soup_message_headers_replace(headers, "Set-Cookie", cookie);
    g_free(cookie);
}

static void
set_management_cookie(SoupServerMessage *msg, const char *token)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);
    if (!token || !*token) {
        soup_message_headers_replace(headers,
                                     "Set-Cookie",
                                     WEB_MANAGEMENT_COOKIE "=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0");
        return;
    }

    char *cookie = g_strdup_printf(
        WEB_MANAGEMENT_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=%d",
        token,
        WEB_MANAGEMENT_MAX_AGE_S);
    soup_message_headers_replace(headers, "Set-Cookie", cookie);
    g_free(cookie);
}

static gboolean
management_request_authenticated(WebServer *web, SoupServerMessage *msg)
{
    if (!web || !web->hooks.validate_management_token)
        return FALSE;

    char *token = extract_session_cookie(msg);
    gboolean ok = token &&
                  web->hooks.validate_management_token(token, web->user_data);
    g_free(token);

    if (ok)
        return TRUE;

    /* Transitional fallback for a pre-unification management cookie. */
    token = extract_cookie(msg, WEB_MANAGEMENT_COOKIE);
    ok = token &&
         web->hooks.validate_management_token(token, web->user_data);
    g_free(token);
    return ok;
}

static gboolean
management_control_request_allowed(SoupServerMessage *msg)
{
    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *control = soup_message_headers_get_one(headers, WEB_CONTROL_HEADER);
    return control && strcmp(control, "1") == 0 &&
           request_origin_matches(msg, FALSE);
}

static char *
json_escape(const char *value)
{
    if (!value)
        return g_strdup("");

    GString *out = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
            case '"': g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:
                if (*p < 0x20)
                    g_string_append_printf(out, "\\u%04x", (unsigned)*p);
                else
                    g_string_append_c(out, (char)*p);
        }
    }
    return g_string_free(out, FALSE);
}

static gboolean
settings_path_valid(const char *path)
{
    if (!path || !*path || !g_path_is_absolute(path) || strlen(path) >= PATH_MAX)
        return FALSE;

    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 0x20 || *p == 0x7f)
            return FALSE;
    }

    return TRUE;
}

static gboolean
settings_port_available(guint port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return FALSE;

    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    gboolean ok = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

static gboolean
save_web_settings(WebServer *web,
                  guint port,
                  const char *certificate_file,
                  const char *private_key_file,
                  GError **error)
{
    char *contents = g_strdup_printf(
        "# VNC Monitor browser/WebRTC broker configuration\n"
        "# Managed through /manage or edited manually.\n\n"
        "[web]\n"
        "enabled=true\n"
        "port=%u\n"
        "certificate=%s\n"
        "private-key=%s\n",
        port,
        certificate_file,
        private_key_file);

    gboolean ok = g_file_set_contents_full(
        web->config_file,
        contents,
        -1,
        G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
        0644,
        error);

    g_free(contents);
    return ok;
}

static gboolean
hls_segment_name_valid(const char *name)
{
    if (!name || !g_str_has_prefix(name, "segment") ||
        !g_str_has_suffix(name, ".ts"))
        return FALSE;

    size_t len = strlen(name);
    const char *digits = name + strlen("segment");
    const char *end = name + len - strlen(".ts");
    if (digits >= end)
        return FALSE;

    for (const char *p = digits; p < end; p++) {
        if (!g_ascii_isdigit(*p))
            return FALSE;
    }
    return TRUE;
}

static gboolean
media_request_authenticated(WebServer *web, SoupServerMessage *msg)
{
    char *token = extract_session_cookie(msg);
    gboolean ok = token &&
                  web->hooks.validate_media_token &&
                  web->hooks.validate_media_token(token, web->user_data);
    g_free(token);
    return ok;
}

static gboolean
parse_single_byte_range(const char *header,
                        gsize total,
                        gsize *start_out,
                        gsize *end_out)
{
    if (!header || !*header || !start_out || !end_out || total == 0)
        return FALSE;
    if (!g_str_has_prefix(header, "bytes=") || strchr(header, ','))
        return FALSE;

    char *spec = g_strdup(header + 6);
    char *dash = strchr(spec, '-');
    if (!dash || strchr(dash + 1, '-')) {
        g_free(spec);
        return FALSE;
    }

    *dash = '\0';
    char *left = spec;
    char *right = dash + 1;
    guint64 start = 0, end = 0;
    gboolean ok = FALSE;

    if (!*left) {
        char *tail = NULL;
        errno = 0;
        guint64 suffix = g_ascii_strtoull(right, &tail, 10);
        if (errno == 0 && tail && *tail == '\0' && suffix > 0) {
            start = suffix >= total ? 0 : (guint64)total - suffix;
            end = (guint64)total - 1;
            ok = TRUE;
        }
    } else {
        char *tail = NULL;
        errno = 0;
        start = g_ascii_strtoull(left, &tail, 10);
        if (errno == 0 && tail && *tail == '\0' && start < total) {
            if (!*right) {
                end = (guint64)total - 1;
                ok = TRUE;
            } else {
                errno = 0;
                end = g_ascii_strtoull(right, &tail, 10);
                if (errno == 0 && tail && *tail == '\0' && end >= start) {
                    if (end >= total)
                        end = (guint64)total - 1;
                    ok = TRUE;
                }
            }
        }
    }

    if (ok) {
        *start_out = (gsize)start;
        *end_out = (gsize)end;
    }
    g_free(spec);
    return ok;
}

static void
hls_handler(SoupServer *server,
            SoupServerMessage *msg,
            const char *path,
            GHashTable *query,
            gpointer user_data)
{
    (void)server;
    (void)query;
    WebServer *web = user_data;
    const char *method = soup_server_message_get_method(msg);
    gboolean head = strcmp(method, "HEAD") == 0;

    if (strcmp(method, "GET") != 0 && !head) {
        respond_method_not_allowed(msg, "GET, HEAD");
        return;
    }

    if (!media_request_authenticated(web, msg)) {
        LOG_INFO("HLS %s path=%s status=401", method, path ? path : "(null)");
        respond_text(msg, SOUP_STATUS_UNAUTHORIZED,
                     "text/plain; charset=utf-8",
                     "Authentication required\n");
        return;
    }

    if (!web->hls_root || !*web->hls_root) {
        respond_text(msg, SOUP_STATUS_SERVICE_UNAVAILABLE,
                     "text/plain; charset=utf-8",
                     "HLS media not ready\n");
        return;
    }

    const char *name = NULL;
    const char *content_type = NULL;
    gsize max_size = 0;

    if (strcmp(path, "/live/index.m3u8") == 0) {
        name = "index.m3u8";
        content_type = "application/vnd.apple.mpegurl";
        max_size = WEB_HLS_PLAYLIST_MAX;
    } else if (g_str_has_prefix(path, "/live/") &&
               hls_segment_name_valid(path + strlen("/live/"))) {
        name = path + strlen("/live/");
        content_type = "video/mp2t";
        max_size = WEB_HLS_SEGMENT_MAX;
    } else {
        respond_text(msg, SOUP_STATUS_NOT_FOUND,
                     "text/plain; charset=utf-8",
                     "Not Found\n");
        return;
    }

    char *file_path = g_build_filename(web->hls_root, name, NULL);
    GStatBuf st;
    if (g_stat(file_path, &st) < 0 || st.st_size <= 0 ||
        (guint64)st.st_size > max_size) {
        g_free(file_path);
        respond_text(msg, SOUP_STATUS_NOT_FOUND,
                     "text/plain; charset=utf-8",
                     "Not Found\n");
        return;
    }

    gsize total = (gsize)st.st_size;
    SoupMessageHeaders *request_headers =
        soup_server_message_get_request_headers(msg);
    SoupMessageHeaders *response_headers =
        soup_server_message_get_response_headers(msg);
    const char *range_header =
        soup_message_headers_get_one(request_headers, "Range");

    gsize range_start = 0, range_end = total - 1;
    gboolean ranged = range_header && *range_header;
    if (ranged && !parse_single_byte_range(range_header, total,
                                            &range_start, &range_end)) {
        char *value = g_strdup_printf("bytes */%zu", total);
        soup_message_headers_replace(response_headers, "Accept-Ranges", "bytes");
        soup_message_headers_replace(response_headers, "Content-Range", value);
        g_free(value);
        LOG_INFO("HLS %s path=%s range=invalid status=416 bytes=0",
                 method, path);
        respond_text(msg, 416, "text/plain; charset=utf-8",
                     "Range Not Satisfiable\n");
        g_free(file_path);
        return;
    }

    gsize send_len = range_end - range_start + 1;
    guint status = ranged ? 206 : SOUP_STATUS_OK;

    set_security_headers(msg);
    soup_message_headers_replace(response_headers, "Accept-Ranges", "bytes");
    soup_message_headers_replace(response_headers, "Content-Type", content_type);

    if (ranged) {
        char *value = g_strdup_printf("bytes %zu-%zu/%zu",
                                      range_start, range_end, total);
        soup_message_headers_replace(response_headers, "Content-Range", value);
        g_free(value);
    }

    if (head) {
        char *length_value = g_strdup_printf("%zu", send_len);
        soup_message_headers_replace(response_headers, "Content-Length", length_value);
        g_free(length_value);
        soup_server_message_set_status(msg, status, NULL);
        LOG_DEBUG("HLS HEAD path=%s range=%s status=%u bytes=%zu",
                 path, ranged ? "partial" : "full", status, send_len);
        g_free(file_path);
        return;
    }

    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(file_path, &contents, &length, &error) ||
        length != total || length > max_size) {
        LOG_DEBUG("Could not read HLS media file %s: %s",
                  file_path,
                  error ? error->message : "size changed");
        g_clear_error(&error);
        g_free(contents);
        g_free(file_path);
        respond_text(msg, SOUP_STATUS_NOT_FOUND,
                     "text/plain; charset=utf-8",
                     "Not Found\n");
        return;
    }
    g_free(file_path);

    if (!ranged) {
        soup_server_message_set_status(msg, status, NULL);
        soup_server_message_set_response(msg, content_type,
                                         SOUP_MEMORY_TAKE,
                                         contents, length);
        LOG_DEBUG("HLS GET path=%s range=full status=%u bytes=%zu",
                 path, status, length);
        return;
    }

    guint8 *partial = g_malloc(send_len);
    memcpy(partial, (guint8 *)contents + range_start, send_len);
    g_free(contents);

    soup_server_message_set_status(msg, status, NULL);
    soup_server_message_set_response(msg, content_type,
                                     SOUP_MEMORY_TAKE,
                                     partial, send_len);
    LOG_DEBUG("HLS GET path=%s range=%zu-%zu status=%u bytes=%zu",
             path, range_start, range_end, status, send_len);
}
static void
root_handler(SoupServer *server,
             SoupServerMessage *msg,
             const char *path,
             GHashTable *query,
             gpointer user_data)
{
    (void)server;
    (void)query;
    (void)user_data;

    if (strcmp(path, "/") != 0) {
        respond_text(msg,
                     SOUP_STATUS_NOT_FOUND,
                     "text/plain; charset=utf-8",
                     "Not Found\n");
        return;
    }

    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "text/html; charset=utf-8",
                 login_page);
}

static void
protocol_worker_js_handler(SoupServer *server,
                           SoupServerMessage *msg,
                           const char *path,
                           GHashTable *query,
                           gpointer user_data)
{
    (void)server;
    (void)path;
    (void)query;
    (void)user_data;

    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "text/javascript; charset=utf-8",
                 protocol_worker_js);
}

static void
client_js_handler(SoupServer *server,
                  SoupServerMessage *msg,
                  const char *path,
                  GHashTable *query,
                  gpointer user_data)
{
    (void)server;
    (void)path;
    (void)query;
    (void)user_data;

    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "text/javascript; charset=utf-8",
                 client_js);
}

static void
management_page_handler(SoupServer *server,
                        SoupServerMessage *msg,
                        const char *path,
                        GHashTable *query,
                        gpointer user_data)
{
    (void)server; (void)path; (void)query; (void)user_data;
    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }
    respond_text(msg, SOUP_STATUS_OK, "text/html; charset=utf-8", management_page);
}

static void
management_js_handler(SoupServer *server,
                      SoupServerMessage *msg,
                      const char *path,
                      GHashTable *query,
                      gpointer user_data)
{
    (void)server; (void)path; (void)query; (void)user_data;
    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }
    respond_text(msg, SOUP_STATUS_OK, "text/javascript; charset=utf-8", management_js);
}

static void
status_handler(SoupServer *server,
               SoupServerMessage *msg,
               const char *path,
               GHashTable *query,
               gpointer user_data)
{
    (void)server;
    (void)path;
    (void)query;

    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }

    WebServerManagementInfo info;
    memset(&info, 0, sizeof(info));
    g_strlcpy(info.viewer_state, slot_state(web), sizeof(info.viewer_state));
    g_strlcpy(info.viewer_transport, "none", sizeof(info.viewer_transport));
    if (web->hooks.get_management_info)
        (void)web->hooks.get_management_info(&info, web->user_data);

    gint64 uptime_us = g_get_monotonic_time() - web->started_us;
    if (uptime_us < 0)
        uptime_us = 0;

    char *state = json_escape(info.viewer_state);
    char *transport = json_escape(info.viewer_transport);
    char *body = g_strdup_printf(
        "{\"service\":\"ready\",\"build\":\"%s\",\"protocol\":%u,"
        "\"uptimeMs\":%" G_GINT64_FORMAT ",\"busy\":%s,\"slot\":\"%s\","
        "\"viewer\":{\"websocket\":%s,\"protocolReady\":%s,"
        "\"frameInFlight\":%s,\"transport\":\"%s\"},"
        "\"telemetry\":{\"framesForwarded\":%" G_GUINT64_FORMAT ","
        "\"framesAcked\":%" G_GUINT64_FORMAT ","
        "\"framesNacked\":%" G_GUINT64_FORMAT "}}\n",
        VNC_MONITOR_VERSION,
        VNC_WEB_PROTOCOL_VERSION,
        uptime_us / 1000,
        slot_busy(web) ? "true" : "false",
        state,
        info.websocket_attached ? "true" : "false",
        info.web_protocol_ready ? "true" : "false",
        info.web_frame_in_flight ? "true" : "false",
        transport,
        info.web_frames_forwarded,
        info.web_frames_acked,
        info.web_frames_nacked);

    SoupMessageHeaders *response_headers = soup_server_message_get_response_headers(msg);
    soup_message_headers_replace(response_headers, "Cache-Control", "no-store, max-age=0");

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "application/json; charset=utf-8",
                 body);
    g_free(body);
    g_free(state);
    g_free(transport);
}

typedef struct {
    SoupServerMessage *msg;
    char device_id[VNC_BROKER_DEVICE_ID_HEX_LEN + 1];
    gboolean set_device_cookie;
} PendingLogin;

static void
secure_clear_string(char *value)
{
    if (!value)
        return;

    size_t len = strlen(value);
#if defined(__GLIBC__)
    explicit_bzero(value, len);
#else
    volatile unsigned char *p = (volatile unsigned char *)value;
    while (len--)
        *p++ = 0;
#endif
}

static void
respond_auth_result(SoupServerMessage *msg,
                    WebServerAuthResult result,
                    const char *session_token)
{
    switch (result) {
        case WEB_SERVER_AUTH_OK:
            if (!session_token || !*session_token) {
                set_session_cookie(msg, NULL);
                respond_text(msg,
                             SOUP_STATUS_INTERNAL_SERVER_ERROR,
                             "application/json; charset=utf-8",
                             "{\"error\":\"authentication-error\"}\n");
                break;
            }
            set_session_cookie(msg, session_token);
            respond_text(msg,
                         SOUP_STATUS_OK,
                         "application/json; charset=utf-8",
                         "{\"ok\":true,\"state\":\"active-webrtc\","
                         "\"attach-timeout-ms\":15000}\n");
            break;
        case WEB_SERVER_AUTH_DENIED:
            set_session_cookie(msg, NULL);
            respond_text(msg,
                         SOUP_STATUS_UNAUTHORIZED,
                         "application/json; charset=utf-8",
                         "{\"error\":\"authentication-failed\"}\n");
            break;
        case WEB_SERVER_AUTH_BUSY:
            respond_text(msg,
                         SOUP_STATUS_CONFLICT,
                         "application/json; charset=utf-8",
                         "{\"error\":\"busy\"}\n");
            break;
        case WEB_SERVER_AUTH_UNAVAILABLE:
            set_session_cookie(msg, NULL);
            respond_text(msg,
                         SOUP_STATUS_SERVICE_UNAVAILABLE,
                         "application/json; charset=utf-8",
                         "{\"error\":\"unavailable\"}\n");
            break;
        case WEB_SERVER_AUTH_ERROR:
        case WEB_SERVER_AUTH_STARTED:
        default:
            set_session_cookie(msg, NULL);
            respond_text(msg,
                         SOUP_STATUS_INTERNAL_SERVER_ERROR,
                         "application/json; charset=utf-8",
                         "{\"error\":\"authentication-error\"}\n");
            break;
    }
}

static void
login_auth_complete(WebServerAuthResult result,
                    const char *session_token,
                    gpointer completion_data)
{
    PendingLogin *pending = completion_data;
    if (!pending)
        return;

    respond_auth_result(pending->msg, result, session_token);
    if (result == WEB_SERVER_AUTH_OK && pending->set_device_cookie)
        append_device_cookie(pending->msg, pending->device_id);
    soup_server_message_unpause(pending->msg);
    g_object_unref(pending->msg);
    g_free(pending);
}

static void
login_handler(SoupServer *server,
              SoupServerMessage *msg,
              const char *path,
              GHashTable *query,
              gpointer user_data)
{
    (void)server;
    (void)path;
    (void)query;

    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "POST") != 0) {
        respond_method_not_allowed(msg, "POST");
        return;
    }

    if (!request_origin_matches(msg, FALSE)) {
        respond_text(msg,
                     SOUP_STATUS_FORBIDDEN,
                     "application/json; charset=utf-8",
                     "{\"error\":\"origin-rejected\"}\n");
        return;
    }

    if (!web->hooks.begin_auth) {
        respond_auth_result(msg, WEB_SERVER_AUTH_UNAVAILABLE, NULL);
        return;
    }

    if (slot_busy(web)) {
        respond_auth_result(msg, WEB_SERVER_AUTH_BUSY, NULL);
        return;
    }

    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *content_type = soup_message_headers_get_content_type(headers, NULL);
    if (!content_type ||
        g_ascii_strcasecmp(content_type, "application/x-www-form-urlencoded") != 0) {
        respond_text(msg,
                     SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE,
                     "application/json; charset=utf-8",
                     "{\"error\":\"invalid-request\"}\n");
        return;
    }

    SoupMessageBody *request_body = soup_server_message_get_request_body(msg);
    if (!request_body || !request_body->data ||
        request_body->length <= 0 ||
        request_body->length > WEB_LOGIN_BODY_MAX) {
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"invalid-request\"}\n");
        return;
    }

    GHashTable *form = soup_form_decode((const char *)request_body->data);
    if (!form) {
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"invalid-request\"}\n");
        return;
    }

    char *username = g_hash_table_lookup(form, "username");
    char *password = g_hash_table_lookup(form, "password");
    size_t username_len = username ? strlen(username) : 0;
    size_t password_len = password ? strlen(password) : 0;

    if (!username || !password ||
        username_len == 0 || password_len == 0 ||
        username_len > VNC_BROKER_AUTH_USERNAME_MAX ||
        password_len > VNC_BROKER_AUTH_PASSWORD_MAX) {
        if (password)
            secure_clear_string(password);
        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"invalid-request\"}\n");
        return;
    }

    const char *peer_addr = soup_server_message_get_remote_host(msg);
    if (!peer_addr || !*peer_addr)
        peer_addr = "unknown";

    PendingLogin *pending = g_new0(PendingLogin, 1);
    pending->msg = g_object_ref(msg);

    char *device_cookie = extract_cookie(msg, WEB_DEVICE_COOKIE);
    if (device_cookie && device_id_valid(device_cookie)) {
        g_strlcpy(pending->device_id,
                  device_cookie,
                  sizeof(pending->device_id));
    }
    else {
        if (generate_device_id(pending->device_id) < 0) {
            g_free(device_cookie);
            g_object_unref(pending->msg);
            g_free(pending);
            secure_clear_string(password);
            g_hash_table_destroy(form);
            soup_message_body_truncate(request_body);
            respond_text(msg,
                         SOUP_STATUS_INTERNAL_SERVER_ERROR,
                         "application/json; charset=utf-8",
                         "{\"error\":\"device-id-unavailable\"}\n");
            return;
        }
        pending->set_device_cookie = TRUE;
    }
    g_free(device_cookie);

    soup_server_message_pause(msg);

    WebServerAuthResult start = web->hooks.begin_auth(username,
                                                       password,
                                                       peer_addr,
                                                       pending->device_id,
                                                       login_auth_complete,
                                                       pending,
                                                       web->user_data);

    secure_clear_string(password);
    g_hash_table_destroy(form);
    soup_message_body_truncate(request_body);

    if (start != WEB_SERVER_AUTH_STARTED)
        login_auth_complete(start, NULL, pending);
}

static void
resume_handler(SoupServer *server,
               SoupServerMessage *msg,
               const char *path,
               GHashTable *query,
               gpointer user_data)
{
    (void)server; (void)path; (void)query;
    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "POST") != 0) {
        respond_method_not_allowed(msg, "POST");
        return;
    }
    if (!management_control_request_allowed(msg) || !web->hooks.begin_resume) {
        respond_text(msg, SOUP_STATUS_FORBIDDEN,
                     "application/json; charset=utf-8",
                     "{\"error\":\"request-rejected\"}\n");
        return;
    }

    char *token = extract_session_cookie(msg);
    if (!token || !*token) {
        g_free(token);
        set_session_cookie(msg, NULL);
        respond_text(msg, SOUP_STATUS_UNAUTHORIZED,
                     "application/json; charset=utf-8",
                     "{\"error\":\"authentication-required\"}\n");
        return;
    }

    if (slot_busy(web)) {
        g_free(token);
        respond_auth_result(msg, WEB_SERVER_AUTH_BUSY, NULL);
        return;
    }

    const char *peer_addr = soup_server_message_get_remote_host(msg);
    if (!peer_addr || !*peer_addr)
        peer_addr = "unknown";

    PendingLogin *pending = g_new0(PendingLogin, 1);
    pending->msg = g_object_ref(msg);

    char *device_cookie = extract_cookie(msg, WEB_DEVICE_COOKIE);
    if (device_cookie && device_id_valid(device_cookie)) {
        g_strlcpy(pending->device_id,
                  device_cookie,
                  sizeof(pending->device_id));
    }
    else {
        if (generate_device_id(pending->device_id) < 0) {
            g_free(device_cookie);
            g_free(token);
            g_object_unref(pending->msg);
            g_free(pending);
            respond_text(msg, SOUP_STATUS_INTERNAL_SERVER_ERROR,
                         "application/json; charset=utf-8",
                         "{\"error\":\"device-id-unavailable\"}\n");
            return;
        }
        pending->set_device_cookie = TRUE;
    }
    g_free(device_cookie);

    soup_server_message_pause(msg);
    WebServerAuthResult start =
        web->hooks.begin_resume(token,
                                peer_addr,
                                pending->device_id,
                                login_auth_complete,
                                pending,
                                web->user_data);
    g_free(token);

    if (start != WEB_SERVER_AUTH_STARTED)
        login_auth_complete(start, NULL, pending);
}

static void
browser_logout_handler(SoupServer *server,
                       SoupServerMessage *msg,
                       const char *path,
                       GHashTable *query,
                       gpointer user_data)
{
    (void)server; (void)path; (void)query;
    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "POST") != 0) {
        respond_method_not_allowed(msg, "POST");
        return;
    }
    if (!management_control_request_allowed(msg)) {
        respond_text(msg, SOUP_STATUS_FORBIDDEN,
                     "application/json; charset=utf-8",
                     "{\"error\":\"request-rejected\"}\n");
        return;
    }

    if (web->hooks.browser_logout)
        web->hooks.browser_logout(web->user_data);

    set_session_cookie(msg, NULL);
    set_management_cookie(msg, NULL);
    respond_text(msg, SOUP_STATUS_OK,
                 "application/json; charset=utf-8",
                 "{\"ok\":true}\n");
}

static void
management_auth_complete(WebServerAuthResult result,
                         const char *session_token,
                         gpointer completion_data)
{
    PendingLogin *pending = completion_data;
    if (!pending)
        return;

    if (result == WEB_SERVER_AUTH_OK && session_token && *session_token) {
        set_session_cookie(pending->msg, session_token);
        set_management_cookie(pending->msg, session_token);
        respond_text(pending->msg, SOUP_STATUS_OK, "application/json; charset=utf-8", "{\"ok\":true}\n");
    }
    else if (result == WEB_SERVER_AUTH_DENIED) {
        set_management_cookie(pending->msg, NULL);
        respond_text(pending->msg, SOUP_STATUS_UNAUTHORIZED, "application/json; charset=utf-8", "{\"error\":\"authentication-failed\"}\n");
    }
    else if (result == WEB_SERVER_AUTH_BUSY) {
        respond_text(pending->msg, SOUP_STATUS_CONFLICT, "application/json; charset=utf-8", "{\"error\":\"busy\"}\n");
    }
    else if (result == WEB_SERVER_AUTH_UNAVAILABLE) {
        respond_text(pending->msg, SOUP_STATUS_SERVICE_UNAVAILABLE, "application/json; charset=utf-8", "{\"error\":\"unavailable\"}\n");
    }
    else {
        respond_text(pending->msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, "application/json; charset=utf-8", "{\"error\":\"authentication-error\"}\n");
    }

    soup_server_message_unpause(pending->msg);
    g_object_unref(pending->msg);
    g_free(pending);
}

static void
management_login_handler(SoupServer *server,
                         SoupServerMessage *msg,
                         const char *path,
                         GHashTable *query,
                         gpointer user_data)
{
    (void)server; (void)path; (void)query;
    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "POST") != 0) {
        respond_method_not_allowed(msg, "POST");
        return;
    }
    if (!management_control_request_allowed(msg) || !web->hooks.begin_management_auth) {
        respond_text(msg, SOUP_STATUS_FORBIDDEN, "application/json; charset=utf-8", "{\"error\":\"request-rejected\"}\n");
        return;
    }

    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *content_type = soup_message_headers_get_content_type(headers, NULL);
    SoupMessageBody *request_body = soup_server_message_get_request_body(msg);
    if (!content_type ||
        g_ascii_strcasecmp(content_type, "application/x-www-form-urlencoded") != 0 ||
        !request_body || !request_body->data ||
        request_body->length <= 0 || request_body->length > WEB_LOGIN_BODY_MAX) {
        respond_text(msg, SOUP_STATUS_BAD_REQUEST, "application/json; charset=utf-8", "{\"error\":\"invalid-request\"}\n");
        return;
    }

    GHashTable *form = soup_form_decode((const char *)request_body->data);
    if (!form) {
        respond_text(msg, SOUP_STATUS_BAD_REQUEST, "application/json; charset=utf-8", "{\"error\":\"invalid-request\"}\n");
        return;
    }

    char *username = g_hash_table_lookup(form, "username");
    char *password = g_hash_table_lookup(form, "password");
    size_t username_len = username ? strlen(username) : 0;
    size_t password_len = password ? strlen(password) : 0;
    if (!username || !password ||
        username_len == 0 || password_len == 0 ||
        username_len > VNC_BROKER_AUTH_USERNAME_MAX ||
        password_len > VNC_BROKER_AUTH_PASSWORD_MAX) {
        if (password)
            secure_clear_string(password);
        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg, SOUP_STATUS_BAD_REQUEST, "application/json; charset=utf-8", "{\"error\":\"invalid-request\"}\n");
        return;
    }

    const char *peer_addr = soup_server_message_get_remote_host(msg);
    if (!peer_addr || !*peer_addr)
        peer_addr = "unknown";

    PendingLogin *pending = g_new0(PendingLogin, 1);
    pending->msg = g_object_ref(msg);
    soup_server_message_pause(msg);

    WebServerAuthResult start =
        web->hooks.begin_management_auth(username,
                                         password,
                                         peer_addr,
                                         management_auth_complete,
                                         pending,
                                         web->user_data);

    secure_clear_string(password);
    g_hash_table_destroy(form);
    soup_message_body_truncate(request_body);

    if (start != WEB_SERVER_AUTH_STARTED)
        management_auth_complete(start, NULL, pending);
}

static void
management_status_handler(SoupServer *server,
                          SoupServerMessage *msg,
                          const char *path,
                          GHashTable *query,
                          gpointer user_data)
{
    (void)server; (void)path; (void)query;
    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }
    if (!management_request_authenticated(web, msg)) {
        respond_text(msg,
                     SOUP_STATUS_UNAUTHORIZED,
                     "application/json; charset=utf-8",
                     "{\"error\":\"authentication-required\"}\n");
        return;
    }

    WebServerManagementInfo info;
    if (!web->hooks.get_management_info ||
        web->hooks.get_management_info(&info, web->user_data) < 0) {
        respond_text(msg,
                     SOUP_STATUS_INTERNAL_SERVER_ERROR,
                     "application/json; charset=utf-8",
                     "{\"error\":\"status-unavailable\"}\n");
        return;
    }

    char *viewer_state = json_escape(info.viewer_state);
    char *viewer_transport = json_escape(info.viewer_transport);
    char *viewer_peer = json_escape(info.viewer_peer);
    char *viewer_user = json_escape(info.viewer_user);
    char *viewer_session = json_escape(info.viewer_session_id);
    char *active_user = json_escape(info.active_user);
    char *active_session = json_escape(info.active_session_id);

    char *body = g_strdup_printf(
        "{\"viewer\":{\"active\":%s,\"state\":\"%s\",\"transport\":\"%s\","
        "\"peer\":\"%s\",\"user\":\"%s\",\"session\":\"%s\",\"websocket\":%s},"
        "\"active\":{\"available\":%s,\"uid\":%u,\"user\":\"%s\",\"session\":\"%s\"}}\n",
        info.viewer_active ? "true" : "false",
        viewer_state,
        viewer_transport,
        viewer_peer,
        viewer_user,
        viewer_session,
        info.websocket_attached ? "true" : "false",
        info.active_user_available ? "true" : "false",
        info.active_uid,
        active_user,
        active_session);

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "application/json; charset=utf-8",
                 body);

    g_free(body);
    g_free(viewer_state);
    g_free(viewer_transport);
    g_free(viewer_peer);
    g_free(viewer_user);
    g_free(viewer_session);
    g_free(active_user);
    g_free(active_session);
}

static void
management_settings_handler(SoupServer *server,
                            SoupServerMessage *msg,
                            const char *path,
                            GHashTable *query,
                            gpointer user_data)
{
    (void)server; (void)path; (void)query;
    WebServer *web = user_data;

    const char *method = soup_server_message_get_method(msg);

    if (strcmp(method, "GET") == 0) {
        if (!management_request_authenticated(web, msg)) {
            respond_text(msg,
                         SOUP_STATUS_UNAUTHORIZED,
                         "application/json; charset=utf-8",
                         "{\"error\":\"authentication-required\"}\n");
            return;
        }

        WebServerManagementInfo info;
        if (!web->hooks.get_management_info ||
            web->hooks.get_management_info(&info, web->user_data) < 0) {
            respond_text(msg,
                         SOUP_STATUS_INTERNAL_SERVER_ERROR,
                         "application/json; charset=utf-8",
                         "{\"error\":\"settings-unavailable\"}\n");
            return;
        }

        char *config_file = json_escape(web->config_file);
        char *certificate = json_escape(web->certificate_file);
        char *private_key = json_escape(web->private_key_file);

        char *body = g_strdup_printf(
            "{\"settings\":{\"httpsPort\":%u,\"vncPort\":%d,"
            "\"configFile\":\"%s\",\"certificate\":\"%s\","
            "\"privateKey\":\"%s\"}}\n",
            web->port,
            info.vnc_port,
            config_file,
            certificate,
            private_key);

        respond_text(msg,
                     SOUP_STATUS_OK,
                     "application/json; charset=utf-8",
                     body);

        g_free(body);
        g_free(config_file);
        g_free(certificate);
        g_free(private_key);
        return;
    }

    if (strcmp(method, "POST") != 0) {
        respond_method_not_allowed(msg, "GET, POST");
        return;
    }

    if (!management_control_request_allowed(msg) ||
        !management_request_authenticated(web, msg)) {
        respond_text(msg,
                     SOUP_STATUS_UNAUTHORIZED,
                     "application/json; charset=utf-8",
                     "{\"error\":\"authentication-required\"}\n");
        return;
    }

    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *content_type = soup_message_headers_get_content_type(headers, NULL);
    SoupMessageBody *request_body = soup_server_message_get_request_body(msg);

    if (!content_type ||
        g_ascii_strcasecmp(content_type, "application/x-www-form-urlencoded") != 0 ||
        !request_body || !request_body->data ||
        request_body->length <= 0 || request_body->length > WEB_SETTINGS_BODY_MAX) {
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"invalid-request\"}\n");
        return;
    }

    GHashTable *form = soup_form_decode((const char *)request_body->data);
    if (!form) {
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"invalid-request\"}\n");
        return;
    }

    const char *port_text = g_hash_table_lookup(form, "port");
    const char *certificate_file = g_hash_table_lookup(form, "certificate");
    const char *private_key_file = g_hash_table_lookup(form, "privateKey");

    char *end = NULL;
    errno = 0;
    long parsed_port = port_text ? strtol(port_text, &end, 10) : 0;

    if (!port_text || errno != 0 || !end || *end != '\0' ||
        parsed_port < 1 || parsed_port > 65535 ||
        !settings_path_valid(certificate_file) ||
        !settings_path_valid(private_key_file)) {
        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"invalid-settings\"}\n");
        return;
    }

    WebServerManagementInfo info;
    if (!web->hooks.get_management_info ||
        web->hooks.get_management_info(&info, web->user_data) < 0) {
        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg,
                     SOUP_STATUS_INTERNAL_SERVER_ERROR,
                     "application/json; charset=utf-8",
                     "{\"error\":\"status-unavailable\"}\n");
        return;
    }

    guint port = (guint)parsed_port;

    if ((int)port == info.vnc_port) {
        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg,
                     SOUP_STATUS_CONFLICT,
                     "application/json; charset=utf-8",
                     "{\"error\":\"port-conflicts-with-vnc\"}\n");
        return;
    }

    if (port != web->port && !settings_port_available(port)) {
        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg,
                     SOUP_STATUS_CONFLICT,
                     "application/json; charset=utf-8",
                     "{\"error\":\"https-port-unavailable\"}\n");
        return;
    }

    char tls_reason[64] = {0};
    if (tls_pair_validate(certificate_file,
                          private_key_file,
                          tls_reason,
                          sizeof(tls_reason)) < 0) {
        LOG_INFO("Management rejected TLS settings: %s",
                 tls_reason[0] ? tls_reason : "certificate-or-key-invalid");

        char *body = g_strdup_printf(
            "{\"error\":\"%s\"}\n",
            tls_reason[0] ? tls_reason : "certificate-or-key-invalid");

        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     body);
        g_free(body);
        return;
    }

    GError *error = NULL;
    if (!save_web_settings(web,
                           port,
                           certificate_file,
                           private_key_file,
                           &error)) {
        LOG_ERROR("Management could not save %s: %s",
                  web->config_file,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        g_hash_table_destroy(form);
        soup_message_body_truncate(request_body);
        respond_text(msg,
                     SOUP_STATUS_INTERNAL_SERVER_ERROR,
                     "application/json; charset=utf-8",
                     "{\"error\":\"save-failed\"}\n");
        return;
    }

    LOG_INFO("Management atomically updated %s: HTTPS port=%u certificate=%s private-key=%s",
             web->config_file,
             port,
             certificate_file,
             private_key_file);

    char *response = g_strdup_printf(
        "{\"ok\":true,\"restart\":true,\"port\":%u}\n",
        port);

    g_hash_table_destroy(form);
    soup_message_body_truncate(request_body);

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "application/json; charset=utf-8",
                 response);
    g_free(response);

    if (web->hooks.request_restart)
        web->hooks.request_restart(web->user_data);
}

static void
management_disconnect_handler(SoupServer *server,
                              SoupServerMessage *msg,
                              const char *path,
                              GHashTable *query,
                              gpointer user_data)
{
    (void)server; (void)path; (void)query;
    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "POST") != 0) {
        respond_method_not_allowed(msg, "POST");
        return;
    }
    if (!management_control_request_allowed(msg) ||
        !management_request_authenticated(web, msg)) {
        respond_text(msg, SOUP_STATUS_UNAUTHORIZED, "application/json; charset=utf-8", "{\"error\":\"authentication-required\"}\n");
        return;
    }

    if (!web->hooks.disconnect_viewer ||
        !web->hooks.disconnect_viewer(web->user_data)) {
        respond_text(msg, SOUP_STATUS_CONFLICT, "application/json; charset=utf-8", "{\"error\":\"no-active-viewer\"}\n");
        return;
    }
    respond_text(msg, SOUP_STATUS_OK, "application/json; charset=utf-8", "{\"ok\":true}\n");
}

static void
management_logout_handler(SoupServer *server,
                          SoupServerMessage *msg,
                          const char *path,
                          GHashTable *query,
                          gpointer user_data)
{
    (void)server; (void)path; (void)query;
    WebServer *web = user_data;

    if (strcmp(soup_server_message_get_method(msg), "POST") != 0) {
        respond_method_not_allowed(msg, "POST");
        return;
    }
    if (!management_control_request_allowed(msg)) {
        respond_text(msg, SOUP_STATUS_FORBIDDEN, "application/json; charset=utf-8", "{\"error\":\"request-rejected\"}\n");
        return;
    }

    if (web->hooks.management_logout)
        web->hooks.management_logout(web->user_data);
    set_session_cookie(msg, NULL);
    set_management_cookie(msg, NULL);
    respond_text(msg, SOUP_STATUS_OK, "application/json; charset=utf-8", "{\"ok\":true}\n");
}

static void
ws_guard_handler(SoupServer *server,
                 SoupServerMessage *msg,
                 const char *path,
                 GHashTable *query,
                 gpointer user_data)
{
    (void)server;
    (void)query;

    WebServer *web = user_data;

    if (strcmp(path, "/ws") != 0) {
        respond_text(msg,
                     SOUP_STATUS_NOT_FOUND,
                     "text/plain; charset=utf-8",
                     "Not Found\n");
        return;
    }

    if (strcmp(soup_server_message_get_method(msg), "GET") != 0) {
        respond_method_not_allowed(msg, "GET");
        return;
    }

    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *upgrade = soup_message_headers_get_one(headers, "Upgrade");
    const char *host = soup_message_headers_get_one(headers, "Host");
    const char *origin = soup_message_headers_get_one(headers, "Origin");
    const char *version = soup_message_headers_get_one(headers, "Sec-WebSocket-Version");
    const char *cookie_header = soup_message_headers_get_one(headers, "Cookie");
    const char *user_agent = soup_message_headers_get_one(headers, "User-Agent");
    const char *peer_addr = soup_server_message_get_remote_host(msg);

    LOG_INFO("WebSocket upgrade request peer=%s host=%s origin=%s version=%s cookie=%s ua=%s",
             peer_addr && *peer_addr ? peer_addr : "unknown",
             host && *host ? host : "(missing)",
             origin && *origin ? origin : "(missing)",
             version && *version ? version : "(missing)",
             cookie_header && *cookie_header ? "present" : "missing",
             user_agent && *user_agent ? user_agent : "(missing)");
    if (!upgrade || g_ascii_strcasecmp(upgrade, "websocket") != 0) {
        LOG_INFO("WebSocket upgrade rejected for peer=%s: missing/invalid Upgrade header",
                 peer_addr && *peer_addr ? peer_addr : "unknown");
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"websocket-required\"}\n");
        return;
    }

    if (!request_origin_matches(msg, TRUE)) {
        LOG_INFO("WebSocket upgrade rejected for peer=%s: origin mismatch host=%s origin=%s",
                 peer_addr && *peer_addr ? peer_addr : "unknown",
                 host && *host ? host : "(missing)",
                 origin && *origin ? origin : "(missing)");
        respond_text(msg,
                     SOUP_STATUS_FORBIDDEN,
                     "application/json; charset=utf-8",
                     "{\"error\":\"origin-rejected\"}\n");
        return;
    }

    char *token = extract_session_cookie(msg);
    if (!token ||
        !web->hooks.validate_websocket_token ||
        !web->hooks.validate_websocket_token(token, web->user_data)) {
        LOG_INFO("WebSocket upgrade rejected for peer=%s: authentication cookie %s or token invalid",
                 peer_addr && *peer_addr ? peer_addr : "unknown",
                 token ? "present" : "missing");
        g_free(token);
        respond_text(msg,
                     SOUP_STATUS_UNAUTHORIZED,
                     "application/json; charset=utf-8",
                     "{\"error\":\"authentication-required\"}\n");
        return;
    }

    LOG_INFO("WebSocket upgrade preflight accepted for peer=%s",
             peer_addr && *peer_addr ? peer_addr : "unknown");

    g_object_set_data_full(G_OBJECT(msg),
                           WEB_WS_TOKEN_DATA_KEY,
                           token,
                           g_free);
}

static gboolean
websocket_message_equals(GBytes *message, const char *expected)
{
    if (!message || !expected)
        return FALSE;

    gsize length = 0;
    const guint8 *data = g_bytes_get_data(message, &length);
    gsize expected_len = strlen(expected);

    return data &&
           length == expected_len &&
           memcmp(data, expected, expected_len) == 0;
}

static gboolean
websocket_client_event_valid(const char *event)
{
    static const char *const allowed[] = {
        "worker-frame-received",
        "worker-blob-read-failed",
        "worker-jpeg-envelope-bad",
        "worker-jpeg-envelope-ok",
        "main-frame-received",
        "main-blob-integrity",
        "main-blob-read-failed",
        "frame-prepare-error",
        "img-blob-decode-error",
        "img-data-url-decode-ok",
        "img-data-url-decode-error",
        "img-data-url-read-failed"
    };
    if (!event) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(allowed); i++) {
        if (strcmp(event, allowed[i]) == 0) return TRUE;
    }
    return FALSE;
}

static gboolean
websocket_parse_client_diagnostic(GBytes *message,
                                  WebServerClientDiagnostic *diagnostic,
                                  char kind[16],
                                  char level[8],
                                  char event[48],
                                  char mime[48])
{
    if (!message || !diagnostic) return FALSE;
    gsize length = 0;
    const guint8 *data = g_bytes_get_data(message, &length);
    if (!data || length == 0 || length > WEB_WS_DIAGNOSTIC_MAX ||
        length > G_MAXINT || memchr(data, '\0', length)) return FALSE;
    char *text = g_strndup((const char *)data, length);
    if (!text) return FALSE;
    unsigned long long seq = 0, bytes = 0, observed = 0;
    unsigned long long checksum = 0, elapsed = 0;
    int consumed = 0;
    int fields = sscanf(
        text,
        "{\"type\":\"client-%15[a-z]\",\"level\":\"%7[a-z]\","
        "\"event\":\"%47[a-z0-9-]\",\"seq\":%llu,\"bytes\":%llu,"
        "\"observed\":%llu,\"checksum\":%llu,\"elapsed\":%llu,"
        "\"mime\":\"%47[a-zA-Z0-9/+.-]\"}%n",
        kind, level, event, &seq, &bytes, &observed, &checksum, &elapsed,
        mime, &consumed);
    gboolean valid =
        fields == 9 && consumed == (int)length &&
        (strcmp(kind, "log") == 0 || strcmp(kind, "telemetry") == 0) &&
        (strcmp(level, "info") == 0 || strcmp(level, "warn") == 0 || strcmp(level, "error") == 0) &&
        websocket_client_event_valid(event) &&
        bytes <= G_MAXUINT32 && observed <= G_MAXUINT32 &&
        checksum <= G_MAXUINT32 && elapsed <= G_MAXUINT32;
    if (valid) {
        diagnostic->kind = kind; diagnostic->level = level; diagnostic->event = event;
        diagnostic->seq = (guint64)seq; diagnostic->bytes = (guint32)bytes;
        diagnostic->observed = (guint32)observed; diagnostic->checksum = (guint32)checksum;
        diagnostic->elapsed_ms = (guint32)elapsed; diagnostic->mime = mime;
    }
    g_free(text);
    return valid;
}

static gboolean
websocket_client_diagnostic_rate_allowed(WebServer *web)
{
    if (!web) return FALSE;
    gint64 now = g_get_monotonic_time();
    if (web->websocket_diagnostic_window_us == 0 ||
        now - web->websocket_diagnostic_window_us >= G_USEC_PER_SEC) {
        web->websocket_diagnostic_window_us = now;
        web->websocket_diagnostic_count = 0;
    }
    if (web->websocket_diagnostic_count >= WEB_WS_DIAGNOSTIC_RATE) return FALSE;
    web->websocket_diagnostic_count++;
    return TRUE;
}

static gboolean
websocket_parse_display_state(GBytes *message, WebServerDisplayState *state)
{
    if (!message || !state)
        return FALSE;

    gsize length = 0;
    const guint8 *data = g_bytes_get_data(message, &length);
    if (!data || length == 0 || length > 256 || length > G_MAXINT ||
        memchr(data, '\0', length))
        return FALSE;

    char *text = g_strndup((const char *)data, length);
    if (!text)
        return FALSE;

    unsigned generation = 0;
    unsigned width = 0;
    unsigned height = 0;
    char mode[16] = {0};
    char orientation[16] = {0};
    int consumed = 0;

    int fields = sscanf(
        text,
        "{\"type\":\"display-state\",\"generation\":%u,"
        "\"mode\":\"%15[a-z]\",\"orientation\":\"%15[a-z]\","
        "\"width\":%u,\"height\":%u}%n",
        &generation, mode, orientation, &width, &height, &consumed);

    gboolean valid =
        fields == 5 &&
        consumed == (int)length &&
        generation > 0 &&
        width >= VNC_BROKER_VIDEO_DIMENSION_MIN &&
        height >= VNC_BROKER_VIDEO_DIMENSION_MIN &&
        width <= VNC_BROKER_VIDEO_DIMENSION_MAX &&
        height <= VNC_BROKER_VIDEO_DIMENSION_MAX &&
        (strcmp(mode, "window") == 0 || strcmp(mode, "fullscreen") == 0) &&
        (strcmp(orientation, "portrait") == 0 ||
         strcmp(orientation, "landscape") == 0);

    if (valid) {
        state->generation = generation;
        state->width = width;
        state->height = height;
        state->mode = strcmp(mode, "fullscreen") == 0 ?
            VNC_BROKER_DISPLAY_FULLSCREEN : VNC_BROKER_DISPLAY_WINDOW;
        state->orientation = strcmp(orientation, "landscape") == 0 ?
            VNC_BROKER_ORIENTATION_LANDSCAPE :
            VNC_BROKER_ORIENTATION_PORTRAIT;
    }

    g_free(text);
    return valid;
}

static void
websocket_message_cb(SoupWebsocketConnection *connection,
                     SoupWebsocketDataType type,
                     GBytes *message,
                     gpointer user_data)
{
    WebServer *web = user_data;
    if (type != SOUP_WEBSOCKET_DATA_TEXT) {
        soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_UNSUPPORTED_DATA, "Text signalling only");
        return;
    }

    if (websocket_message_equals(message, "{\"type\":\"protocol-ready\",\"protocol\":" VNC_WEB_PROTOCOL_VERSION_TEXT "}")) {
        if (web && web->hooks.websocket_protocol_ready &&
            web->hooks.websocket_protocol_ready(VNC_WEB_PROTOCOL_VERSION, web->user_data)) {
            web->websocket_protocol_ready = TRUE;
            soup_websocket_connection_send_text(connection, "{\"type\":\"ready\",\"state\":\"active-browser\",\"media\":\"pending\"}");
            return;
        }
        soup_websocket_connection_send_text(connection, "{\"type\":\"error\",\"error\":\"protocol-not-accepted\"}");
        soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION, "Protocol not accepted");
        return;
    }

    if (web && web->websocket_protocol_ready) {
        WebServerDisplayState display_state = {0};
        if (websocket_parse_display_state(message, &display_state)) {
            gint64 now = g_get_monotonic_time();
            gboolean generation_ok =
                display_state.generation > web->websocket_display_generation;
            gboolean rate_ok =
                web->websocket_display_last_us == 0 ||
                now - web->websocket_display_last_us >=
                    WEB_DISPLAY_MIN_INTERVAL_US;

            if (generation_ok && rate_ok &&
                web->hooks.websocket_display_state &&
                web->hooks.websocket_display_state(&display_state,
                                                   web->user_data)) {
                web->websocket_display_generation = display_state.generation;
                web->websocket_display_last_us = now;
            }
            else {
                char *reply = g_strdup_printf(
                    "{\"type\":\"display-state-rejected\","
                    "\"generation\":%u,\"reason\":\"%s\"}",
                    display_state.generation,
                    !generation_ok ? "stale" :
                    (!rate_ok ? "rate-limit" : "not-accepted"));
                soup_websocket_connection_send_text(connection, reply);
                g_free(reply);
            }
            return;
        }

        WebServerClientDiagnostic diagnostic = {0};
        char kind[16] = {0}, level[8] = {0}, event[48] = {0}, mime[48] = {0};
        if (websocket_parse_client_diagnostic(message, &diagnostic, kind, level, event, mime)) {
            if (websocket_client_diagnostic_rate_allowed(web) && web->hooks.websocket_client_diagnostic)
                web->hooks.websocket_client_diagnostic(&diagnostic, web->user_data);
            return;
        }
    }

    if (websocket_message_equals(message, "{\"type\":\"frame-ack\"}")) {
        if (web && web->websocket_protocol_ready && web->hooks.websocket_frame_ack &&
            web->hooks.websocket_frame_ack(web->user_data)) return;
        soup_websocket_connection_send_text(connection, "{\"type\":\"error\",\"error\":\"frame-ack-not-accepted\"}");
        return;
    }

    if (websocket_message_equals(message, "{\"type\":\"frame-nack\"}")) {
        if (web && web->websocket_protocol_ready && web->hooks.websocket_frame_nack &&
            web->hooks.websocket_frame_nack(web->user_data)) return;
        LOG_INFO("Authenticated legacy browser reached consecutive JPEG decode failure limit; closing media session");
        soup_websocket_connection_close(connection, SOUP_WEBSOCKET_CLOSE_UNSUPPORTED_DATA, "JPEG decode failure limit");
        return;
    }

    soup_websocket_connection_send_text(connection, "{\"type\":\"error\",\"error\":\"signalling-not-implemented\"}");
}

static void
websocket_closed_cb(SoupWebsocketConnection *connection, gpointer user_data)
{
    WebServer *web = user_data;

    if (!web || web->websocket != connection)
        return;

    web->websocket = NULL;
    web->websocket_protocol_ready = FALSE;
    web->websocket_diagnostic_window_us = 0;
    web->websocket_diagnostic_count = 0;
    web->websocket_display_last_us = 0;
    web->websocket_display_generation = 0;

    if (web->hooks.websocket_closed)
        web->hooks.websocket_closed(web->user_data);

    g_object_unref(connection);
}

static void
websocket_handler(SoupServer *server,
                  SoupServerMessage *msg,
                  const char *path,
                  SoupWebsocketConnection *connection,
                  gpointer user_data)
{
    (void)server;
    (void)path;

    WebServer *web = user_data;
    const char *token = g_object_get_data(G_OBJECT(msg), WEB_WS_TOKEN_DATA_KEY);

    if (web->websocket ||
        !token ||
        !web->hooks.bind_websocket ||
        !web->hooks.bind_websocket(token, web->user_data)) {
        LOG_INFO("WebSocket upgraded but session bind was rejected");
        soup_websocket_connection_close(connection,
                                        SOUP_WEBSOCKET_CLOSE_POLICY_VIOLATION,
                                        "Session is not attachable");
        return;
    }

    web->websocket = g_object_ref(connection);
    web->websocket_protocol_ready = FALSE;
    web->websocket_diagnostic_window_us = 0;
    web->websocket_diagnostic_count = 0;
    web->websocket_display_last_us = 0;
    web->websocket_display_generation = 0;
    soup_websocket_connection_set_max_incoming_payload_size(connection,
                                                            WEB_WS_MESSAGE_MAX);
    g_signal_connect(connection, "message",
                     G_CALLBACK(websocket_message_cb), web);
    g_signal_connect(connection, "closed",
                     G_CALLBACK(websocket_closed_cb), web);

    soup_websocket_connection_send_text(
        connection,
        "{\"type\":\"hello\",\"protocol\":" VNC_WEB_PROTOCOL_VERSION_TEXT
        ",\"build\":\"" VNC_MONITOR_VERSION "\"}");

    LOG_INFO("Broker authenticated WebSocket attached; browser protocol hello=%u build=%s",
             VNC_WEB_PROTOCOL_VERSION,
             VNC_MONITOR_VERSION);
}

static int
keyfile_get_enabled(GKeyFile *keyfile, gboolean *enabled)
{
    GError *error = NULL;
    gboolean value = g_key_file_get_boolean(keyfile, "web", "enabled", &error);

    if (!error) {
        *enabled = value;
        return 0;
    }

    if (error->domain == G_KEY_FILE_ERROR &&
        (error->code == G_KEY_FILE_ERROR_GROUP_NOT_FOUND ||
         error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
        g_clear_error(&error);
        *enabled = FALSE;
        return 0;
    }

    LOG_ERROR("Broker invalid [web] enabled: %s", error->message);
    g_clear_error(&error);
    return -1;
}

static int
keyfile_get_port(GKeyFile *keyfile, guint *port)
{
    GError *error = NULL;
    gint value = g_key_file_get_integer(keyfile, "web", "port", &error);

    if (error) {
        if (error->domain == G_KEY_FILE_ERROR &&
            (error->code == G_KEY_FILE_ERROR_GROUP_NOT_FOUND ||
             error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
            g_clear_error(&error);
            *port = WEB_DEFAULT_PORT;
            return 0;
        }

        LOG_ERROR("Broker invalid [web] port: %s", error->message);
        g_clear_error(&error);
        return -1;
    }

    if (value < 1 || value > 65535) {
        LOG_ERROR("Broker invalid [web] port=%d", value);
        return -1;
    }

    *port = (guint)value;
    return 0;
}

static char *
keyfile_get_path(GKeyFile *keyfile, const char *key, const char *fallback)
{
    GError *error = NULL;
    char *value = g_key_file_get_string(keyfile, "web", key, &error);

    if (!error)
        return value;

    if (error->domain == G_KEY_FILE_ERROR &&
        (error->code == G_KEY_FILE_ERROR_GROUP_NOT_FOUND ||
         error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
        g_clear_error(&error);
        return g_strdup(fallback);
    }

    LOG_ERROR("Broker invalid [web] %s: %s", key, error->message);
    g_clear_error(&error);
    return NULL;
}

int
web_server_start(WebServer **out,
                 const char *config_file,
                 const WebServerHooks *hooks,
                 gpointer user_data)
{
    if (!out || !config_file)
        return -1;

    *out = NULL;

    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;

    if (!g_key_file_load_from_file(keyfile,
                                   config_file,
                                   G_KEY_FILE_NONE,
                                   &error)) {
        if (error && error->domain == G_FILE_ERROR &&
            error->code == G_FILE_ERROR_NOENT) {
            g_clear_error(&error);
            g_key_file_unref(keyfile);
            return 0;
        }

        LOG_ERROR("Web endpoint cannot read %s: %s",
                  config_file,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        g_key_file_unref(keyfile);
        return -1;
    }

    gboolean enabled = FALSE;
    guint port = WEB_DEFAULT_PORT;

    if (keyfile_get_enabled(keyfile, &enabled) < 0) {
        g_key_file_unref(keyfile);
        return -1;
    }

    if (!enabled) {
        g_key_file_unref(keyfile);
        return 0;
    }

    if (keyfile_get_port(keyfile, &port) < 0) {
        g_key_file_unref(keyfile);
        return -1;
    }

    char *cert_file = keyfile_get_path(keyfile, "certificate", WEB_DEFAULT_CERT_FILE);
    char *key_file = keyfile_get_path(keyfile, "private-key", WEB_DEFAULT_KEY_FILE);
    g_key_file_unref(keyfile);

    if (!cert_file || !key_file) {
        g_free(cert_file);
        g_free(key_file);
        return -1;
    }

    char tls_reason[64] = {0};
    if (tls_pair_validate(cert_file,
                          key_file,
                          tls_reason,
                          sizeof(tls_reason)) < 0) {
        LOG_ERROR("Web HTTPS certificate/private-key validation failed (%s, %s): %s",
                  cert_file,
                  key_file,
                  tls_reason[0] ? tls_reason : "invalid TLS identity");
        g_free(cert_file);
        g_free(key_file);
        return -1;
    }

    WebServer *web = g_new0(WebServer, 1);
    web->port = port;
    web->started_us = g_get_monotonic_time();
    web->config_file = g_strdup(config_file);
    web->certificate_file = g_strdup(cert_file);
    web->private_key_file = g_strdup(key_file);
    web->user_data = user_data;
    if (hooks)
        web->hooks = *hooks;

    error = NULL;
    web->certificate = g_tls_certificate_new_from_files(cert_file,
                                                        key_file,
                                                        &error);
    if (!web->certificate) {
        LOG_ERROR("Web HTTPS certificate load failed (%s, %s): %s",
                  cert_file,
                  key_file,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        g_free(cert_file);
        g_free(key_file);
        g_free(web->config_file);
        g_free(web->certificate_file);
        g_free(web->private_key_file);
        g_free(web);
        return -1;
    }

    g_free(cert_file);
    g_free(key_file);

    web->server = soup_server_new("server-header", "vnc-monitor", NULL);
    if (!web->server) {
        LOG_ERROR("Could not create broker HTTPS server");
        g_object_unref(web->certificate);
        g_free(web->config_file);
        g_free(web->certificate_file);
        g_free(web->private_key_file);
        g_free(web);
        return -1;
    }

    soup_server_set_tls_certificate(web->server, web->certificate);
    soup_server_add_handler(web->server, "/api/status", status_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/login", login_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/resume", resume_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/logout", browser_logout_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/login", management_login_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/status", management_status_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/settings", management_settings_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/disconnect", management_disconnect_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/logout", management_logout_handler, web, NULL);
    soup_server_add_handler(web->server, "/client.js", client_js_handler, web, NULL);
    soup_server_add_handler(web->server, "/protocol-worker.js", protocol_worker_js_handler, web, NULL);
    soup_server_add_handler(web->server, "/manage.js", management_js_handler, web, NULL);
    soup_server_add_handler(web->server, "/manage", management_page_handler, web, NULL);
    soup_server_add_handler(web->server, "/live", hls_handler, web, NULL);
    soup_server_add_handler(web->server, "/ws", ws_guard_handler, web, NULL);
    soup_server_add_websocket_handler(web->server,
                                      "/ws",
                                      NULL,
                                      NULL,
                                      websocket_handler,
                                      web,
                                      NULL);
    soup_server_add_handler(web->server, "/", root_handler, web, NULL);

    error = NULL;
    if (!soup_server_listen_all(web->server,
                                web->port,
                                SOUP_SERVER_LISTEN_HTTPS |
                                SOUP_SERVER_LISTEN_IPV4_ONLY,
                                &error)) {
        LOG_ERROR("Broker cannot listen on HTTPS TCP/%u: %s",
                  web->port,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        soup_server_disconnect(web->server);
        g_object_unref(web->server);
        g_object_unref(web->certificate);
        g_free(web->config_file);
        g_free(web->certificate_file);
        g_free(web->private_key_file);
        g_free(web);
        return -1;
    }

    *out = web;
    LOG_INFO("Broker HTTPS/WSS authentication with HLS/H.264 test media ready on TCP/%u (WSS/JPEG fallback; SDP/ICE not enabled yet)",
             web->port);
    return 1;
}

gboolean
web_server_send_text(WebServer *web, const char *text)
{
    if (!web || !web->websocket || !text ||
        soup_websocket_connection_get_state(web->websocket) !=
            SOUP_WEBSOCKET_STATE_OPEN)
        return FALSE;

    soup_websocket_connection_send_text(web->websocket, text);
    return TRUE;
}

void
web_server_set_hls_root(WebServer *web, const char *root)
{
    if (!web)
        return;

    g_free(web->hls_root);
    web->hls_root = root && *root ? g_strdup(root) : NULL;
}

gboolean
web_server_send_binary(WebServer *web,
                       const guint8 *data,
                       gsize length)
{
    if (!web || !web->websocket || !data || length == 0 ||
        soup_websocket_connection_get_state(web->websocket) !=
            SOUP_WEBSOCKET_STATE_OPEN)
        return FALSE;

    soup_websocket_connection_send_binary(web->websocket, data, length);
    return TRUE;
}

void
web_server_close_websocket(WebServer *web)
{
    if (!web || !web->websocket)
        return;

    if (soup_websocket_connection_get_state(web->websocket) ==
        SOUP_WEBSOCKET_STATE_OPEN) {
        soup_websocket_connection_close(web->websocket,
                                        SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "Session revoked");
    }
}

void
web_server_stop(WebServer *web)
{
    if (!web)
        return;

    if (web->websocket) {
        g_signal_handlers_disconnect_by_data(web->websocket, web);
        if (soup_websocket_connection_get_state(web->websocket) ==
            SOUP_WEBSOCKET_STATE_OPEN) {
            soup_websocket_connection_close(web->websocket,
                                            SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                            "Server stopping");
        }
        g_object_unref(web->websocket);
        web->websocket = NULL;
    }

    if (web->server) {
        soup_server_disconnect(web->server);
        g_object_unref(web->server);
    }

    if (web->certificate)
        g_object_unref(web->certificate);

    g_free(web->hls_root);
    g_free(web->config_file);
    g_free(web->certificate_file);
    g_free(web->private_key_file);
    g_free(web);
}
