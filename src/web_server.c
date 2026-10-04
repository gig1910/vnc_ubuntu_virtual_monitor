#define _GNU_SOURCE

#include "web_server.h"
#include "broker_protocol.h"
#include "log.h"
#include "tls_pair.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WEB_DEFAULT_PORT       8443
#define WEB_DEFAULT_CERT_FILE  "/etc/vnc-monitor/tls/server.crt"
#define WEB_DEFAULT_KEY_FILE   "/etc/vnc-monitor/tls/server.key"
#define WEB_LOGIN_BODY_MAX      8192
#define WEB_WS_MESSAGE_MAX      8192
#define WEB_SESSION_COOKIE          "vnc-monitor-session"
#define WEB_MANAGEMENT_COOKIE       "vnc-monitor-management"
#define WEB_CONTROL_HEADER          "X-VNC-Monitor-Control"
#define WEB_WS_TOKEN_DATA_KEY       "vnc-monitor-ws-token"
#define WEB_MANAGEMENT_MAX_AGE_S    600
#define WEB_SETTINGS_BODY_MAX       8192

struct WebServer {
    SoupServer *server;
    GTlsCertificate *certificate;
    SoupWebsocketConnection *websocket;
    guint port;
    char *config_file;
    char *certificate_file;
    char *private_key_file;
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
    "    .hint { margin: 18px 0 0; color: #84919c; font-size: 12px; line-height: 18px; text-align: center; }\n"
    "    @media only screen and (max-width: 600px) {\n"
    "      .page { padding: 16px 10px; }\n"
    "      .card { border-radius: 10px; }\n"
    "      .head { padding: 22px 20px 19px; }\n"
    "      .body { padding: 20px; }\n"
    "      .mark { width: 44px; height: 44px; margin-bottom: 14px; border-radius: 10px; font-size: 21px; line-height: 44px; }\n"
    "      h1 { font-size: 26px; line-height: 32px; }\n"
    "    }\n"
    "  </style>\n"
    "  <script src=\"/client.js\" defer></script>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"page\">\n"
    "    <div class=\"card\">\n"
    "      <div class=\"head\">\n"
    "        <div class=\"mark\">V</div>\n"
    "        <h1>VNC Monitor</h1>\n"
    "        <p class=\"sub\">Secure browser connection to the active GNOME desktop.</p>\n"
    "      </div>\n"
    "      <div class=\"body\">\n"
    "        <span class=\"badge\">HTTPS</span><span class=\"badge\">View only</span><span class=\"badge\">Active user only</span>\n"
    "        <form id=\"login\" method=\"post\" action=\"/api/login\" autocomplete=\"on\">\n"
    "          <label class=\"field\"><span>Username</span><input id=\"username\" name=\"username\" type=\"text\" autocomplete=\"username\" autocapitalize=\"off\" autocorrect=\"off\" spellcheck=\"false\" required></label>\n"
    "          <label class=\"field\"><span>Password</span><input id=\"password\" name=\"password\" type=\"password\" autocomplete=\"current-password\" required></label>\n"
    "          <button id=\"connect\" type=\"submit\">Connect</button>\n"
    "          <button id=\"disconnect\" class=\"secondary\" type=\"button\" style=\"display:none\">Disconnect</button>\n"
    "        </form>\n"
    "        <div id=\"status\" class=\"status\"><span class=\"status-dot\"></span><span id=\"status-text\">Ready to connect.</span></div>\n"
    "        <p class=\"hint\">Authentication is bound to the currently active local GNOME Wayland user.<br><a href=\"/manage\">Manage sessions and settings</a></p>\n"
    "      </div>\n"
    "    </div>\n"
    "  </div>\n"
    "</body>\n"
    "</html>\n";

static const char client_js[] =
    "(function () {\n"
    "  'use strict';\n"
    "  var form = document.getElementById('login');\n"
    "  var username = document.getElementById('username');\n"
    "  var password = document.getElementById('password');\n"
    "  var connectButton = document.getElementById('connect');\n"
    "  var disconnectButton = document.getElementById('disconnect');\n"
    "  var statusBox = document.getElementById('status');\n"
    "  var statusText = document.getElementById('status-text');\n"
    "  var socket = null;\n"
    "  var socketOpened = false;\n"
    "\n"
    "  function setStatus(text, kind) {\n"
    "    statusText.innerHTML = '';\n"
    "    statusText.appendChild(document.createTextNode(text));\n"
    "    statusBox.className = 'status' + (kind ? ' status-' + kind : '');\n"
    "  }\n"
    "\n"
    "  function setFormBusy(busy) {\n"
    "    username.disabled = busy;\n"
    "    password.disabled = busy;\n"
    "    connectButton.disabled = busy;\n"
    "    connectButton.innerHTML = busy ? 'Connecting...' : 'Connect';\n"
    "  }\n"
    "\n"
    "  function setConnected(connected) {\n"
    "    username.disabled = connected;\n"
    "    password.disabled = connected;\n"
    "    connectButton.style.display = connected ? 'none' : 'block';\n"
    "    disconnectButton.style.display = connected ? 'block' : 'none';\n"
    "    if (!connected) connectButton.disabled = false;\n"
    "  }\n"
    "\n"
    "  function safeJson(text) {\n"
    "    try { return JSON.parse(text || '{}'); }\n"
    "    catch (e) { return {}; }\n"
    "  }\n"
    "\n"
    "  function closeSocket() {\n"
    "    if (!socket) return;\n"
    "    try { socket.close(); } catch (e) {}\n"
    "  }\n"
    "\n"
    "  function openSocket() {\n"
    "    var url = 'wss://' + window.location.host + '/ws';\n"
    "    socketOpened = false;\n"
    "    try { socket = new WebSocket(url); }\n"
    "    catch (e) {\n"
    "      socket = null;\n"
    "      setFormBusy(false);\n"
    "      setConnected(false);\n"
    "      var detail = '';\n"
    "      if (e) {\n"
    "        if (e.name) detail += String(e.name);\n"
    "        if (e.message) detail += (detail ? ': ' : '') + String(e.message);\n"
    "        if (e.code != null) detail += (detail ? ' ' : '') + '[code=' + String(e.code) + ']';\n"
    "      }\n"
    "      setStatus('WebSocket constructor failed' + (detail ? ': ' + detail : '.'), 'error');\n"
    "      return;\n"
    "    }\n"
    "\n"
    "    socket.onopen = function () {\n"
    "      socketOpened = true;\n"
    "      setFormBusy(false);\n"
    "      setConnected(true);\n"
    "      setStatus('Connected. Secure control channel is active.', 'ok');\n"
    "    };\n"
    "\n"
    "    socket.onmessage = function (event) {\n"
    "      var message = safeJson(event.data);\n"
    "      if (message.type === 'ready') {\n"
    "        setStatus('Connected. WebRTC video is being prepared.', 'ok');\n"
    "      } else if (message.error) {\n"
    "        setStatus('Server message: ' + message.error, 'error');\n"
    "      }\n"
    "    };\n"
    "\n"
    "    socket.onerror = function () {\n"
    "      if (!socketOpened) setStatus('Secure WebSocket network/TLS handshake failed before open.', 'error');\n"
    "    };\n"
    "\n"
    "    socket.onclose = function () {\n"
    "      var wasOpen = socketOpened;\n"
    "      socket = null;\n"
    "      socketOpened = false;\n"
    "      setFormBusy(false);\n"
    "      setConnected(false);\n"
    "      if (wasOpen) setStatus('Disconnected. Ready to connect again.', '');\n"
    "      else if (statusBox.className.indexOf('status-error') < 0) setStatus('Connection closed before it was ready.', 'error');\n"
    "    };\n"
    "  }\n"
    "\n"
    "  function login(event) {\n"
    "    if (event && event.preventDefault) event.preventDefault();\n"
    "    if (socket) closeSocket();\n"
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
    "        setStatus('Authenticated. Opening secure control channel...', 'working');\n"
    "        openSocket();\n"
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
    "  form.onsubmit = login;\n"
    "  disconnectButton.onclick = function () {\n"
    "    setStatus('Disconnecting...', 'working');\n"
    "    closeSocket();\n"
    "  };\n"
    "\n"
    "  if (!window.WebSocket || !window.XMLHttpRequest || !window.JSON) {\n"
    "    connectButton.disabled = true;\n"
    "    setStatus('This browser is too old for the secure browser connection.', 'error');\n"
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
            "script-src 'self'; connect-src 'self' wss://%s; "
            "form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
            host);
    } else {
        csp = g_strdup(
            "default-src 'none'; style-src 'unsafe-inline'; "
            "script-src 'self'; connect-src 'self'; "
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
        WEB_SESSION_COOKIE "=%s; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=30",
        token);
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
    char *token = extract_cookie(msg, WEB_MANAGEMENT_COOKIE);
    gboolean ok = token &&
                  web->hooks.validate_management_token &&
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

    char *body = g_strdup_printf(
        "{\"web\":\"ready\",\"busy\":%s,\"slot\":\"%s\"}\n",
        slot_busy(web) ? "true" : "false",
        slot_state(web));

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "application/json; charset=utf-8",
                 body);
    g_free(body);
}

