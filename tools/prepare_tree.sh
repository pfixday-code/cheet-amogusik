#!/usr/bin/env bash
# CI helper: builds a complete source tree in build/tree.
#  - If au_zygisk_source_v2.2.tgz is in the repo, it is unpacked first (it carries third_party/).
#  - The repo's own src/, java/, tools/, jni/, module/, third_party/ are laid over it.
#  - If third_party/ is still missing, tools/fetch_third_party.sh downloads it.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ARCHIVE="$REPO/au_zygisk_source_v2.2.tgz"
WORK="$REPO/build/unpacked"
TREE="$REPO/build/tree"

rm -rf "$WORK" "$TREE"
mkdir -p "$WORK" "$TREE"

if [ -f "$ARCHIVE" ]; then
    tar -xzf "$ARCHIVE" -C "$WORK"
    MK="$(find "$WORK" -path '*/jni/Android.mk' -print -quit)"
    if [ -n "$MK" ]; then
        cp -a "$(dirname "$(dirname "$MK")")/." "$TREE/"
    else
        echo "warning: no jni/Android.mk inside $ARCHIVE, using repo files only" >&2
    fi
fi

for d in src java tools jni module third_party; do
    if [ -d "$REPO/$d" ]; then
        mkdir -p "$TREE/$d"
        cp -a "$REPO/$d/." "$TREE/$d/"
    fi
done

if [ ! -f "$TREE/third_party/And64.cpp" ] || [ ! -f "$TREE/third_party/imgui/imgui.cpp" ] || [ ! -f "$TREE/third_party/zygisk.hpp" ]; then
    echo "third_party incomplete, downloading"
    bash "$TREE/tools/fetch_third_party.sh"
fi
echo "tree ready: $TREE"
