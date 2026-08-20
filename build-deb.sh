#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

JOBS="${JOBS:-$(nproc)}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/dist}"
DEB_REVISION="${DEB_REVISION:-1}"
DEB_MAINTAINER="${DEB_MAINTAINER:-gig1910 <noreply@github.com>}"
CLEAN=0

usage() {
    cat <<'EOF'
Usage: ./build-deb.sh [--clean] [--jobs N] [--output-dir DIR]

Build a compiled Debian/Ubuntu binary package for the current architecture.
The resulting .deb contains runtime binaries, config, PAM support and systemd
units. Build/development packages are checked only on this build machine and
are NOT added to the binary package Depends field.

Options:
  --clean            Run make clean before compilation.
  -j, --jobs N       Parallel build jobs (default: nproc; also JOBS=N).
  -o, --output-dir   Output directory (default: ./dist; also OUT_DIR=DIR).
  -h, --help         Show this help.

Environment:
  DEB_REVISION=N     Debian package revision (default: 1).
  DEB_MAINTAINER=... Maintainer field override.

Run as a normal user. No sudo is used by the package builder.
EOF
}

while (($#)); do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        -j|--jobs)
            (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
            JOBS="$2"
            shift 2
            ;;
        -o|--output-dir)
            (($# >= 2)) || { echo "Missing value for $1" >&2; exit 2; }
            OUT_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$JOBS" in
    ''|*[!0-9]*) echo "Invalid job count: $JOBS" >&2; exit 2;;
esac
((JOBS >= 1)) || { echo "Job count must be at least 1" >&2; exit 2; }

case "$DEB_REVISION" in
    ''|*[!0-9A-Za-z.+~]*) echo "Invalid DEB_REVISION: $DEB_REVISION" >&2; exit 2;;
esac

if ((EUID == 0)); then
    echo "Do not run build-deb.sh as root." >&2
    exit 1
fi

printf '\n===== VNC MONITOR: DEB BUILD PREFLIGHT =====\n'

required_commands=(
    make cc pkg-config nproc mktemp install sed
    dpkg-deb dpkg-shlibdeps dpkg-architecture
)
missing_commands=()

for command_name in "${required_commands[@]}"; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        missing_commands+=("$command_name")
    fi
done

if ((${#missing_commands[@]})); then
    printf 'Missing required build commands:\n' >&2
    printf '  %s\n' "${missing_commands[@]}" >&2
    cat >&2 <<'EOF'

Typical Ubuntu packages providing the packaging tools are:
  sudo apt install build-essential pkg-config dpkg-dev

No build was attempted.
EOF
    exit 1
fi

pkg_modules=(
    libvncserver
    openssl
    nettle
    glib-2.0
    gio-2.0
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
    libpipewire-0.3
)
missing_modules=()

for module in "${pkg_modules[@]}"; do
    if pkg-config --exists "$module"; then
        printf '  %-24s %s\n' "$module" "$(pkg-config --modversion "$module")"
    else
        missing_modules+=("$module")
    fi
done

probe_dir="$(mktemp -d)"
work_dir=""
cleanup() {
    rm -rf -- "$probe_dir"
    if [[ -n "$work_dir" ]]; then
        rm -rf -- "$work_dir"
    fi
}
trap cleanup EXIT
trap 'rc=$?; echo "DEB build failed (line $LINENO, exit $rc)" >&2; exit "$rc"' ERR

cat >"$probe_dir/jpeg.c" <<'EOF'
#include <stdio.h>
#include <jpeglib.h>
int main(void) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return 0;
}
EOF

cat >"$probe_dir/pam.c" <<'EOF'
#include <security/pam_appl.h>
int main(void) {
    pam_handle_t *pamh = 0;
    (void)pamh;
    return PAM_SUCCESS;
}
EOF

jpeg_ok=1
pam_ok=1
cc "$probe_dir/jpeg.c" -o "$probe_dir/jpeg" -ljpeg >/dev/null 2>&1 || jpeg_ok=0
cc "$probe_dir/pam.c" -o "$probe_dir/pam" -lpam >/dev/null 2>&1 || pam_ok=0

((jpeg_ok)) && printf '  %-24s %s\n' 'libjpeg' 'compile/link OK'
((pam_ok)) && printf '  %-24s %s\n' 'PAM' 'compile/link OK'

if ((${#missing_modules[@]})) || ((jpeg_ok == 0)) || ((pam_ok == 0)); then
    printf '\nDependency preflight failed.\n' >&2
    if ((${#missing_modules[@]})); then
        printf 'Missing pkg-config modules:\n' >&2
        printf '  %s\n' "${missing_modules[@]}" >&2
    fi
    ((jpeg_ok)) || printf 'libjpeg development headers/library are missing.\n' >&2
    ((pam_ok)) || printf 'PAM development headers/library are missing.\n' >&2
    cat >&2 <<'EOF'

Typical Ubuntu build packages are:
  sudo apt install \
    build-essential pkg-config dpkg-dev \
    libvncserver-dev libssl-dev nettle-dev libglib2.0-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libpipewire-0.3-dev libjpeg-dev libpam0g-dev

These are BUILD dependencies only. They are not copied into the binary .deb
Depends field.
EOF
    exit 1
fi

upstream_version="$(
    sed -nE 's/^#define[[:space:]]+VNC_MONITOR_VERSION[[:space:]]+"([^"]+)".*/\1/p' \
        include/config.h
)"

if [[ -z "$upstream_version" ]]; then
    echo "Could not read VNC_MONITOR_VERSION from include/config.h" >&2
    exit 1
fi

# Debian sorts '~' before the final release, so 0.1.0~beta.2 < 0.1.0.
deb_upstream_version="${upstream_version/-beta./~beta.}"
deb_upstream_version="${deb_upstream_version/-rc./~rc.}"
deb_version="${deb_upstream_version}-${DEB_REVISION}"
architecture="$(dpkg-architecture -qDEB_HOST_ARCH)"

printf '\nVersion:      %s -> %s\n' "$upstream_version" "$deb_version"
printf 'Architecture: %s\n' "$architecture"
printf 'Build jobs:   %s (nproc=%s)\n' "$JOBS" "$(nproc)"

printf '\n===== VNC MONITOR: COMPILE =====\n'
if ((CLEAN)); then
    make clean
fi
make -j"$JOBS" all auth-helper

work_dir="$(mktemp -d)"
stage="$work_dir/vnc-monitor"
mkdir -p "$stage/DEBIAN"

printf '\n===== VNC MONITOR: PACKAGE STAGE =====\n'

install -Dm0755 vnc-monitor \
    "$stage/usr/bin/vnc-monitor"
install -Dm0755 auth-helper/vnc-monitor-auth-helper \
    "$stage/usr/libexec/vnc-monitor-auth-helper"
install -Dm0644 config/vnc-monitor.conf \
    "$stage/etc/vnc-monitor/config.ini"
install -Dm0644 auth-helper/vnc-monitor.pam \
    "$stage/etc/pam.d/vnc-monitor"

# Package user service. Unlike the source-install unit, the executable is under
# /usr/bin and no explicit --config is needed: the daemon itself loads
# /etc/vnc-monitor/config.ini and then the user's optional override.
mkdir -p "$stage/usr/lib/systemd/user"
cat >"$stage/usr/lib/systemd/user/vnc-monitor.service" <<'EOF'
[Unit]
Description=VNC Monitor for the active GNOME Wayland session
Documentation=https://github.com/gig1910/vnc_ubuntu_virtual_monitor
PartOf=graphical-session.target
After=graphical-session.target pipewire.service
Wants=pipewire.service

[Service]
Type=simple
ExecStartPre=/usr/bin/mkdir -p %h/.config/vnc-monitor %h/.config/vnc-monitor-server %h/.cache/vnc-monitor
ExecStart=/usr/bin/vnc-monitor
Restart=on-failure
RestartSec=2s
TimeoutStopSec=10s
UMask=0077
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=%h/.config %h/.cache

[Install]
WantedBy=graphical-session.target
EOF
chmod 0644 "$stage/usr/lib/systemd/user/vnc-monitor.service"

# Generic system PAM socket. The socket is intentionally not tied to the user
# who built the package. The privileged helper enforces SO_PEERCRED and rejects
# authentication requests whose username differs from the connecting process'
# Unix account, so one local user cannot use the helper to authenticate another.
mkdir -p "$stage/usr/lib/systemd/system"
cat >"$stage/usr/lib/systemd/system/vnc-monitor-auth.socket" <<'EOF'
[Unit]
Description=VNC Monitor PAM authentication socket

[Socket]
ListenStream=/run/vnc-monitor-auth.sock
SocketMode=0666
Accept=yes
RemoveOnStop=yes

[Install]
WantedBy=sockets.target
EOF
chmod 0644 "$stage/usr/lib/systemd/system/vnc-monitor-auth.socket"

sed 's#/usr/local/libexec/vnc-monitor-auth-helper#/usr/libexec/vnc-monitor-auth-helper#' \
    auth-helper/vnc-monitor-auth@.service \
    >"$stage/usr/lib/systemd/system/vnc-monitor-auth@.service"
chmod 0644 "$stage/usr/lib/systemd/system/vnc-monitor-auth@.service"

install -Dm0644 README.md \
    "$stage/usr/share/doc/vnc-monitor/README.md"
install -Dm0644 CHANGELOG.md \
    "$stage/usr/share/doc/vnc-monitor/CHANGELOG.md"
install -Dm0644 SECURITY.md \
    "$stage/usr/share/doc/vnc-monitor/SECURITY.md"
install -Dm0644 LICENSE \
    "$stage/usr/share/doc/vnc-monitor/LICENSE"
install -Dm0644 docs/INSTALL.md \
    "$stage/usr/share/doc/vnc-monitor/INSTALL.md"
install -Dm0644 docs/ARCHITECTURE.md \
    "$stage/usr/share/doc/vnc-monitor/ARCHITECTURE.md"
install -Dm0644 docs/TROUBLESHOOTING.md \
    "$stage/usr/share/doc/vnc-monitor/TROUBLESHOOTING.md"

# Make dpkg preserve administrator edits to both system configuration files.
cat >"$stage/DEBIAN/conffiles" <<'EOF'
/etc/vnc-monitor/config.ini
/etc/pam.d/vnc-monitor
EOF

# Determine only the runtime shared-library dependencies from the compiled ELF
# files. No compiler, *-dev package, pkg-config or debhelper dependency is
# propagated into the finished binary package.
shlib_work="$work_dir/shlibdeps"
mkdir -p "$shlib_work/debian"
cat >"$shlib_work/debian/control" <<'EOF'
Source: vnc-monitor
Section: net
Priority: optional
Maintainer: VNC Monitor build <noreply@localhost>

Package: vnc-monitor
Architecture: any
Depends: ${shlibs:Depends}
Description: GNOME Wayland virtual monitor over VNC
 View-only virtual monitor server for GNOME Wayland sessions.
EOF

runtime_deps="$(
    cd "$shlib_work"
    dpkg-shlibdeps -O \
        -e"$stage/usr/bin/vnc-monitor" \
        -e"$stage/usr/libexec/vnc-monitor-auth-helper" \
        | sed -n 's/^shlibs:Depends=//p'
)"

if [[ -z "$runtime_deps" ]]; then
    echo "dpkg-shlibdeps did not produce runtime dependencies" >&2
    exit 1
fi

cat >"$stage/DEBIAN/control" <<EOF
Package: vnc-monitor
Version: $deb_version
Section: net
Priority: optional
Architecture: $architecture
Maintainer: $DEB_MAINTAINER
Depends: $runtime_deps, pipewire
Description: GNOME Wayland virtual monitor over VNC
 VNC Monitor exposes a real Mutter virtual monitor from the active GNOME
 Wayland session through a view-only RA2r VNC server. It uses PipeWire capture,
 PAM authentication, adaptive JPEG/CopyRect transport and strong single-connect
 session ownership. A GNOME Wayland session is required at runtime.
EOF

cat >"$stage/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e

if [ -d /run/systemd/system ]; then
    systemctl daemon-reload
    systemctl enable vnc-monitor-auth.socket >/dev/null
    systemctl restart vnc-monitor-auth.socket
fi

exit 0
EOF
chmod 0755 "$stage/DEBIAN/postinst"

cat >"$stage/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e

case "$1" in
    remove|deconfigure)
        if [ -d /run/systemd/system ]; then
            systemctl disable --now vnc-monitor-auth.socket >/dev/null 2>&1 || true
        fi
        ;;
esac

exit 0
EOF
chmod 0755 "$stage/DEBIAN/prerm"

cat >"$stage/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e

if [ -d /run/systemd/system ]; then
    systemctl daemon-reload || true
fi

exit 0
EOF
chmod 0755 "$stage/DEBIAN/postrm"

mkdir -p "$OUT_DIR"
out_file="$OUT_DIR/vnc-monitor_${deb_version}_${architecture}.deb"

printf '\n===== VNC MONITOR: BUILD .DEB =====\n'
dpkg-deb --root-owner-group --build "$stage" "$out_file"

printf '\n===== VNC MONITOR: PACKAGE INFO =====\n'
dpkg-deb --info "$out_file"
printf '\nRuntime Depends:\n  %s\n' "$runtime_deps"

printf '\nBuilt package:\n  %s\n' "$out_file"
printf '\nInstall with:\n  sudo apt install %q\n' "$out_file"
printf '\nThen, as the GNOME desktop user, enable the user service once:\n'
printf '  systemctl --user daemon-reload\n'
printf '  systemctl --user enable --now vnc-monitor.service\n'
printf '\nIf this machine still has the old ./install.sh source installation, remove\n'
printf 'its user/system unit overrides first (config and RA2 identity are preserved):\n'
printf '  make uninstall-service\n'
printf '  make uninstall-pam-service\n'
