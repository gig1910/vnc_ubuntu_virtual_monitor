#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

JOBS="${JOBS:-}"
CLEAN=0

usage() {
    cat <<'EOF'
Usage: ./install.sh [--clean] [--jobs N]

Build and install the complete VNC Monitor beta stack:
  - vnc-monitor binary;
  - vnc-monitor.service (systemd user service);
  - PAM auth helper;
  - vnc-monitor-auth.socket + vnc-monitor-auth@.service (system units).

Before building, the installer verifies all required build tools and libraries.

Options:
  --clean       Run make clean before building.
  -j, --jobs N  Parallel build jobs (default: nproc; also JOBS=N).
  -h, --help    Show this help.

Run this script as the logged-in GNOME desktop user, not as root.
The script invokes sudo only for the PAM helper/systemd system units.
EOF
}

while (($#)); do
    case "$1" in
        --clean)
            CLEAN=1
            shift
            ;;
        -j|--jobs)
            if (($# < 2)); then
                echo "Missing value for $1" >&2
                exit 2
            fi
            JOBS="$2"
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

if ((EUID == 0)); then
    cat >&2 <<'EOF'
Do not run install.sh as root.
The main daemon is a systemd user service and must belong to the logged-in
GNOME Wayland user. sudo is requested internally only for PAM/system units.
EOF
    exit 1
fi

trap 'rc=$?; echo "Installation failed (line $LINENO, exit $rc)" >&2; exit "$rc"' ERR

printf '\n===== VNC MONITOR: DEPENDENCY PREFLIGHT =====\n'

required_commands=(
    make
    cc
    pkg-config
    nproc
    mktemp
    install
    sed
    systemctl
    sudo
)

missing_commands=()
for command_name in "${required_commands[@]}"; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        missing_commands+=("$command_name")
    fi
done

if ((${#missing_commands[@]})); then
    printf 'Missing required commands:\n' >&2
    printf '  %s\n' "${missing_commands[@]}" >&2
    cat >&2 <<'EOF'

On Ubuntu, start with:
  sudo apt install build-essential pkg-config
EOF
    exit 1
fi

if [[ -z "$JOBS" ]]; then
    JOBS="$(nproc)"
fi

case "$JOBS" in
    ''|*[!0-9]*)
        echo "Invalid job count: $JOBS" >&2
        exit 2
        ;;
esac

if ((JOBS < 1)); then
    echo "Job count must be at least 1" >&2
    exit 2
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
cleanup_probe() {
    rm -rf "$probe_dir"
}
trap cleanup_probe EXIT

cat >"$probe_dir/jpeg.c" <<'EOF'
#include <stddef.h>
#include <stdio.h>
#include <jpeglib.h>

int main(void)
{
    struct jpeg_error_mgr error;
    return jpeg_std_error(&error) ? 0 : 1;
}
EOF

jpeg_ok=1
if ! cc "$probe_dir/jpeg.c" -o "$probe_dir/jpeg" -ljpeg >/dev/null 2>&1; then
    jpeg_ok=0
fi

cat >"$probe_dir/pam.c" <<'EOF'
#include <security/pam_appl.h>

int main(void)
{
    pam_handle_t *pamh = 0;
    (void)pamh;
    return PAM_SUCCESS;
}
EOF

pam_ok=1
if ! cc "$probe_dir/pam.c" -o "$probe_dir/pam" -lpam >/dev/null 2>&1; then
    pam_ok=0
fi

if ((jpeg_ok)); then
    printf '  %-24s %s\n' 'libjpeg' 'compile/link OK'
fi

if ((pam_ok)); then
    printf '  %-24s %s\n' 'PAM' 'compile/link OK'
fi

if ((${#missing_modules[@]})) || ((jpeg_ok == 0)) || ((pam_ok == 0)); then
    printf '\nDependency preflight failed.\n' >&2

    if ((${#missing_modules[@]})); then
        printf 'Missing pkg-config modules:\n' >&2
        printf '  %s\n' "${missing_modules[@]}" >&2
    fi

    if ((jpeg_ok == 0)); then
        printf 'libjpeg development headers/library are missing or not linkable.\n' >&2
    fi

    if ((pam_ok == 0)); then
        printf 'PAM development headers/library are missing or not linkable.\n' >&2
    fi

    cat >&2 <<'EOF'

Typical Ubuntu development packages for this project are:
  sudo apt install \
    build-essential pkg-config \
    libvncserver-dev libssl-dev nettle-dev libglib2.0-dev \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libpipewire-0.3-dev libjpeg-dev libpam0g-dev

No build or service installation was attempted.
EOF
    exit 1
fi

if ! systemctl --user show-environment >/dev/null 2>&1; then
    cat >&2 <<'EOF'
The systemd user manager is not reachable from this shell.
Run install.sh from the logged-in GNOME desktop user's session.
No build or service installation was attempted.
EOF
    exit 1
fi

printf '\nAll build/runtime installation prerequisites passed.\n'
printf 'Parallel build jobs: %s (nproc=%s)\n' "$JOBS" "$(nproc)"

printf '\n===== VNC MONITOR: BUILD =====\n'
printf 'Source: %s\n' "$ROOT_DIR"
printf 'Jobs:   %s\n' "$JOBS"

if ((CLEAN)); then
    printf '\n-- Clean build requested --\n'
    make clean
fi

# Compile everything before asking for sudo. pam-service also renders the
# socket unit with the invoking desktop user's uid/gid ownership settings.
make -j"$JOBS" all auth-helper pam-service

printf '\n===== VNC MONITOR: INSTALL =====\n'
# This target installs both privilege domains. It deliberately runs as the
# desktop user; only its PAM/system-unit sub-steps invoke sudo themselves.
make install-service

# enable --now is a no-op for an already-running unit. Explicit restarts make
# upgrades deterministic: the newly installed helper/unit and daemon binary
# are the versions actually serving the next connection.
printf '\n===== VNC MONITOR: ACTIVATE INSTALLED VERSION =====\n'
sudo systemctl restart vnc-monitor-auth.socket
systemctl --user restart vnc-monitor.service

printf '\n===== VNC MONITOR: FINAL STATUS =====\n'
make status-support

printf '\nInstallation complete.\n'
printf 'User daemon:   vnc-monitor.service\n'
printf 'System auth:   vnc-monitor-auth.socket -> vnc-monitor-auth@.service\n'
printf 'Viewer port:   TCP/5901\n'
printf 'Live log:      journalctl --user -u vnc-monitor.service -f\n'
