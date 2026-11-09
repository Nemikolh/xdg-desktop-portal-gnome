#!/usr/bin/env bash
# Install the built xdg-desktop-portal-gnome on the host in place of the
# system one, or put the system one back.
#
# The packaged binary is moved aside with dpkg-divert, so package upgrades
# won't overwrite the installed build, and --uninstall restores it.
#
# Usage: scripts/install.sh [--uninstall]
#
# Run as your desktop user (not root); sudo is used for the system paths and
# the user service is restarted in your session.

set -euo pipefail

cd "$(dirname "$0")/.."

TARGET=/usr/libexec/xdg-desktop-portal-gnome
DIVERTED="$TARGET.distrib"
BINARY=build/src/xdg-desktop-portal-gnome
SERVICE=xdg-desktop-portal-gnome.service

is_diverted() {
  dpkg-divert --list "$TARGET" | grep -q .
}

restart_service() {
  echo "Restarting $SERVICE"
  systemctl --user restart "$SERVICE"
  sleep 1
  if systemctl --user is-active --quiet "$SERVICE"; then
    echo "$SERVICE is running."
  else
    echo "$SERVICE failed to start; see: journalctl --user -u $SERVICE -b" >&2
    exit 1
  fi
}

install_build() {
  if [ ! -x "$BINARY" ]; then
    echo "$BINARY not found; run scripts/build.sh first." >&2
    exit 1
  fi

  missing=$(ldd "$BINARY" | grep "not found" || true)
  if [ -n "$missing" ]; then
    echo "The build needs libraries this system lacks:" >&2
    echo "$missing" >&2
    exit 1
  fi

  # Copy next to the target first, so the path is never missing for long
  sudo install -m 0755 "$BINARY" "$TARGET.new"

  if ! is_diverted; then
    sudo dpkg-divert --local --rename --divert "$DIVERTED" --add "$TARGET"
  fi

  sudo mv -f "$TARGET.new" "$TARGET"
  echo "Installed $BINARY to $TARGET (system binary kept as $DIVERTED)."

  restart_service
}

uninstall_build() {
  if ! is_diverted; then
    echo "No installed build found ($TARGET is not diverted); nothing to do."
    exit 0
  fi

  sudo rm -f "$TARGET"
  sudo dpkg-divert --rename --remove "$TARGET"
  echo "Restored the system binary to $TARGET."

  restart_service
}

require_user() {
  if [ "$(id -u)" -eq 0 ]; then
    echo "Run as your desktop user, not root: the portal is a per-user service." >&2
    exit 1
  fi
}

case "${1:-}" in
  "") require_user; install_build ;;
  --uninstall) require_user; uninstall_build ;;
  -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' ;;
  *) echo "Unknown argument: $1" >&2; exit 1 ;;
esac
