# Among Us Zygisk Menu 2.3.0

Zygisk module for `com.innersloth.spacemafia` (arm64). A floating Java menu (works on GLES and
Vulkan) plus the original ImGui overlay (GLES only). Features: speed multiplier, vision radius,
noclip, local kill cooldown, class dump of Assembly-CSharp.

Field offsets and method pointers are resolved at runtime through the il2cpp API by name
(`PlayerControl.MyPhysics`, `Collider`, `killTimer`, `PlayerPhysics.Speed`,
`PlayerControl.FixedUpdate`, `ShipStatus.CalculateLightRadius`); nothing build-specific is hardcoded.

## Layout

| Path | What |
|---|---|
| `src/` | native code: Zygisk entry, il2cpp resolver, ELF/GOT helpers, hooks, features, ImGui overlay, Java bridge |
| `java/au/menu/Menu.java` | the menu UI, compiled to dex and embedded in the `.so` |
| `jni/` | ndk-build files (`Android.mk`, `Application.mk`, `exports.map`) |
| `module/` | Magisk `module.prop` and `customize.sh` |
| `tools/` | `build_dex.sh`, `bin2h.py`, `package_module.sh`, `prepare_tree.sh`, `fetch_third_party.sh` |
| `.github/workflows/build.yml` | GitHub Actions build that uploads the module zip as an artifact |
| `third_party/` | not included: And64InlineHook, Dear ImGui, zygisk.hpp (from the v2.2 archive or `tools/fetch_third_party.sh`) |

## Build on GitHub (no local toolchain)

Push this tree (and optionally `au_zygisk_source_v2.2.tgz`) to the repo. Actions runs
**Build Magisk module**; download the `au_zygisk-magisk` artifact, it contains
`au_zygisk-2.3.0.zip`.

## Build locally

Needs Android SDK (a platform + build-tools), an NDK, JDK 11+, python3, curl.

```bash
export ANDROID_HOME=/path/to/Android/Sdk
export ANDROID_NDK_HOME=/path/to/ndk
bash tools/fetch_third_party.sh      # skip if third_party/ is already filled from the v2.2 archive
bash tools/package_module.sh         # -> out/au_zygisk-2.3.0.zip
```

Install the zip in Magisk, enable Zygisk, reboot. Logs: `adb logcat -s AUMod`,
`/data/data/com.innersloth.spacemafia/files/au_report.txt`,
`/sdcard/Android/data/com.innersloth.spacemafia/files/au_report.txt`.

## Provenance notes

- `jni/Application.mk` and `jni/exports.map` were recreated (the originals could not be read);
  if you have the originals from the v2.2 archive, they take precedence in CI.
- `fetch_third_party.sh` pins And64InlineHook to commit `2787585` and ImGui to `v1.91.9`
  (needs >= 1.91.1 for `ImGuiChildFlags_Borders`). The v2.2 archive may carry other versions.
