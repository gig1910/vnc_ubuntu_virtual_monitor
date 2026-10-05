#!/usr/bin/env bash
set -euo pipefail

source_file="src/web_server.c"

client_js="$(
    awk '
        /static const char client_js\[\] =/ { capture = 1; next }
        capture && /static const char management_page\[\] =/ { exit }
        capture && /^static void$/ { exit }
        capture { print }
    ' "$source_file"
)"

login_page="$(
    awk '
        /static const char login_page\[\] =/ { capture = 1; next }
        /static const char client_js\[\] =/ { exit }
        capture { print }
    ' "$source_file"
)"

management_js="$(
    awk '
        /static const char management_js\[\] =/ { capture = 1; next }
        capture && /^static void$/ { exit }
        capture { print }
    ' "$source_file"
)"

management_page="$(
    awk '
        /static const char management_page\[\] =/ { capture = 1; next }
        /static const char management_js\[\] =/ { exit }
        capture { print }
    ' "$source_file"
)"

if [[ -z "$client_js" || -z "$login_page" || -z "$management_js" || -z "$management_page" ]]; then
    echo "Could not extract embedded browser frontend" >&2
    exit 1
fi

# iPad 3 / iOS 9.3.x Safari baseline: keep the shipped JavaScript parseable
# as ES5 and avoid Web APIs that arrived only in later Safari releases.
forbidden_js=(
    '=>'
    'async '
    'await '
    'fetch('
    'URLSearchParams'
    'FormData'
    ' const '
    ' let '
    '?.'
    '??'
)

for token in "${forbidden_js[@]}"; do
    if grep -Fq -- "$token" <<<"$client_js" || grep -Fq -- "$token" <<<"$management_js"; then
        echo "Legacy Safari regression: forbidden JavaScript token: $token" >&2
        exit 1
    fi
done

grep -Fq 'XMLHttpRequest' <<<"$client_js"
grep -Fq 'new WebSocket' <<<"$client_js"
grep -Fq 'WebSocket constructor failed' <<<"$client_js"
grep -Fq 'network/TLS handshake failed' <<<"$client_js"
grep -Fq 'encodeURIComponent' <<<"$client_js"
grep -Fq 'function (' <<<"$client_js"
grep -Fq "socket.binaryType = 'arraybuffer'" <<<"$client_js"
grep -Fq "new Blob([data], { type: 'image/jpeg' })" <<<"$client_js"
grep -Fq 'var loader = new Image()' <<<"$client_js"
grep -Fq 'loader.onload = function ()' <<<"$client_js"
grep -Fq 'loader.onerror = function ()' <<<"$client_js"
grep -Fq 'framePending' <<<"$client_js"
grep -Fq 'createObjectURL' <<<"$client_js"
grep -Fq 'video-frame' <<<"$login_page"
grep -Fq 'hls-video' <<<"$login_page"
grep -Fq 'application/vnd.apple.mpegurl' "$source_file"
grep -Fq 'video/mp2t' "$source_file"
grep -Fq '/live/index.m3u8' "$source_file"
grep -Fq 'media-ready' <<<"$client_js"
grep -Fq 'H.264/HLS video playing.' <<<"$client_js"
grep -Fq 'Live browser video.' <<<"$client_js"
grep -Fq "document.body.className = 'streaming'" <<<"$client_js"
grep -Fq "body.streaming .viewer" <<<"$login_page"
grep -Fq "body.streaming #disconnect" <<<"$login_page"
if grep -Fq 'WebRTC video is being prepared' <<<"$client_js"; then
    echo "Legacy Safari regression: browser UI still claims WebRTC media" >&2
    exit 1
fi

# Do not rely on layout engines absent from Safari on iOS 9.
if grep -Eq 'display:[[:space:]]*(grid|flex)|color-scheme|var\(' <<<"$login_page" ||
   grep -Eq 'display:[[:space:]]*(grid|flex)|color-scheme|var\(' <<<"$management_page"; then
    echo "Legacy Safari regression: modern-only CSS found in login page" >&2
    exit 1
fi

grep -Fq 'max-width: 520px' <<<"$login_page"
grep -Fq -- '-webkit-appearance: none' <<<"$login_page"
grep -Fq -- '-webkit-text-size-adjust: 100%' <<<"$login_page"
grep -Fq 'XMLHttpRequest' <<<"$management_js"
grep -Fq 'X-VNC-Monitor-Control' <<<"$management_js"
grep -Fq '/api/manage/settings' <<<"$management_js"
grep -Fq 'function pollStatus()' <<<"$management_js"
grep -Fq 'function loadSettings(force)' <<<"$management_js"
grep -Fq 'function renderStatus(data)' <<<"$management_js"
grep -Fq 'function renderSettings(data)' <<<"$management_js"
grep -Fq 'settingsDirty' <<<"$management_js"
grep -Fq 'Discard unsaved server settings' <<<"$management_js"
grep -Fq 'settings-port' <<<"$management_page"
grep -Fq 'settings-cert' <<<"$management_page"
grep -Fq 'settings-key' <<<"$management_page"
grep -Fq 'max-width: 760px' <<<"$management_page"

