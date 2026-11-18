#!/usr/bin/env bash
# Downloads sources required by the native build when the original v2.2 archive is not at hand:
#   And64InlineHook (Rprop, pinned commit), zygisk.hpp (topjohnwu/zygisk-module-sample).
# Needs curl and tar. Existing files in third_party/ are overwritten.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"
AND64_COMMIT="2787585191552b291c508e7ae639557164441a13"

echo "And64InlineHook @ $AND64_COMMIT"
curl -fsSL "https://raw.githubusercontent.com/Rprop/And64InlineHook/$AND64_COMMIT/And64InlineHook.cpp" -o "$TP/And64.cpp"
curl -fsSL "https://raw.githubusercontent.com/Rprop/And64InlineHook/$AND64_COMMIT/And64InlineHook.hpp" -o "$TP/And64InlineHook.hpp"
# src/hook.h includes "And64.h"; the upstream header uses uintptr_t without including <cstdint>.
printf '#pragma once\n#include <cstdint>\n#include "And64InlineHook.hpp"\n' > "$TP/And64.h"

echo "zygisk.hpp (zygisk-module-sample, master)"
curl -fsSL "https://raw.githubusercontent.com/topjohnwu/zygisk-module-sample/master/module/jni/zygisk.hpp" -o "$TP/zygisk.hpp"
echo "third_party ready: $TP"
