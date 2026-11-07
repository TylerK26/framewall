#!/usr/bin/env bash
# Build Sunshine with framewall's "workspace" capture patch and install it to
# a private prefix (default ~/.local/opt/sunshine-wall). Your normal Sunshine
# package is not touched.
#
#   sunshine/build.sh [--prefix DIR] [--src DIR] [--cuda]
#
# The patch adds `output_name = workspace` to Sunshine's KWin capture: the
# bounding box of all screens as one stream (KWin's stream_region).
#
# Build dependencies (Arch/CachyOS names; see Sunshine's build docs for
# others): base-devel cmake ninja git boost nlohmann-json nodejs npm shaderc
# python-jinja python-setuptools libva libdrm libpipewire libpulse opus
# miniupnpc libcap libevdev libnotify libayatana-appindicator numactl
# wayland libx11 libxcb libxfixes libxrandr libxtst mesa vulkan-icd-loader
# qt6-base qt6-svg curl openssl icu avahi. Add `cuda` for --cuda (NVENC).
#
# Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
set -euo pipefail

# Tested Sunshine commit (the patch applies to it as-is).
SUNSHINE_COMMIT=019b1ba661d829e334706557a3e2bdc3d2374591
SUNSHINE_REPO=https://github.com/LizardByte/Sunshine.git

here="$(cd "$(dirname "$0")" && pwd)"
prefix="$HOME/.local/opt/sunshine-wall"
src="$HOME/.cache/framewall/sunshine-src"
cuda=OFF
while [[ $# -gt 0 ]]; do
    case "$1" in
    --prefix) prefix="$2"; shift 2 ;;
    --src) src="$2"; shift 2 ;;
    --cuda) cuda=ON; shift ;;
    *) echo "usage: $0 [--prefix DIR] [--src DIR] [--cuda]" >&2; exit 2 ;;
    esac
done

if [[ ! -d "$src/.git" ]]; then
    git clone "$SUNSHINE_REPO" "$src"
fi
cd "$src"
git fetch --quiet origin "$SUNSHINE_COMMIT" 2>/dev/null || true
git checkout --quiet --force "$SUNSHINE_COMMIT"
git submodule update --init --recursive --depth 1
git -c user.name=framewall -c user.email=framewall@localhost am --quiet --3way "$here/0001-kwingrab-workspace-capture.patch" ||
    { git am --abort 2>/dev/null; echo "patch did not apply" >&2; exit 1; }

# Use the distribution's Boost instead of the exact pinned version.
sed -i 's/find_package(Boost CONFIG ${BOOST_VERSION} EXACT COMPONENTS ${BOOST_COMPONENTS})/find_package(Boost CONFIG ${BOOST_VERSION} COMPONENTS ${BOOST_COMPONENTS})/' \
    cmake/dependencies/Boost_Sunshine.cmake

export BRANCH=framewall COMMIT="$(git rev-parse HEAD)" BUILD_VERSION="framewall-${SUNSHINE_COMMIT:0:7}"
cmake -S . -B build -G Ninja -Wno-dev \
    -D BUILD_DOCS=OFF -D BUILD_TESTS=OFF -D BUILD_WERROR=OFF -D BOOST_USE_STATIC=OFF \
    -D CMAKE_BUILD_TYPE=Release -D CMAKE_INSTALL_PREFIX="$prefix" \
    -D SUNSHINE_ASSETS_DIR=share/sunshine -D SUNSHINE_EXECUTABLE_PATH="$prefix/bin/sunshine" \
    -D SUNSHINE_ENABLE_CUDA=$cuda -D CUDA_FAIL_ON_MISSING=OFF \
    -D SUNSHINE_ENABLE_DRM=ON -D SUNSHINE_ENABLE_PORTAL=ON -D SUNSHINE_ENABLE_TRAY=ON \
    -D SUNSHINE_ENABLE_VAAPI=ON -D SUNSHINE_ENABLE_VULKAN=ON -D SUNSHINE_ENABLE_WAYLAND=ON \
    -D SUNSHINE_ENABLE_X11=ON -D SUNSHINE_PUBLISHER_NAME=framewall
cmake --build build -j "$(nproc)"

# Install into a staging dir and copy only the prefix: the full install also
# writes system files (udev rules, systemd units) that the regular Sunshine
# package already provides.
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
DESTDIR="$stage" cmake --install build >/dev/null
mkdir -p "$prefix"
cp -a "$stage$prefix/." "$prefix/"
echo "installed: $prefix/bin/sunshine"
"$prefix/bin/sunshine" --version 2>&1 | grep -m1 -i version || true
