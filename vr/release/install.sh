#!/usr/bin/env bash
# Install framewall-vr from a release tarball. Run on the headset (Steam
# Frame), from the extracted folder:
#
#   ./install.sh [--prefix DIR]     default prefix: ~/.local/opt/framewall-vr
#
# Installs the binary with its OpenXR loader into the prefix, and the Steam
# launcher as ~/framewall-vr (add that to Steam as a non-Steam game). Running
# it again with a newer release updates in place; your settings
# (~/.config/framewall-vr.conf) are kept.
#
# Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
prefix="$HOME/.local/opt/framewall-vr"
while [[ $# -gt 0 ]]; do
    case "$1" in
    --prefix) prefix="$2"; shift 2 ;;
    *) echo "usage: $0 [--prefix DIR]" >&2; exit 2 ;;
    esac
done

if [[ "$(uname -m)" != aarch64 ]]; then
    echo "this build is for the Steam Frame (aarch64); this machine is $(uname -m)" >&2
    exit 1
fi

mkdir -p "$prefix"
install -m 755 "$here/framewall-vr" "$prefix/framewall-vr.new"
mv -f "$prefix/framewall-vr.new" "$prefix/framewall-vr"
install -m 644 "$here/libopenxr_loader.so.1" "$prefix/libopenxr_loader.so.1"
rm -f "$prefix/libopenvr_api.so"   # from the overlay version

launcher="$HOME/framewall-vr"
sed "s|^BIN=.*|BIN=\"$prefix/framewall-vr\"|" "$here/framewall-vr-launcher" > "$launcher.new"
chmod 755 "$launcher.new"
mv -f "$launcher.new" "$launcher"

# The binary uses SteamOS's own libraries; say so if an update removed one.
missing=$(ldd "$prefix/framewall-vr" 2>/dev/null | awk '/not found/ {print $1}')
if [[ -n "$missing" ]]; then
    echo "warning: libraries not found on this system:" $missing >&2
    echo "         build from source instead (vr/build.sh in the framewall repo)" >&2
fi

echo "installed: $prefix/framewall-vr"
echo "launcher:  $launcher"
echo
echo "In Desktop mode, right click ~/framewall-vr, click \"Add to Steam\","
echo "then launch it from Steam. Set its refresh rate in SteamVR's video settings."
