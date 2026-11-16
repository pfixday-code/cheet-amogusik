#!/usr/bin/env bash
# Compiles java/au/menu/Menu.java to classes.dex and embeds it as src/menu_dex.h.
# Run before ndk-build whenever Menu.java changes. Needs JDK 11+, Android SDK and python3; bash on Linux/macOS or Git Bash on Windows.
set -euo pipefail

: "${ANDROID_HOME:?set ANDROID_HOME to your Android SDK path}"
# Newest installed numeric platform unless ANDROID_API is given.
API="${ANDROID_API:-$(ls "$ANDROID_HOME/platforms" 2>/dev/null | sed -n 's/^android-\([0-9][0-9]*\)$/\1/p' | sort -n | tail -n1)}"
BT="${BUILD_TOOLS:-$(ls "$ANDROID_HOME/build-tools" 2>/dev/null | sort -V | tail -n1)}"
[ -n "$API" ] || { echo "no platforms/android-NN in $ANDROID_HOME" >&2; exit 1; }
[ -n "$BT" ] || { echo "no build-tools in $ANDROID_HOME" >&2; exit 1; }
JAR="$ANDROID_HOME/platforms/android-$API/android.jar"
D8="$ANDROID_HOME/build-tools/$BT/d8"
[ -f "$D8" ] || D8="$D8.bat"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/menu"

[ -f "$JAR" ] || { echo "missing $JAR (install platform android-$API or set ANDROID_API)" >&2; exit 1; }
[ -f "$D8" ] || { echo "missing d8 in build-tools $BT" >&2; exit 1; }
echo "dex: platform android-$API, build-tools $BT"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"
javac --release 8 -encoding UTF-8 -classpath "$JAR" -d "$OUT/classes" "$ROOT/java/au/menu/Menu.java"
mapfile -t CLASSES < <(find "$OUT/classes" -name '*.class')
"$D8" --release --min-api 26 --lib "$JAR" --output "$OUT/dex" "${CLASSES[@]}"
python3 "$ROOT/tools/bin2h.py" "$OUT/dex/classes.dex" "$ROOT/src/menu_dex.h"
