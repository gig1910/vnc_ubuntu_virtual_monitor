#!/usr/bin/env bash
set -euo pipefail

source_file="src/web_server.c"

protocol_worker_js="$(
    awk '
        /static const char protocol_worker_js\[\] =/ { capture = 1; next }
        capture && /static const char client_js\[\] =/ { exit }
        capture { print }
    ' "$source_file"
)"

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

if [[ -z "$protocol_worker_js" || -z "$client_js" || -z "$login_page" || -z "$management_js" || -z "$management_page" ]]; then
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
    if grep -Fq -- "$token" <<<"$protocol_worker_js" ||
       grep -Fq -- "$token" <<<"$client_js" ||
       grep -Fq -- "$token" <<<"$management_js"; then
        echo "Legacy Safari regression: forbidden JavaScript token: $token" >&2
        exit 1
    fi
done

grep -Fq 'XMLHttpRequest' <<<"$client_js"
grep -Fq 'new Worker' <<<"$client_js"
grep -Fq '/protocol-worker.js?protocol=' <<<"$client_js"
grep -Fq '/client.js?protocol=' <<<"$login_page"
grep -Fq 'protocol_worker_js_handler' "$source_file"
grep -Fq '"/protocol-worker.js"' "$source_file"
grep -Fq 'new WebSocket' <<<"$protocol_worker_js"
grep -Fq 'WebSocket constructor failed' <<<"$protocol_worker_js"
grep -Fq 'network/TLS handshake failed' <<<"$protocol_worker_js"
grep -Fq 'encodeURIComponent' <<<"$client_js"
grep -Fq 'function (' <<<"$client_js"
grep -Fq "socket.binaryType = 'blob'" <<<"$protocol_worker_js"
grep -Fq "objectUrlApi.createObjectURL(blob)" <<<"$client_js"
grep -Fq "type: 'frame-result'" <<<"$client_js"
grep -Fq 'socket.send' <<<"$protocol_worker_js"
grep -Fq 'frame-ack' <<<"$protocol_worker_js"
grep -Fq 'frame-nack' <<<"$protocol_worker_js"
grep -Fq 'new FileReaderSync().readAsArrayBuffer(data)' <<<"$protocol_worker_js"
grep -Fq 'new Uint8Array(buffer)' <<<"$protocol_worker_js"
grep -Fq "data.slice(0, length, 'image/jpeg')" <<<"$protocol_worker_js"
if grep -Fq 'new WebSocket' <<<"$client_js" ||
   grep -Fq 'new Uint8Array' <<<"$client_js" ||
   grep -Fq 'FileReaderSync' <<<"$client_js" ||
   grep -Fq 'new Blob([data]' <<<"$client_js"; then
    echo "Legacy Safari regression: WSS/protocol parsing leaked back to the UI thread" >&2
    exit 1
fi
if grep -Eq 'document\.|window\.' <<<"$protocol_worker_js"; then
    echo "Legacy Safari regression: protocol worker depends on DOM/window state" >&2
    exit 1
fi
grep -Fq 'Protocol client/server:' <<<"$login_page"
grep -Fq 'Build client/server:' <<<"$login_page"
grep -Fq 'protocol-mismatch' <<<"$protocol_worker_js"
grep -Fq 'protocol-ready' <<<"$protocol_worker_js"
grep -Fq 'DISPLAY_STATE_HYSTERESIS_MS = 2500' <<<"$client_js"
grep -Fq "window.addEventListener('resize'" <<<"$client_js"
grep -Fq "window.addEventListener('orientationchange'" <<<"$client_js"
grep -Fq "document.addEventListener('webkitfullscreenchange'" <<<"$client_js"
grep -Fq "message.type === 'display-state'" <<<"$protocol_worker_js"
grep -Fq 'flushDisplayState' <<<"$protocol_worker_js"
grep -Fq 'display-state-applied' <<<"$protocol_worker_js"
grep -Fq '__Host-vnc-monitor-device' "$source_file"
grep -Fq 'HttpOnly; SameSite=Strict' "$source_file"
grep -Fq 'websocket_parse_display_state' "$source_file"
grep -Fq 'WEB_DISPLAY_MIN_INTERVAL_US' "$source_file"
grep -Fq 'VNC_BROKER_CONTROL_DEVICE_BIND' include/broker_protocol.h src/broker.c
grep -Fq 'VNC_BROKER_CONTROL_DISPLAY_SIZE_APPLIED' include/broker_protocol.h src/broker.c
if grep -Fq 'device_id' <<<"$client_js" || grep -Fq 'deviceId' <<<"$client_js"; then
    echo "Legacy Safari regression: server device identity leaked into client JavaScript" >&2
    exit 1
