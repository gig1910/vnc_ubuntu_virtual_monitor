#!/usr/bin/env bash
set -u

UUID='vnc-monitor-force-redraw@local'
EXT_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/gnome-shell/extensions/$UUID"
PROFILE="$HOME/.profile"

printf '%s\n' '===== REMOVE OBSOLETE REDRAW HELPER ====='
if command -v gnome-extensions >/dev/null 2>&1; then
    gnome-extensions disable "$UUID" >/dev/null 2>&1 || true
fi
rm -rf -- "$EXT_DIR"
printf 'removed: %s\n' "$EXT_DIR"

printf '%s\n' '===== REMOVE OBSOLETE MUTTER HW-CURSOR WORKAROUND ====='
if [ -f "$PROFILE" ] && grep -Eq '^[[:space:]]*export[[:space:]]+MUTTER_DEBUG_DISABLE_HW_CURSORS=1[[:space:]]*$' "$PROFILE"; then
    backup="$PROFILE.vnc-monitor-v19-backup-$(date +%Y%m%d-%H%M%S)"
    cp -a -- "$PROFILE" "$backup"
    sed -i -E '/^[[:space:]]*export[[:space:]]+MUTTER_DEBUG_DISABLE_HW_CURSORS=1[[:space:]]*$/d' "$PROFILE"
    printf 'removed exact workaround line from %s\nbackup: %s\n' "$PROFILE" "$backup"
else
    printf 'no exact workaround line found in %s\n' "$PROFILE"
fi

printf '%s\n' '===== SYSTEMD AUDIT (AUTH UNITS ARE EXPECTED) ====='
printf '%s\n' '-- system units --'
systemctl list-unit-files --no-pager 2>/dev/null | grep -Ei 'vnc-monitor|redraw' || true
printf '%s\n' '-- user units --'
systemctl --user list-unit-files --no-pager 2>/dev/null | grep -Ei 'vnc-monitor|redraw' || true

printf '%s\n' 'Cleanup complete.'
printf '%s\n' 'Keep vnc-monitor-auth.socket and vnc-monitor-auth@.service: V19 still uses PAM authentication.'
printf '%s\n' 'Log out/in once to make removal of MUTTER_DEBUG_DISABLE_HW_CURSORS effective in GNOME Shell.'
