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

for command_name in dpkg-deb grep mktemp find readlink; do
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

# The binary package must never depend on development libraries.
if grep -Eiq '(^|,[[:space:]]*)[^,[:space:]]+-dev([[:space:]]|\(|,|$)' <<<"$depends"; then
    echo "Forbidden *-dev dependency found:" >&2
    echo "  $depends" >&2
    exit 1
fi

# Nor may compiler/build/packaging tools leak into binary Depends.
if grep -Eiq '(^|,[[:space:]]*)(build-essential|pkg-config|dpkg-dev|debhelper[^,[:space:]]*|gcc[^,[:space:]]*|g\+\+[^,[:space:]]*|make)([[:space:]]|\(|,|$)' <<<"$depends"; then
    echo "Forbidden build-tool dependency found:" >&2
    echo "  $depends" >&2
    exit 1
fi

if ! grep -Eq '(^|,[[:space:]]*)pipewire([[:space:]]|,|$)' <<<"$depends"; then
    echo "Runtime dependency 'pipewire' is missing" >&2
    exit 1
fi

required_paths=(
    usr/bin/vnc-monitor
    usr/libexec/vnc-monitor-broker
    usr/libexec/vnc-monitor-auth-helper
    usr/lib/systemd/user/vnc-monitor.service
    usr/lib/systemd/user/graphical-session.target.wants/vnc-monitor.service
    usr/lib/systemd/system/vnc-monitor-broker.service
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
    if [[ ! -e "$root/$path" && ! -L "$root/$path" ]]; then
        echo "Required package path is missing: /$path" >&2
        exit 1
    fi
done

for binary in \
    "$root/usr/bin/vnc-monitor" \
    "$root/usr/libexec/vnc-monitor-broker" \
    "$root/usr/libexec/vnc-monitor-auth-helper"; do
    [[ -x "$binary" ]] || {
        echo "Packaged binary is not executable: $binary" >&2
        exit 1
    }
done

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

# Production user service must be agent-only and have a writable private
# runtime directory for the broker control socket.
grep -Fxq 'ExecStart=/usr/bin/vnc-monitor --agent' \
    "$root/usr/lib/systemd/user/vnc-monitor.service" || {
    echo "Packaged user service is not in broker-managed agent mode" >&2
    exit 1
}
grep -Fxq 'RuntimeDirectory=vnc-monitor' \
    "$root/usr/lib/systemd/user/vnc-monitor.service" || {
    echo "Packaged user service lacks RuntimeDirectory=vnc-monitor" >&2
    exit 1
}

agent_wants="$root/usr/lib/systemd/user/graphical-session.target.wants/vnc-monitor.service"
[[ -L "$agent_wants" ]] || {
    echo "Global graphical-session.target Wants link is not a symlink" >&2
    exit 1
}
[[ "$(readlink "$agent_wants")" == '../vnc-monitor.service' ]] || {
    echo "Unexpected user-service Wants target: $(readlink "$agent_wants")" >&2
    exit 1
}

broker_unit="$root/usr/lib/systemd/system/vnc-monitor-broker.service"
grep -Fxq 'ExecStart=/usr/libexec/vnc-monitor-broker' "$broker_unit" || {
    echo "Packaged broker service does not start /usr/libexec/vnc-monitor-broker" >&2
    exit 1
}

# ProtectHome=yes also hides /run/user, which makes the active user's
# $XDG_RUNTIME_DIR/vnc-monitor/agent.sock invisible to the system broker.
# Keep /home and /root inaccessible explicitly, while exposing /run/user
# read-only so connect(2) to the agent's Unix socket remains possible.
if grep -Fxq 'ProtectHome=yes' "$broker_unit"; then
    echo "Packaged broker incorrectly hides /run/user with ProtectHome=yes" >&2
    exit 1
fi
grep -Fxq 'InaccessiblePaths=/home /root' "$broker_unit" || {
    echo "Packaged broker does not keep /home and /root inaccessible" >&2
    exit 1
}
grep -Fxq 'ReadOnlyPaths=/run/user' "$broker_unit" || {
    echo "Packaged broker cannot safely see /run/user agent sockets" >&2
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

# Maintainer scripts must manage the two system services. The per-user agent
# is globally wanted for future graphical sessions and is not started from a
# root package script inside arbitrary user managers.
grep -q 'vnc-monitor-broker.service' "$control/postinst" || {
    echo "postinst does not activate the system broker" >&2
    exit 1
}
grep -q 'vnc-monitor-auth.socket' "$control/postinst" || {
    echo "postinst does not activate the PAM socket" >&2
    exit 1
}

printf '\n===== VNC MONITOR: PACKAGE VERIFY =====\n'
printf 'Package:      %s\n' "$package"
printf 'Version:      %s\n' "$version"
printf 'Architecture: %s\n' "$architecture"
printf 'Depends:      %s\n' "$depends"
printf 'Config:       /etc/vnc-monitor/config.ini (conffile)\n'
printf 'Broker:       /usr/libexec/vnc-monitor-broker\n'
printf 'Broker unit:  /usr/lib/systemd/system/vnc-monitor-broker.service\n'
printf 'Broker IPC:   /run/user visible read-only; /home and /root inaccessible\n'
printf 'User agent:   /usr/bin/vnc-monitor --agent\n'
printf 'Agent wants:  graphical-session.target (global package symlink)\n'
printf 'Auth socket:  /usr/lib/systemd/system/vnc-monitor-auth.socket\n'
printf 'Private key:  not present in package\n'
printf 'Result:       OK\n'