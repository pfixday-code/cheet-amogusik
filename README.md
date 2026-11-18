# Among Us Zygisk Menu 2.7.1

Zygisk module for `com.innersloth.spacemafia` (arm64). The single UI is a Java floating menu,
working independently of Unity's GLES or Vulkan renderer. The legacy ImGui overlay and Unity input
hook are not compiled or started.

Features are resolved through il2cpp at runtime and disabled when a required class, field or method
does not match the installed game build. Multipliers and toggles are saved across restarts.

- Movement/vision: speed, Noclip with wall-shadow visibility, safe `FollowerCamera` zoom,
  Wall vision and light-radius multiplier.
- Information: impostor name colors, ghosts and local-only invisibility (other players still see you).
- Local impostor timer and an il2cpp class dump.
- Player selection by player id: teleport, bring player (host/Freeplay only), local camera spectate
  and return, report a selected dead body, complete owned tasks.

Networked actions use regular game RPCs. Server ownership and host corrections still apply:
teleport and bring-player are not a bypass of authoritative online position checks, and spectate is
purely local. Commands are queued from JNI to the next local `PlayerControl.FixedUpdate`; Unity
objects never cross into the Android UI thread. The snapshots contain copied ids, names and flags.

The world zoom takes the camera from `HudManager.PlayerCam` using
`Component.GetComponent(typeof(Camera))`, not `Camera.main` or the HUD `UICamera`; it restores its
baseline during meetings/minigames and when switched off. Noclip watches `Collider.enabled` so game
code that re-enables collision after death, voting or a scene change cannot silently disable it.
Wall shadows are hidden through `HudManager.ShadowQuad` and restored to their prior `activeSelf`
state when switched off.

There is no guarantee of “anti-ban” or undetectability. Safety is limited to API-version checks,
normal game RPC boundaries, host/Freeplay guards for Bring-player and a 750 ms player-action rate
limit. Do not use player actions to disrupt other people's games or to provoke moderation.

## Layout

| Path | What |
|---|---|
| `src/` | Zygisk entry, il2cpp resolver, hooks, features and JNI bridge |
| `java/au/menu/Menu.java` | Java menu, compiled to dex and embedded in the `.so` |
| `jni/` | ndk-build files (`Android.mk`, `Application.mk`, `exports.map`) |
| `module/` | Magisk metadata and installer customization |
| `tools/` | SDK/dex, native build, module packaging and third-party fetch scripts |
| `.github/workflows/build.yml` | GitHub Actions build that uploads the module zip as an artifact |
| `third_party/` | generated: And64InlineHook and zygisk.hpp |

## Build on GitHub

Push `main`, wait for **Build Magisk module** and download `au_zygisk-magisk`. It contains
`au_zygisk-2.7.1.zip`. `tools/prepare_tree.sh` restores third-party files when needed.

## Build locally

Needs Android SDK (a platform + build-tools), an NDK, JDK 11+, Python 3 and curl.

```bash
export ANDROID_HOME=/path/to/Android/Sdk
export ANDROID_NDK_HOME=/path/to/ndk
bash tools/prepare_tree.sh
bash build/tree/tools/package_module.sh
```

Install `out/au_zygisk-2.7.1.zip` in Magisk, enable Zygisk and reboot. Logs: `adb logcat -s AUMod` and
`/sdcard/Android/data/com.innersloth.spacemafia/files/au_report.txt`.

And64InlineHook is pinned to commit `2787585`; `zygisk.hpp` comes from topjohnwu's module sample.