echo "legacy Safari frontend: OK"

status_handler="$(
    awk '
        /management_status_handler\(SoupServer \*server,/ { capture = 1 }
        /management_settings_handler\(SoupServer \*server,/ { exit }
        capture { print }
    ' "$source_file"
)"

settings_handler="$(
    awk '
        /management_settings_handler\(SoupServer \*server,/ { capture = 1 }
        /management_disconnect_handler\(SoupServer \*server,/ { exit }
        capture { print }
    ' "$source_file"
)"

if grep -Fq '"settings"' <<<"$status_handler" ||
   grep -Fq 'certificate_file' <<<"$status_handler" ||
   grep -Fq 'private_key_file' <<<"$status_handler"; then
    echo "Management status endpoint leaked static settings back into polling" >&2
    exit 1
fi

grep -Fq 'strcmp(method, "GET") == 0' <<<"$settings_handler"
grep -Fq 'configFile' <<<"$settings_handler"
grep -Fq 'certificate' <<<"$settings_handler"
grep -Fq 'privateKey' <<<"$settings_handler"

echo "management status/settings separation: OK"

grep -Fq 'WebSocket upgrade request peer=' "$source_file"
grep -Fq 'origin mismatch' "$source_file"
grep -Fq 'authentication cookie %s or token invalid' "$source_file"
grep -Fq 'WebSocket upgrade preflight accepted' "$source_file"
echo "legacy WebSocket diagnostics: OK"

# Legacy WebKit does not consistently treat connect-src 'self' as allowing
# wss:// on the same host. The response CSP must therefore add only the
# request Host as an explicit WSS source, not a scheme-wide wildcard.
grep -Fq "connect-src 'self' wss://%s" "$source_file"
grep -Fq 'csp_host_valid(host)' "$source_file"
if grep -Fq "connect-src 'self' wss:;" "$source_file" ||
   grep -Fq 'connect-src *' "$source_file"; then
    echo "Legacy Safari CSP regression: WSS source became overly broad" >&2
    exit 1
fi
echo "legacy Safari same-host WSS CSP: OK"

grep -Fq "img-src blob:" "$source_file"
grep -Fq "media-src 'self'" "$source_file"
echo "legacy Safari Blob/HLS media CSP: OK"

grep -Fq 'validate_media_token' "$source_file"
grep -Fq 'hls_segment_name_valid' "$source_file"
echo "authenticated HLS serving: OK"

grep -Fq 'soup_websocket_connection_get_state(web->websocket) ==' "$source_file"
echo "WebSocket shutdown state guard: OK"


hls_source="src/web_hls.c"

if grep -Fq '= g_shell_quote(' "$hls_source"; then
    echo "HLS regression: shell quoting must never be used for GStreamer file properties" >&2
    exit 1
fi

grep -Fq 'g_strescape(segment_path, NULL)' "$hls_source"
grep -Fq 'send-keyframe-requests=false' "$hls_source"
grep -Fq 'playlist-location=\"%s\"' "$hls_source"
echo "HLS GStreamer path quoting: OK"

grep -Fq 'gst_element_set_state(stream->pipeline, GST_STATE_NULL)' "$hls_source"
grep -Fq 'gst_element_get_state(stream->pipeline' "$hls_source"
if grep -Fq 'gst_app_src_end_of_stream(GST_APP_SRC(stream->appsrc))' "$hls_source"; then
    echo "HLS regression: teardown must not send EOS before the pipeline reaches NULL" >&2
    exit 1
fi
echo "HLS teardown ordering: OK"

grep -Fq 'strcmp(method, "HEAD") == 0' "$source_file"
grep -Fq 'soup_message_headers_get_one(request_headers, "Range")' "$source_file"
grep -Fq '"Accept-Ranges"' "$source_file"
grep -Fq '"Content-Range"' "$source_file"
grep -Fq 'range=invalid status=416' "$source_file"
grep -Fq 'HLS GET path=%s range=%zu-%zu status=%u bytes=%zu' "$source_file"
echo "HLS HEAD/byte-range serving: OK"

grep -Fq '#define WEB_HLS_GOP_DIVISOR 2' "$hls_source"
grep -Fq 'key_int_max = (fps + WEB_HLS_GOP_DIVISOR - 1)' "$hls_source"
grep -Fq 'WEB_HLS_PLAYLIST_LENGTH 6' "$hls_source"
grep -Fq 'WEB_HLS_PLAYLIST_FILES 12' "$hls_source"
echo "HLS sub-second GOP latency profile: OK"
