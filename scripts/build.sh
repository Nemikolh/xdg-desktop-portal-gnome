#!/usr/bin/env bash
# Build xdg-desktop-portal-gnome into build/.
#
# Run this where the build dependencies are available (the ubuntu:resolute
# container): the binary must link against the same library versions as the
# host it will be installed on.
#
# Usage: scripts/build.sh [--deps]
#   --deps   apt-get install the build dependencies first (needs root or sudo)

set -euo pipefail

cd "$(dirname "$0")/.."

DEPS=(
  build-essential git pkg-config meson ninja-build
  libglib2.0-dev libgtk-4-dev libadwaita-1-dev libgnome-desktop-4-dev
  gsettings-desktop-schemas-dev xdg-desktop-portal-dev libfontconfig-dev systemd-dev
  libpipewire-0.3-dev libwayland-dev wayland-protocols libx11-dev
)

install_deps=false
for arg in "$@"; do
  case "$arg" in
    --deps) install_deps=true ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

. /etc/os-release
if [ "${VERSION_CODENAME:-}" != "resolute" ]; then
  echo "Expected Ubuntu 26.04 (resolute), found ${PRETTY_NAME:-unknown}." >&2
  echo "The binary has to match the host's libraries; build in ubuntu:resolute." >&2
  exit 1
fi

if $install_deps; then
  sudo=""
  [ "$(id -u)" -eq 0 ] || sudo="sudo"
  $sudo apt-get update
  DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y "${DEPS[@]}"
fi

if [ ! -f build/build.ninja ]; then
  # --prefix=/usr matches the paths the system package uses
  meson setup build --prefix=/usr --buildtype=debugoptimized
fi

ninja -C build

echo
echo "Built: $(pwd)/build/src/xdg-desktop-portal-gnome"