typedef struct {
    SoupServerMessage *msg;
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

    soup_server_message_pause(msg);

    WebServerAuthResult start = web->hooks.begin_auth(username,
                                                       password,
                                                       peer_addr,
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
management_auth_complete(WebServerAuthResult result,
                         const char *session_token,
                         gpointer completion_data)
{
    PendingLogin *pending = completion_data;
    if (!pending)
        return;

    if (result == WEB_SERVER_AUTH_OK && session_token && *session_token) {
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

static void
websocket_message_cb(SoupWebsocketConnection *connection,
                     SoupWebsocketDataType type,
                     GBytes *message,
                     gpointer user_data)
{
    (void)message;
    (void)user_data;

    if (type != SOUP_WEBSOCKET_DATA_TEXT) {
        soup_websocket_connection_close(connection,
                                        SOUP_WEBSOCKET_CLOSE_UNSUPPORTED_DATA,
                                        "Text signalling only");
        return;
    }

    soup_websocket_connection_send_text(
        connection,
        "{\"type\":\"error\",\"error\":\"signalling-not-implemented\"}");
}

static void
websocket_closed_cb(SoupWebsocketConnection *connection, gpointer user_data)
{
    WebServer *web = user_data;

    if (!web || web->websocket != connection)
        return;

    web->websocket = NULL;

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
    soup_websocket_connection_set_max_incoming_payload_size(connection,
                                                            WEB_WS_MESSAGE_MAX);
    g_signal_connect(connection, "message",
                     G_CALLBACK(websocket_message_cb), web);
    g_signal_connect(connection, "closed",
                     G_CALLBACK(websocket_closed_cb), web);

    soup_websocket_connection_send_text(
        connection,
        "{\"type\":\"ready\",\"state\":\"active-webrtc\","
        "\"signalling\":\"pending\"}");

    LOG_INFO("Broker authenticated WebSocket attached");
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
    soup_server_add_handler(web->server, "/api/manage/login", management_login_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/status", management_status_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/settings", management_settings_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/disconnect", management_disconnect_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/manage/logout", management_logout_handler, web, NULL);
    soup_server_add_handler(web->server, "/client.js", client_js_handler, web, NULL);
    soup_server_add_handler(web->server, "/manage.js", management_js_handler, web, NULL);
    soup_server_add_handler(web->server, "/manage", management_page_handler, web, NULL);
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
    LOG_INFO("Broker HTTPS/WSS authentication ready on TCP/%u (IPv4; SDP/ICE not enabled yet)",
             web->port);
    return 1;
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
        soup_websocket_connection_close(web->websocket,
                                        SOUP_WEBSOCKET_CLOSE_GOING_AWAY,
                                        "Server stopping");
        g_object_unref(web->websocket);
        web->websocket = NULL;
    }

    if (web->server) {
        soup_server_disconnect(web->server);
        g_object_unref(web->server);
    }

    if (web->certificate)
        g_object_unref(web->certificate);

    g_free(web->config_file);
    g_free(web->certificate_file);
    g_free(web->private_key_file);
    g_free(web);
}
