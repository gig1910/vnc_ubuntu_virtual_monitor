#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

JOBS="${JOBS:-20}"
CLEAN=0

usage() {
    cat <<'EOF'
Usage: ./install.sh [--clean] [--jobs N]

Build and install the complete VNC Monitor beta stack:
  - vnc-monitor binary;
  - vnc-monitor.service (systemd user service);
  - PAM auth helper;
  - vnc-monitor-auth.socket + vnc-monitor-auth@.service (system units).

Options:
  --clean       Run make clean before building.
  -j, --jobs N  Parallel build jobs (default: 20; also JOBS=N).
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

if ((EUID == 0)); then
    cat >&2 <<'EOF'
Do not run install.sh as root.
The main daemon is a systemd user service and must belong to the logged-in
GNOME Wayland user. sudo is requested internally only for PAM/system units.
EOF
    exit 1
fi

for command_name in make systemctl sudo install; do
    if ! command -v "$command_name" >/dev/null 2>&1; then
        echo "Required command not found: $command_name" >&2
        exit 1
    fi
done

trap 'rc=$?; echo "Installation failed (line $LINENO, exit $rc)" >&2; exit "$rc"' ERR

printf '\n===== VNC MONITOR: BUILD =====\n'
printf 'Source: %s\n' "$ROOT_DIR"
printf 'Jobs:   %s\n' "$JOBS"

if ((CLEAN)); then
    printf '\n-- Clean build requested --\n'
    make clean
fi

# Compile everything before asking for sudo.  pam-service also renders the
# socket unit with the invoking desktop user's uid/gid ownership settings.
make -j"$JOBS" all auth-helper pam-service

printf '\n===== VNC MONITOR: INSTALL =====\n'
# This target installs both privilege domains.  It deliberately runs as the
# desktop user; only its PAM/system-unit sub-steps invoke sudo themselves.
make install-service

# enable --now is a no-op for an already-running unit.  Explicit restarts make
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