fi
grep -Fq 'protocol-reload=' <<<"$client_js"
grep -Fq 'function finishSuccess()' <<<"$client_js"
grep -Fq 'videoFrame.onload = finishSuccess' <<<"$client_js"
grep -Fq 'videoFrame.onerror = function ()' <<<"$client_js"
grep -Fq 'framePending' <<<"$client_js"
grep -Fq 'createObjectURL' <<<"$client_js"
grep -Fq 'video-frame' <<<"$login_page"
grep -Fq 'hls-video' <<<"$login_page"
grep -Fq 'application/vnd.apple.mpegurl' "$source_file"
grep -Fq 'video/mp2t' "$source_file"
grep -Fq '/live/index.m3u8' "$source_file"
grep -Fq 'media-ready' <<<"$client_js"
grep -Fq 'H.264/HLS video playing' <<<"$client_js"
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

grep -Fq "img-src blob: data:" "$source_file"
grep -Fq "media-src 'self'" "$source_file"
echo "legacy Safari Blob/HLS media CSP: OK"

grep -Fq 'validate_media_token' "$source_file"
grep -Fq 'hls_segment_name_valid' "$source_file"
echo "authenticated HLS serving: OK"

grep -Fq '#define VNC_WEB_PROTOCOL_VERSION              5u' include/web_server.h
grep -Fq 'VNC_WEB_PROTOCOL_VERSION' include/web_server.h src/web_server.c src/broker.c
grep -Fq 'websocket_protocol_ready' include/web_server.h src/web_server.c src/broker.c
grep -Fq 'websocket_frame_ack' include/web_server.h src/web_server.c src/broker.c
grep -Fq 'websocket_frame_nack' include/web_server.h src/web_server.c src/broker.c
grep -Fq 'websocket_client_diagnostic' include/web_server.h src/web_server.c src/broker.c
grep -Fq 'WEB_WS_DIAGNOSTIC_MAX' src/web_server.c
grep -Fq 'WEB_WS_DIAGNOSTIC_RATE' src/web_server.c
grep -Fq 'websocket_client_event_valid' src/web_server.c
grep -Fq "sendDiagnostic('telemetry'" <<<"$protocol_worker_js"
grep -Fq "sendDiagnostic('log'" <<<"$protocol_worker_js"
grep -Fq 'worker-jpeg-envelope-ok' <<<"$protocol_worker_js"
grep -Fq 'main-blob-integrity' <<<"$client_js"
grep -Fq 'img-blob-decode-error' <<<"$client_js"
grep -Fq 'img-data-url-decode-ok' <<<"$client_js"
grep -Fq "var jpegRenderMode = 'probe'" <<<"$client_js"
grep -Fq "jpegRenderMode === 'data'" <<<"$client_js"
grep -Fq "jpegRenderMode = 'data'" <<<"$client_js"
grep -Fq "jpegRenderMode = 'blob'" <<<"$client_js"
sticky_line="$(grep -nF "if (jpegRenderMode === 'data')" <<<"$client_js" | tail -n1 | cut -d: -f1)"
blob_create_line="$(grep -nF 'objectUrlApi.createObjectURL(blob)' <<<"$client_js" | head -n1 | cut -d: -f1)"
if [[ -z "$sticky_line" || -z "$blob_create_line" || "$sticky_line" -ge "$blob_create_line" ]]; then
    echo "Legacy Safari regression: sticky data-URL path must bypass Blob URL creation" >&2
    exit 1
fi
echo "legacy Safari sticky data-URL JPEG rendering: OK"
grep -Fq 'id="video-frame-a"' <<<"$login_page"
grep -Fq 'id="video-frame-b"' <<<"$login_page"
grep -Fq "var activeVideoFrame = videoFrameA" <<<"$client_js"
grep -Fq "var stagingVideoFrame = videoFrameB" <<<"$client_js"
grep -Fq "activeVideoFrame = stagingVideoFrame" <<<"$client_js"
grep -Fq "stagingVideoFrame = oldActive" <<<"$client_js"
grep -Fq "stagingVideoFrame.src = nextUrl" <<<"$client_js"
grep -Fq "stagingVideoFrame.removeAttribute('src')" <<<"$client_js"
if grep -Fq "videoFrame.src = nextUrl" <<<"$client_js"; then
    echo "Legacy Safari regression: JPEG renderer writes directly into visible frame" >&2
    exit 1
