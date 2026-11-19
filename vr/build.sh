#!/usr/bin/env bash
# Build framewall-vr and install it. Run on the headset (Steam Frame), from a
# copy of the framewall repo:
#
#   vr/build.sh [--prefix DIR]     default prefix: ~/.local/opt/framewall-vr
#
# Installs the binary with its OpenXR loader into the prefix, and the Steam
# launcher as ~/framewall-vr (add that to Steam as a non-Steam game).
#
# Needs: cmake, a C/C++ compiler, pkg-config, glslc and the development files
# for Vulkan, SDL3, libcurl, opus, libdrm and OpenSSL (all present on SteamOS
# for the Steam Frame).
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

[[ -f "$here/third_party/openxr-sdk/CMakeLists.txt" && -f "$here/third_party/moonlight-common-c/CMakeLists.txt" ]] ||
    "$here/fetch-deps.sh"

build="$here/build"
cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j "$(nproc)"

mkdir -p "$prefix"
install -m 755 "$build/framewall-vr" "$prefix/framewall-vr.new"
mv -f "$prefix/framewall-vr.new" "$prefix/framewall-vr"
# The OpenXR loader, under the name the binary asks for.
loader=$(readlink -f "$build/openxr/src/loader/libopenxr_loader.so.1")
install -m 644 "$loader" "$prefix/libopenxr_loader.so.1"
rm -f "$prefix/libopenvr_api.so"   # from the overlay version

launcher="$HOME/framewall-vr"
sed "s|^BIN=.*|BIN=\"$prefix/framewall-vr\"|" "$here/../client/framewall-vr" > "$launcher.new"
chmod 755 "$launcher.new"
mv -f "$launcher.new" "$launcher"
echo "installed: $prefix/framewall-vr"
echo "launcher:  $launcher  (add it to Steam as a non-Steam game)"
