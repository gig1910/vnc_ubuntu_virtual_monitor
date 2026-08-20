#!/usr/bin/env bash
set -Eeuo pipefail

if (($# != 1)); then
    echo "Usage: $0 PACKAGE.deb" >&2
    exit 2
fi

DEB="$1"
if [[ ! -f "$DEB" ]]; then
    echo "Package not found: $DEB" >&2
    exit 1
fi

for command_name in dpkg-deb grep mktemp; do
    command -v "$command_name" >/dev/null 2>&1 || {
        echo "Required command not found: $command_name" >&2
        exit 1
    }
done

work_dir="$(mktemp -d)"
trap 'rm -rf -- "$work_dir"' EXIT

root="$work_dir/root"
control="$work_dir/control"
mkdir -p "$root" "$control"

dpkg-deb -x "$DEB" "$root"
dpkg-deb -e "$DEB" "$control"

package="$(dpkg-deb -f "$DEB" Package)"
version="$(dpkg-deb -f "$DEB" Version)"
architecture="$(dpkg-deb -f "$DEB" Architecture)"
depends="$(dpkg-deb -f "$DEB" Depends)"

[[ "$package" == "vnc-monitor" ]] || {
    echo "Unexpected Package field: $package" >&2
    exit 1
}

[[ -n "$version" ]] || { echo "Missing Version field" >&2; exit 1; }
[[ -n "$architecture" ]] || { echo "Missing Architecture field" >&2; exit 1; }
[[ -n "$depends" ]] || { echo "Missing Depends field" >&2; exit 1; }

# The binary package must never depend on build/development tooling.
if grep -Eiq '(^|[,[:space:]])(build-essential|pkg-config|dpkg-dev|debhelper[^,[:space:]]*|gcc[^,[:space:]]*|g\+\+[^,[:space:]]*|make)([,([:space:]]|$)|-dev([,([:space:]]|$)' <<<"$depends"; then
    echo "Forbidden build/development dependency found:" >&2
    echo "  $depends" >&2
    exit 1
fi

# PipeWire is a runtime service requirement in addition to the shared-library
# dependencies discovered by dpkg-shlibdeps.
if ! grep -Eq '(^|,[[:space:]]*)pipewire([[:space:]]|,|$)' <<<"$depends"; then
    echo "Runtime dependency 'pipewire' is missing" >&2
    exit 1
fi

required_paths=(
    usr/bin/vnc-monitor
    usr/libexec/vnc-monitor-auth-helper
    usr/lib/systemd/user/vnc-monitor.service
    usr/lib/systemd/system/vnc-monitor-auth.socket
    usr/lib/systemd/system/vnc-monitor-auth@.service
    etc/vnc-monitor/config.ini
    etc/pam.d/vnc-monitor
    usr/share/doc/vnc-monitor/README.md
    usr/share/doc/vnc-monitor/CHANGELOG.md
    usr/share/doc/vnc-monitor/SECURITY.md
    usr/share/doc/vnc-monitor/LICENSE
)

for path in "${required_paths[@]}"; do
    if [[ ! -e "$root/$path" ]]; then
        echo "Required package path is missing: /$path" >&2
        exit 1
    fi
done

[[ -x "$root/usr/bin/vnc-monitor" ]] || {
    echo "/usr/bin/vnc-monitor is not executable" >&2
    exit 1
}
[[ -x "$root/usr/libexec/vnc-monitor-auth-helper" ]] || {
    echo "/usr/libexec/vnc-monitor-auth-helper is not executable" >&2
    exit 1
}

# No server private identity may ever be shipped in the package.
if find "$root" -type f -name '*.pem' -print -quit | grep -q .; then
    echo "A PEM/private-identity file was found inside the package" >&2
    find "$root" -type f -name '*.pem' -print >&2
    exit 1
fi

[[ -f "$control/conffiles" ]] || {
    echo "DEBIAN/conffiles is missing" >&2
    exit 1
}
grep -Fxq '/etc/vnc-monitor/config.ini' "$control/conffiles" || {
    echo "/etc/vnc-monitor/config.ini is not declared as a conffile" >&2
    exit 1
}
grep -Fxq '/etc/pam.d/vnc-monitor' "$control/conffiles" || {
    echo "/etc/pam.d/vnc-monitor is not declared as a conffile" >&2
    exit 1
}

# Validate the package-specific unit paths and generic local-auth socket model.
grep -Fxq 'ExecStart=/usr/bin/vnc-monitor' \
    "$root/usr/lib/systemd/user/vnc-monitor.service" || {
    echo "Packaged user service does not start /usr/bin/vnc-monitor" >&2
    exit 1
}
grep -Fxq 'ExecStart=/usr/libexec/vnc-monitor-auth-helper' \
    "$root/usr/lib/systemd/system/vnc-monitor-auth@.service" || {
    echo "Packaged PAM service does not start /usr/libexec/vnc-monitor-auth-helper" >&2
    exit 1
}
grep -Fxq 'SocketMode=0666' \
    "$root/usr/lib/systemd/system/vnc-monitor-auth.socket" || {
    echo "Packaged PAM socket is not the generic SO_PEERCRED-protected variant" >&2
    exit 1
}

printf '\n===== VNC MONITOR: PACKAGE VERIFY =====\n'
printf 'Package:      %s\n' "$package"
printf 'Version:      %s\n' "$version"
printf 'Architecture: %s\n' "$architecture"
printf 'Depends:      %s\n' "$depends"
printf 'Config:       /etc/vnc-monitor/config.ini (conffile)\n'
printf 'User unit:    /usr/lib/systemd/user/vnc-monitor.service\n'
printf 'Auth socket:  /usr/lib/systemd/system/vnc-monitor-auth.socket\n'
printf 'Private key:  not present in package\n'
printf 'Result:       OK\n'