fi
echo "legacy Safari double-buffer JPEG swap: OK"

grep -Fq 'img-data-url-decode-error' <<<"$client_js"
grep -Fq 'img-data-url-read-failed' <<<"$client_js"
grep -Fq 'readAsDataURL(blob)' <<<"$client_js"
grep -Fq "reader.result.indexOf('data:image/jpeg')" <<<"$client_js"
grep -Fq "mode=' + decodeMode" <<<"$client_js"
grep -Fq 'readAsBinaryString' <<<"$client_js"
grep -Fq 'Broker browser client diagnostic:' src/broker.c
grep -Fq '"frame-forwarded"' src/broker.c
grep -Fq '"frame-ack"' src/broker.c
grep -Fq '"frame-nack"' src/broker.c
grep -Fq 'web_in_flight_adler32' src/broker.c
grep -Fq 'VNC_BROKER_CONTROL_VIDEO_FRAME_ACK' include/broker_protocol.h src/broker.c src/main.c
grep -Fq 'queue-depth=1' src/main.c src/broker.c
grep -Fq 'Legacy browser first JPEG integrity:' src/main.c
grep -Fq 'Broker first JPEG integrity:' src/broker.c
grep -Fq 'reached consecutive JPEG decode failure limit' src/web_server.c
grep -Fq 'Broker browser JPEG decode failure:' src/broker.c
grep -Fq 'VNC_WEB_JPEG_DECODE_FAILURE_LIMIT' include/web_server.h src/broker.c
echo "legacy WSS/JPEG worker protocol, ACK/NACK pacing and integrity diagnostics: OK"

web_media_lifetime="$(
    awk '
        /serve_web_media_lifetime\(int control_fd,/ { capture = 1 }
        capture { print }
        capture && /^}/ { exit }
    ' src/main.c
)"
grep -Fq 'MonitorLayoutCache layout_cache' <<<"$web_media_lifetime"
grep -Fq 'web_device_layout_prepare(&layout_cache' <<<"$web_media_lifetime"
grep -Fq 'monitor_layout_cache_apply(&layout_cache' <<<"$web_media_lifetime"
grep -Fq 'monitor_layout_cache_save(&layout_cache' <<<"$web_media_lifetime"
grep -Fq 'monitor_layout_cache_clear(&layout_cache)' <<<"$web_media_lifetime"

save_line="$(grep -nF 'monitor_layout_cache_save(&layout_cache' <<<"$web_media_lifetime" | tail -n1 | cut -d: -f1)"
stop_line="$(grep -nF 'real_monitor_stop(&real)' <<<"$web_media_lifetime" | tail -n1 | cut -d: -f1)"
if [[ -z "$save_line" || -z "$stop_line" || "$save_line" -ge "$stop_line" ]]; then
    echo "Web monitor layout regression: device layout must be saved before virtual monitor teardown" >&2
    exit 1
fi
echo "transport-independent monitor layout persistence: OK"

grep -Fq 'monitor_layout_cache_prepare_scoped' include/monitor_layout_cache.h src/monitor_layout_cache.c
grep -Fq 'layout-v3-%s.ini' src/monitor_layout_cache.c
grep -Fq 'device_profile_id_valid' include/device_profile.h src/device_profile.c
grep -Fq 'DEVICE_DISPLAY_WINDOW' include/device_profile.h src/device_profile.c
grep -Fq 'DEVICE_DISPLAY_FULLSCREEN' include/device_profile.h src/device_profile.c
grep -Fq 'DEVICE_ORIENTATION_PORTRAIT' include/device_profile.h src/device_profile.c
grep -Fq 'DEVICE_ORIENTATION_LANDSCAPE' include/device_profile.h src/device_profile.c
grep -Fq '"%s.%s"' src/device_profile.c
grep -Fq '"device-%s-%s-%s"' src/device_profile.c
grep -Fq 'src/device_profile.c' Makefile
echo "device-scoped display profile persistence: OK"

