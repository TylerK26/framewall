#!/usr/bin/env bash
# Build a framewall-vr release tarball for a GitHub release. Run on the
# headset (Steam Frame), from a clean copy of the framewall repo:
#
#   vr/package.sh [--version VER] [--out DIR]
#
#   --version  release version (default: `git describe --tags --always`)
#   --out      where the tarball goes (default: vr/dist)
#
# Builds from scratch in vr/build-release with the pinned dependencies, then
# writes framewall-vr-VER-steamos-aarch64.tar.gz and its .sha256. The tarball
# holds the stripped binary, the OpenXR loader, the launcher, an installer,
# the licenses and the exact source versions. It does not install anything.
#
# Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/.." && pwd)"
version=""
out="$here/dist"
while [[ $# -gt 0 ]]; do
    case "$1" in
    --version) version="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    *) echo "usage: $0 [--version VER] [--out DIR]" >&2; exit 2 ;;
    esac
done

arch="$(uname -m)"
[[ "$arch" == aarch64 ]] || echo "warning: building on $arch, not the Steam Frame's aarch64" >&2

commit="$(git -C "$repo" rev-parse HEAD)"
if [[ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]]; then
    echo "the repo has uncommitted changes; a release must match a commit" >&2
    git -C "$repo" status --short --untracked-files=no >&2
    exit 1
fi
[[ -n "$version" ]] || version="$(git -C "$repo" describe --tags --always)"

# The dependencies must be the versions fetch-deps.sh pins.
[[ -f "$here/third_party/openxr-sdk/CMakeLists.txt" && -f "$here/third_party/moonlight-common-c/CMakeLists.txt" ]] ||
    "$here/fetch-deps.sh"
pin() { sed -n "s/^$1=\([0-9a-f]*\).*/\1/p" "$here/fetch-deps.sh"; }
for dep in moonlight-common-c:MOONLIGHT_COMMON_C openxr-sdk:OPENXR; do
    dir="${dep%%:*}" want="$(pin "${dep#*:}")"
    have="$(git -C "$here/third_party/$dir" rev-parse HEAD)"
    if [[ "$have" != "$want" ]]; then
        echo "third_party/$dir is at $have, fetch-deps.sh pins $want: run vr/fetch-deps.sh" >&2
        exit 1
    fi
done

build="$here/build-release"
rm -rf "$build"
cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build" -j "$(nproc)"

name="framewall-vr-$version-steamos-$arch"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
pkg="$stage/$name"
mkdir -p "$pkg/LICENSES"

install -m 755 "$build/framewall-vr" "$pkg/framewall-vr"
install -m 644 "$(readlink -f "$build/openxr/src/loader/libopenxr_loader.so.1")" "$pkg/libopenxr_loader.so.1"
strip --strip-unneeded "$pkg/framewall-vr" "$pkg/libopenxr_loader.so.1"
install -m 755 "$repo/client/framewall-vr" "$pkg/framewall-vr-launcher"
install -m 755 "$here/release/install.sh" "$pkg/install.sh"
install -m 644 "$repo/client/framewall-vr.conf.example" "$pkg/framewall-vr.conf.example"
sed "s/@VERSION@/$version/g" "$here/release/README.txt" > "$pkg/README.txt"

tp="$here/third_party"
install -m 644 "$repo/LICENSE" "$pkg/LICENSES/framewall-GPL-3.0.txt"
install -m 644 "$tp/moonlight-common-c/LICENSE.txt" "$pkg/LICENSES/moonlight-common-c-GPL-3.0.txt"
install -m 644 "$tp/moonlight-common-c/enet/LICENSE" "$pkg/LICENSES/enet-MIT.txt"
install -m 644 "$tp/moonlight-common-c/nanors/LICENSE" "$pkg/LICENSES/nanors.txt"
install -m 644 "$tp/openxr-sdk/LICENSE" "$pkg/LICENSES/openxr-loader-Apache-2.0.txt"

# Submodule commits as moonlight-common-c records them.
sub() { git -C "$tp/moonlight-common-c" ls-tree HEAD "$1" | awk '{print $3}'; }
cat > "$pkg/SOURCES.txt" <<EOF
framewall-vr $version, built $(date -u +%Y-%m-%d) on $(. /etc/os-release && echo "$PRETTY_NAME ${VERSION_ID:-} build ${BUILD_ID:-?}") ($arch)

framewall           https://github.com/TylerK26/framewall  $commit
moonlight-common-c  https://github.com/moonlight-stream/moonlight-common-c  $(pin MOONLIGHT_COMMON_C)
  enet              (submodule)  $(sub enet)
  nanors            (submodule)  $(sub nanors)
OpenXR-SDK          https://github.com/KhronosGroup/OpenXR-SDK  $(pin OPENXR)

To rebuild: check out the framewall commit above and run vr/build.sh on the
headset (vr/fetch-deps.sh fetches the dependency versions listed here).

Uses these SteamOS libraries at run time: $(readelf -d "$pkg/framewall-vr" "$pkg/libopenxr_loader.so.1" |
    sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | grep -v '^libopenxr_loader' | sort -u | tr '\n' ' ')
EOF

mkdir -p "$out"
tar -C "$stage" --owner=0 --group=0 --numeric-owner -czf "$out/$name.tar.gz" "$name"
(cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "release: $out/$name.tar.gz"
cat "$out/$name.tar.gz.sha256"
