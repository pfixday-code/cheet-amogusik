#!/usr/bin/env bash
# Builds the menu dex and libau_zygisk.so, then assembles out/au_zygisk-<version>.zip for Magisk.
# Needs: ANDROID_HOME (SDK with a platform and build-tools), an NDK, JDK 11+, python3.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_ROOT:-}}}"
[ -n "$NDK" ] && [ -x "$NDK/ndk-build" ] || { echo "set ANDROID_NDK_HOME to an NDK containing ndk-build" >&2; exit 1; }
for f in jni/Android.mk jni/Application.mk module/module.prop third_party/And64.cpp third_party/zygisk.hpp; do
    [ -f "$ROOT/$f" ] || { echo "missing $f in $ROOT" >&2; exit 1; }
done

bash "$ROOT/tools/build_dex.sh"

# And64InlineHook is arm64-only, so the ABI is forced here regardless of Application.mk.
"$NDK/ndk-build" -C "$ROOT" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=jni/Android.mk \
    NDK_APPLICATION_MK=jni/Application.mk APP_ABI=arm64-v8a -j"$(nproc 2>/dev/null || echo 4)"

SO="$ROOT/libs/arm64-v8a/libau_zygisk.so"
[ -f "$SO" ] || { echo "build finished but $SO is missing" >&2; exit 1; }

STAGE="$ROOT/build/module"
rm -rf "$STAGE"
mkdir -p "$STAGE/zygisk" "$STAGE/META-INF/com/google/android" "$ROOT/out"
cp "$ROOT/module/module.prop" "$STAGE/"
[ -f "$ROOT/module/customize.sh" ] && cp "$ROOT/module/customize.sh" "$STAGE/"
cp "$SO" "$STAGE/zygisk/arm64-v8a.so"

# Standard Magisk module installer (module_installer.sh from the Magisk repo).
cat > "$STAGE/META-INF/com/google/android/update-binary" <<'EOF'
#!/sbin/sh

#################
# Initialization
#################

umask 022

# echo before loading util_functions
ui_print() { echo "$1"; }

require_new_magisk() {
  ui_print "*******************************"
  ui_print " Please install Magisk v20.4+! "
  ui_print "*******************************"
  exit 1
}

#########################
# Load util_functions.sh
#########################

OUTFD=$2
ZIPFILE=$3

mount /data 2>/dev/null

[ -f /data/adb/magisk/util_functions.sh ] || require_new_magisk
. /data/adb/magisk/util_functions.sh
[ $MAGISK_VER_CODE -lt 20400 ] && require_new_magisk

install_module
exit 0
EOF
echo "#MAGISK" > "$STAGE/META-INF/com/google/android/updater-script"

VERSION="$(sed -n 's/^version=//p' "$ROOT/module/module.prop" | tr -d '\r')"
ZIP="$ROOT/out/au_zygisk-${VERSION:-dev}.zip"
python3 - "$STAGE" "$ZIP" <<'PY'
import os, sys, zipfile
stage, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for base, _, files in os.walk(stage):
        for name in sorted(files):
            path = os.path.join(base, name)
            z.write(path, os.path.relpath(path, stage).replace(os.sep, "/"))
print("module zip:", out)
PY