grep -Fq '#include "device_profile.h"' src/main.c
grep -Fq 'VNC_BROKER_CONTROL_DEVICE_BIND' src/main.c
grep -Fq 'device_profile_load(&device_profile' src/main.c
grep -Fq 'device_profile_get_size(' src/main.c
grep -Fq 'VNC_BROKER_CONTROL_DISPLAY_SIZE' src/main.c
grep -Fq 'real_monitor_resize(&real' src/main.c
grep -Fq 'device_profile_update_state(' src/main.c
grep -Fq 'device_profile_save(&device_profile)' src/main.c
grep -Fq 'web_device_layout_prepare(&layout_cache' src/main.c
grep -Fq 'VNC_BROKER_CONTROL_DISPLAY_SIZE_APPLIED' src/main.c

display_resize_block="$(
    awk '
        /if \(type == VNC_BROKER_CONTROL_DISPLAY_SIZE\)/ { capture = 1 }
        capture { print }
        capture && /Browser display state applied:/ { done = 1 }
        done && /continue;/ { exit }
    ' src/main.c
)"
grep -Fq 'web_media_sender_stop_join' <<<"$display_resize_block"
grep -Fq 'monitor_layout_cache_save(&layout_cache' <<<"$display_resize_block"
grep -Fq 'real_monitor_resize(&real' <<<"$display_resize_block"
grep -Fq 'monitor_layout_cache_apply(' <<<"$display_resize_block"
grep -Fq 'web_send_display_applied' <<<"$display_resize_block"
echo "device display-state resize lifecycle: OK"
grep -Fq 'VNC_BROKER_CONTROL_DISPLAY_SIZE_REJECTED' include/broker_protocol.h src/broker.c src/main.c
grep -Fq 'web_send_display_rejected' src/main.c
grep -Fq "message.reason === 'runtime'" <<<"$client_js"
grep -Fq "message.reason === 'rate-limit'" <<<"$client_js"
echo "display resize rollback is acknowledged without retry loop: OK"


grep -Fq '"logical-width"' src/monitor_layout_cache.c
grep -Fq '"logical-height"' src/monitor_layout_cache.c
grep -Fq 'transform_swaps_dimensions' src/monitor_layout_cache.c
grep -Fq 'primary_geometry_valid' src/monitor_layout_cache.c
grep -Fq 'virtual_group && !primary' src/monitor_layout_cache.c
grep -Fq 'primary_x - current_logical_width - gap' src/monitor_layout_cache.c
grep -Fq 'primary_y - current_logical_height - gap' src/monitor_layout_cache.c
echo "resized virtual monitor keeps primary-relative layout gap: OK"
grep -Fq 'monitor_layout_cache_seed_from' include/monitor_layout_cache.h src/monitor_layout_cache.c src/main.c
grep -Fq 'previous_layout_path' src/main.c
grep -Fq '!layout_cache.cache_existed' src/main.c
grep -Fq 'g_chmod(cache->cache_path, 0600)' src/monitor_layout_cache.c
echo "new display state inherits previous layout before first apply: OK"

grep -Fq 'web_device_layout_seed_legacy' src/main.c
grep -Fq 'monitor_layout_cache_prepare(&legacy, cfg)' src/main.c
grep -Fq 'monitor_layout_cache_file_has_virtual(&legacy)' src/main.c
grep -Fq 'Browser device layout inherited existing VNC layout:' src/main.c
echo "first device state inherits existing VNC layout non-destructively: OK"







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

grep -Fq 'WEB_HLS_PLAYLIST_LENGTH 3' "$hls_source"
grep -Fq 'WEB_HLS_PLAYLIST_FILES 4' "$hls_source"
if grep -Fq 'WEB_HLS_GOP_DIVISOR' "$hls_source"; then
    echo "HLS regression: experimental sub-second GOP left enabled" >&2
    exit 1
fi
echo "HLS stable one-second legacy profile: OK"

grep -Fq 'function seekHlsNearLiveEdge(reason)' <<<"$client_js"
grep -Fq 'hlsVideo.seekable.end(index)' <<<"$client_js"
grep -Fq 'target = end - 0.75' <<<"$client_js"
grep -Fq 'hlsVideo.onloadedmetadata = function ()' <<<"$client_js"
grep -Fq 'hlsVideo.oncanplay = function ()' <<<"$client_js"
grep -Fq 'hlsVideo.onplaying = function ()' <<<"$client_js"
echo "HLS live-edge seek: OK"
