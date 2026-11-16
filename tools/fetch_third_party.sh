#!/usr/bin/env bash
# Downloads the third-party sources into third_party/ when the original v2.2 archive is not at hand:
#   And64InlineHook (Rprop, pinned commit), Dear ImGui v1.91.9, zygisk.hpp (topjohnwu/zygisk-module-sample).
# Needs curl and tar. Existing files in third_party/ are overwritten.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

AND64_COMMIT="2787585191552b291c508e7ae639557164441a13"
IMGUI_TAG="v1.91.9"

mkdir -p "$TP/imgui/backends"

echo "And64InlineHook @ $AND64_COMMIT"
curl -fsSL "https://raw.githubusercontent.com/Rprop/And64InlineHook/$AND64_COMMIT/And64InlineHook.cpp" -o "$TP/And64.cpp"
curl -fsSL "https://raw.githubusercontent.com/Rprop/And64InlineHook/$AND64_COMMIT/And64InlineHook.hpp" -o "$TP/And64InlineHook.hpp"
# src/hook.h includes "And64.h"; the upstream header uses uintptr_t without including <cstdint>.
printf '#pragma once\n#include <cstdint>\n#include "And64InlineHook.hpp"\n' > "$TP/And64.h"

echo "zygisk.hpp (zygisk-module-sample, master)"
curl -fsSL "https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp" -o "$TP/zygisk.hpp"

echo "Dear ImGui $IMGUI_TAG"
curl -fsSL "https://github.com/ocornut/imgui/archive/refs/tags/$IMGUI_TAG.tar.gz" -o "$TMP/imgui.tgz"
tar -xzf "$TMP/imgui.tgz" -C "$TMP"
SRC="$(find "$TMP" -maxdepth 1 -type d -name 'imgui-*' -print -quit)"
cp "$SRC"/imgui.cpp "$SRC"/imgui.h "$SRC"/imgui_internal.h "$SRC"/imgui_draw.cpp "$SRC"/imgui_tables.cpp \
   "$SRC"/imgui_widgets.cpp "$SRC"/imconfig.h "$SRC"/imstb_rectpack.h "$SRC"/imstb_textedit.h \
   "$SRC"/imstb_truetype.h "$TP/imgui/"
cp "$SRC"/backends/imgui_impl_opengl3.cpp "$SRC"/backends/imgui_impl_opengl3.h \
   "$SRC"/backends/imgui_impl_opengl3_loader.h "$TP/imgui/backends/"
echo "third_party ready: $TP"
