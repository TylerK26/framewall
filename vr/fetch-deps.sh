#!/usr/bin/env bash
# Fetch framewall-vr's dependencies into vr/third_party at tested versions:
#   moonlight-common-c  Moonlight's streaming protocol library (GPL-3.0)
#   openvr-sdk          Valve's OpenVR headers and client library (BSD-3)
#
# Copyright (C) 2026 framewall contributors. GPL-3.0-or-later.
set -euo pipefail

MOONLIGHT_COMMON_C=e80a786afedeaccff63e839e68c8f3d8a057c13d
OPENVR=0924064316de3effbcd1acf1e309182a2deb1c05   # SDK 2.15.6

tp="$(cd "$(dirname "$0")" && pwd)/third_party"
mkdir -p "$tp"

fetch() {   # dir url commit
    local dir="$tp/$1"
    if [[ ! -d "$dir/.git" ]]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$2"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$3"
    git -C "$dir" checkout -q --force FETCH_HEAD
    git -C "$dir" submodule -q update --init --recursive --depth 1
    echo "$1: $(git -C "$dir" rev-parse --short HEAD)"
}
fetch moonlight-common-c https://github.com/moonlight-stream/moonlight-common-c.git "$MOONLIGHT_COMMON_C"
fetch openvr-sdk https://github.com/ValveSoftware/openvr.git "$OPENVR"
