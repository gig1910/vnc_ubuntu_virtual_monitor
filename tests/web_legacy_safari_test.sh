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
grep -Fq 'encodeURIComponent' <<<"$client_js"
grep -Fq 'function (' <<<"$client_js"

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
grep -Fq 'max-width: 760px' <<<"$management_page"

echo "legacy Safari frontend: OK"
