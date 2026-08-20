#define _GNU_SOURCE

#include "web_server.h"
#include "log.h"

#include <gio/gio.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>

#define WEB_DEFAULT_PORT       8443
#define WEB_DEFAULT_CERT_FILE  "/etc/vnc-monitor/tls/server.crt"
#define WEB_DEFAULT_KEY_FILE   "/etc/vnc-monitor/tls/server.key"

struct WebServer {
    SoupServer *server;
    GTlsCertificate *certificate;
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
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <main>\n"
    "    <h1>VNC Monitor</h1>\n"
    "    <p>Browser WebRTC client</p>\n"
    "    <form method=\"post\" action=\"/api/login\" autocomplete=\"on\">\n"
    "      <label>Username<input name=\"username\" autocomplete=\"username\" required></label>\n"
    "      <label>Password<input type=\"password\" name=\"password\" autocomplete=\"current-password\" required></label>\n"
    "      <button type=\"submit\" disabled>Connect</button>\n"
    "    </form>\n"
    "    <p class=\"note\">Web authentication is not enabled in this development checkpoint yet.</p>\n"
    "  </main>\n"
    "</body>\n"
    "</html>\n";

static void
set_security_headers(SoupServerMessage *msg)
{
    SoupMessageHeaders *headers = soup_server_message_get_response_headers(msg);

    soup_message_headers_replace(headers, "Cache-Control", "no-store");
    soup_message_headers_replace(headers, "Pragma", "no-cache");
    soup_message_headers_replace(headers, "X-Content-Type-Options", "nosniff");
    soup_message_headers_replace(headers, "Referrer-Policy", "no-referrer");
    soup_message_headers_replace(headers, "X-Frame-Options", "DENY");
    soup_message_headers_replace(headers,
                                 "Content-Security-Policy",
                                 "default-src 'none'; style-src 'unsafe-inline'; "
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
        "{\"web\":\"scaffold\",\"busy\":%s,\"slot\":\"%s\"}\n",
        slot_busy(web) ? "true" : "false",
        slot_state(web));

    respond_text(msg,
                 SOUP_STATUS_OK,
                 "application/json; charset=utf-8",
                 body);
    g_free(body);
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

    if (slot_busy(web)) {
        respond_text(msg,
                     SOUP_STATUS_CONFLICT,
                     "application/json; charset=utf-8",
                     "{\"error\":\"busy\"}\n");
        return;
    }

    /*
     * Deliberately do not parse or log credentials until the PAM message path
     * exists. This endpoint only establishes routing/slot semantics for now.
     */
    respond_text(msg,
                 SOUP_STATUS_NOT_IMPLEMENTED,
                 "application/json; charset=utf-8",
                 "{\"error\":\"web-auth-not-implemented\"}\n");
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
    LOG_INFO("Broker HTTPS scaffold ready on TCP/%u (IPv4; WebRTC auth not enabled yet)",
             web->port);
    return 1;
}

void
web_server_stop(WebServer *web)
{
    if (!web)
        return;

    if (web->server) {
        soup_server_disconnect(web->server);
        g_object_unref(web->server);
    }

    if (web->certificate)
        g_object_unref(web->certificate);

    g_free(web);
}
