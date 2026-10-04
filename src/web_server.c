#define _GNU_SOURCE

#include "web_server.h"
#include "broker_protocol.h"
#include "log.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>

#define WEB_DEFAULT_PORT       8443
#define WEB_DEFAULT_CERT_FILE  "/etc/vnc-monitor/tls/server.crt"
#define WEB_DEFAULT_KEY_FILE   "/etc/vnc-monitor/tls/server.key"
#define WEB_LOGIN_BODY_MAX      8192
#define WEB_WS_MESSAGE_MAX      8192
#define WEB_SESSION_COOKIE      "vnc-monitor-session"
#define WEB_WS_TOKEN_DATA_KEY   "vnc-monitor-ws-token"

struct WebServer {
    SoupServer *server;
    GTlsCertificate *certificate;
    SoupWebsocketConnection *websocket;
    guint port;
    WebServerHooks hooks;
    gpointer user_data;
};

static const char login_page[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "  <title>VNC Monitor WebRTC</title>\n"
    "  <style>\n"
    "    html { color-scheme: light dark; font-family: sans-serif; }\n"
    "    body { max-width: 32rem; margin: 3rem auto; padding: 0 1rem; }\n"
    "    form { display: grid; gap: .75rem; }\n"
    "    label { display: grid; gap: .25rem; }\n"
    "    input, button { font: inherit; padding: .6rem; }\n"
    "    .note { opacity: .75; }\n"
    "    #status { white-space: pre-wrap; min-height: 2.5rem; }\n"
    "  </style>\n"
    "  <script src=\"/client.js\" defer></script>\n"
    "</head>\n"
    "<body>\n"
    "  <main>\n"
    "    <h1>VNC Monitor</h1>\n"
    "    <p>Browser WebRTC client</p>\n"
    "    <form id=\"login\" method=\"post\" action=\"/api/login\" autocomplete=\"on\">\n"
    "      <label>Username<input name=\"username\" autocomplete=\"username\" required></label>\n"
    "      <label>Password<input type=\"password\" name=\"password\" autocomplete=\"current-password\" required></label>\n"
    "      <button type=\"submit\">Connect</button>\n"
    "    </form>\n"
    "    <p id=\"status\" class=\"note\">Authentication is bound to the active local GNOME Wayland user.</p>\n"
    "  </main>\n"
    "</body>\n"
    "</html>\n";

static const char client_js[] =
    "(() => {\n"
    "  const form = document.getElementById('login');\n"
    "  const status = document.getElementById('status');\n"
    "  let socket = null;\n"
    "  const show = (text) => { status.textContent = text; };\n"
    "  form.addEventListener('submit', async (event) => {\n"
    "    event.preventDefault();\n"
    "    if (socket) { try { socket.close(); } catch (_) {} socket = null; }\n"
    "    show('Authenticating…');\n"
    "    const body = new URLSearchParams(new FormData(form));\n"
    "    try {\n"
    "      const response = await fetch('/api/login', {\n"
    "        method: 'POST',\n"
    "        headers: {'Content-Type': 'application/x-www-form-urlencoded'},\n"
    "        body,\n"
    "        credentials: 'same-origin'\n"
    "      });\n"
    "      form.elements.password.value = '';\n"
    "      const result = await response.json();\n"
    "      if (!response.ok) { show(result.error || 'Authentication failed'); return; }\n"
    "      show('Authenticated; attaching signalling socket…');\n"
    "      socket = new WebSocket('wss://' + location.host + '/ws');\n"
    "      socket.addEventListener('open', () => show('Authenticated WebSocket connected. WebRTC media is not enabled yet.'));\n"
    "      socket.addEventListener('message', (e) => show(String(e.data)));\n"
    "      socket.addEventListener('close', () => { socket = null; show('WebSocket closed.'); });\n"
    "      socket.addEventListener('error', () => show('WebSocket error.'));\n"
    "    } catch (_) {\n"
    "      form.elements.password.value = '';\n"
    "      show('Connection failed.');\n"
    "    }\n"
    "  });\n"
    "})();\n";

static void
set_security_headers(SoupServerMessage *msg)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);

    soup_message_headers_replace(headers, "Cache-Control", "no-store");
    soup_message_headers_replace(headers, "Pragma", "no-cache");
    soup_message_headers_replace(headers, "X-Content-Type-Options", "nosniff");
    soup_message_headers_replace(headers, "Referrer-Policy", "no-referrer");
    soup_message_headers_replace(headers, "X-Frame-Options", "DENY");
    soup_message_headers_replace(headers, "Cross-Origin-Resource-Policy", "same-origin");
    soup_message_headers_replace(headers, "Cross-Origin-Opener-Policy", "same-origin");
    soup_message_headers_replace(headers,
                                 "Content-Security-Policy",
                                 "default-src 'none'; style-src 'unsafe-inline'; "
                                 "script-src 'self'; connect-src 'self'; "
                                 "form-action 'self'; base-uri 'none'; frame-ancestors 'none'");
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
extract_session_cookie(SoupServerMessage *msg)
{
    SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
    const char *cookie = soup_message_headers_get_one(headers, "Cookie");
    if (!cookie || !*cookie)
        return NULL;

    char **parts = g_strsplit(cookie, ";", -1);
    char *found = NULL;

    for (char **p = parts; p && *p; p++) {
        char *part = g_strstrip(*p);
        const char prefix[] = WEB_SESSION_COOKIE "=";
        if (!g_str_has_prefix(part, prefix))
            continue;

        if (found) {
            g_free(found);
            found = NULL;
            break;
        }

        found = g_strdup(part + sizeof(prefix) - 1);
    }

    g_strfreev(parts);
    return found;
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
    if (!upgrade || g_ascii_strcasecmp(upgrade, "websocket") != 0) {
        respond_text(msg,
                     SOUP_STATUS_BAD_REQUEST,
                     "application/json; charset=utf-8",
                     "{\"error\":\"websocket-required\"}\n");
        return;
    }

    if (!request_origin_matches(msg, TRUE)) {
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
        g_free(token);
        respond_text(msg,
                     SOUP_STATUS_UNAUTHORIZED,
                     "application/json; charset=utf-8",
                     "{\"error\":\"authentication-required\"}\n");
        return;
    }

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

    WebServer *web = g_new0(WebServer, 1);
    web->port = port;
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
        g_free(web);
        return -1;
    }

    g_free(cert_file);
    g_free(key_file);

    web->server = soup_server_new("server-header", "vnc-monitor", NULL);
    if (!web->server) {
        LOG_ERROR("Could not create broker HTTPS server");
        g_object_unref(web->certificate);
        g_free(web);
        return -1;
    }

    soup_server_set_tls_certificate(web->server, web->certificate);
    soup_server_add_handler(web->server, "/api/status", status_handler, web, NULL);
    soup_server_add_handler(web->server, "/api/login", login_handler, web, NULL);
    soup_server_add_handler(web->server, "/client.js", client_js_handler, web, NULL);
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

    g_free(web);
}
