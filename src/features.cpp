#include "features.h"
#include "common.h"
#include "elf.h"
#include "hook.h"
#include "il2cpp.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

FeatureStatus g_feat;

namespace {

void* g_localPlayerField = nullptr;
int g_offPhysics = -1, g_offCollider = -1, g_offKillTimer = -1, g_offSpeed = -1;

using SetEnabledFn = void (*)(void*, bool, const void*);
SetEnabledFn g_setEnabled = nullptr;
void* g_setEnabledInfo = nullptr;

void (*o_fixedUpdate)(void*, const void*) = nullptr;
float (*o_lightRadius)(void*, void*, const void*) = nullptr;

template <typename T>
T& At(void* obj, int offset) { return *reinterpret_cast<T*>(static_cast<char*>(obj) + offset); }

// UnityEngine.Color by value: four floats, an HFA, so it travels in s0-s3 on arm64.
struct Color {
    float r, g, b, a;
};
bool SameColor(const Color& x, const Color& y) { return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a; }
constexpr Color kImpostorNameColor{1.f, 0.15f, 0.15f, 1.f};
// More than a lobby can hold; past this the trackers only contain dead entries and are reset.
constexpr size_t kMaxTrackedPlayers = 64;

// ---------------------------------------------------------------- extra il2cpp exports

// Used only by the features below; each one is optional and its absence disables what needs it.
struct ExtraExports {
    void* (*class_from_type)(void*) = nullptr;
    void* (*field_get_type)(void*) = nullptr;
    void* (*method_get_return_type)(void*) = nullptr;
    const char* (*type_get_name)(void*) = nullptr;
} g_x;

template <typename Fn>
void ResolveExtra(Fn* out, const char* name) {
    *out = reinterpret_cast<Fn>(FindExport(il2::lib, name));
    if (!*out) Log("missing optional export %s", name);
}

std::string TypeName(void* type) {
    const char* n = (type && g_x.type_get_name) ? g_x.type_get_name(type) : nullptr;
    return n ? n : "?";
}

std::string FieldTypeName(void* field) {
    return (field && g_x.field_get_type) ? TypeName(g_x.field_get_type(field)) : "?";
}

void* FieldClass(void* field) {
    if (!field || !g_x.field_get_type || !g_x.class_from_type) return nullptr;
    void* t = g_x.field_get_type(field);
    return t ? g_x.class_from_type(t) : nullptr;
}

void* ReturnClass(void* method) {
    if (!method || !g_x.method_get_return_type || !g_x.class_from_type) return nullptr;
    void* t = g_x.method_get_return_type(method);
    return t ? g_x.class_from_type(t) : nullptr;
}

// ---------------------------------------------------------------- validation helpers

bool OffsetOk(int offset, int align = 4) { return offset >= 0x10 && offset < 0x2000 && (offset % align) == 0; }

bool ValidTarget(void* method, const char* what) {
    void* p = il2::MethodPointer(method);
    if (!p || !il2::lib.contains(p)) {
        Log("%s: method pointer %p outside libil2cpp", what, p);
        return false;
    }
    return true;
}

bool Returns(void* method, const char* type, const char* what) {
    if (!method) {
        Log("method %s MISSING", what);
        return false;
    }
    const std::string ret = il2::ReturnTypeName(method);
    if (ret != type) {
        Log("method %s returns %s, expected %s; skipped", what, ret.c_str(), type);
        return false;
    }
    return true;
}

bool Param0(void* method, const char* type, const char* what) {
    if (!method) {
        Log("method %s MISSING", what);
        return false;
    }
    const std::string p = il2::ParamTypeName(method, 0);
    if (p != type) {
        Log("method %s takes %s, expected %s; skipped", what, p.c_str(), type);
        return false;
    }
    return true;
}

bool FieldIs(void* field, int offset, int align, const char* type, const char* what) {
    const std::string t = FieldTypeName(field);
    Log("field %s %d (%s)", what, offset, field ? t.c_str() : "MISSING");
    if (!field) return false;
    if (!OffsetOk(offset, align)) {
        Log("field %s: offset %d implausible; skipped", what, offset);
        return false;
    }
    if (type && t != type) {
        Log("field %s is %s, expected %s; skipped", what, t.c_str(), type);
        return false;
    }
    return true;
}

template <typename Fn>
bool Bind(void* method, const char* what, Fn* fn, const void** info) {
    if (!method || !ValidTarget(method, what)) return false;
    *fn = reinterpret_cast<Fn>(il2::MethodPointer(method));
    *info = method;
    Log("method %s OK", what);
    return true;
}

// ---------------------------------------------------------------- speed / noclip / kill

struct SpeedState {
    bool applied = false;
    float base = 0.f;
    float lastWritten = 0.f;
} g_speed;

bool g_noclipApplied = false;

void ApplySpeed(void* physics) {
    float& speed = At<float>(physics, g_offSpeed);
    if (g_cfg.speedOn) {
        // If the game changed the value since our last write, that is the new base speed.
        if (!g_speed.applied || speed != g_speed.lastWritten) g_speed.base = speed;
        g_speed.lastWritten = g_speed.base * g_cfg.speedMul;
        speed = g_speed.lastWritten;
        g_speed.applied = true;
    } else if (g_speed.applied) {
        speed = g_speed.base;
        g_speed.applied = false;
    }
}

// ---------------------------------------------------------------- zoom (Camera.main.orthographicSize)

struct CameraApi {
    void* (*getMain)(const void*) = nullptr;
    const void* getMainInfo = nullptr;
    float (*getSize)(void*, const void*) = nullptr;
    const void* getSizeInfo = nullptr;
    void (*setSize)(void*, float, const void*) = nullptr;
    const void* setSizeInfo = nullptr;
} g_cam;

struct ZoomState {
    bool applied = false;
    void* camera = nullptr;  // identity only, never dereferenced
    float base = 0.f;
    float lastWritten = 0.f;
} g_zoom;

bool PlausibleOrtho(float v) { return std::isfinite(v) && v > 0.01f && v < 1000.f; }

void ApplyZoom() {
    if (!g_cam.getMain || !g_cam.getSize || !g_cam.setSize) return;
    void* cam = g_cam.getMain(g_cam.getMainInfo);
    if (!cam) return;
    if (cam != g_zoom.camera) {
        // New scene or new camera object: its current size is the base again.
        g_zoom.camera = cam;
        g_zoom.applied = false;
    }
    const float size = g_cam.getSize(cam, g_cam.getSizeInfo);
    if (!PlausibleOrtho(size)) return;
    if (g_cfg.zoomOn) {
        if (!g_zoom.applied || size != g_zoom.lastWritten) g_zoom.base = size;
        const float target = g_zoom.base * g_cfg.zoomMul;
        if (!PlausibleOrtho(target)) return;
        if (size != target) g_cam.setSize(cam, target, g_cam.setSizeInfo);
        g_zoom.lastWritten = target;
        g_zoom.applied = true;
    } else if (g_zoom.applied) {
        g_cam.setSize(cam, g_zoom.base, g_cam.setSizeInfo);
        g_zoom.applied = false;
    }
}

// ---------------------------------------------------------------- per-player: reveal impostors, see ghosts

struct PlayerApi {
    void* (*getData)(void*, const void*) = nullptr;
    const void* getDataInfo = nullptr;
    bool (*isImpostor)(void*, const void*) = nullptr;
    const void* isImpostorInfo = nullptr;
    Color (*getColor)(void*, const void*) = nullptr;
    const void* getColorInfo = nullptr;
    void (*setColor)(void*, Color, const void*) = nullptr;
    const void* setColorInfo = nullptr;
    bool (*getVisible)(void*, const void*) = nullptr;
    const void* getVisibleInfo = nullptr;
    void (*setVisible)(void*, bool, const void*) = nullptr;
    const void* setVisibleInfo = nullptr;
    int offRole = -1, offIsDead = -1, offCosmetics = -1, offNameText = -1;
} g_pl;

// Game thread only (PlayerControl.FixedUpdate). Keys are identities, never dereferenced from here.
std::unordered_map<void*, Color> g_recolored;  // PlayerControl -> name colour before we changed it
std::unordered_set<void*> g_ghostsShown;

void* PlayerInfo(void* player) { return player && g_pl.getData ? g_pl.getData(player, g_pl.getDataInfo) : nullptr; }

bool IsDead(void* info) { return info && At<bool>(info, g_pl.offIsDead); }

void* NameText(void* player) {
    void* cosmetics = At<void*>(player, g_pl.offCosmetics);
    return cosmetics ? At<void*>(cosmetics, g_pl.offNameText) : nullptr;
}

void ApplyReveal(void* player, void* info) {
    if (!g_pl.isImpostor || !g_pl.getColor || !g_pl.setColor) return;
    bool impostor = false;
    if (g_cfg.revealOn && info) {
        void* role = At<void*>(info, g_pl.offRole);
        impostor = role && g_pl.isImpostor(role, g_pl.isImpostorInfo);
    }
    auto it = g_recolored.find(player);
    if (!impostor && it == g_recolored.end()) return;
    void* text = NameText(player);
    if (!text) return;
    const Color now = g_pl.getColor(text, g_pl.getColorInfo);
    if (impostor) {
        if (it == g_recolored.end()) {
            if (g_recolored.size() >= kMaxTrackedPlayers) g_recolored.clear();
            g_recolored.emplace(player, now);
        }
        if (!SameColor(now, kImpostorNameColor)) g_pl.setColor(text, kImpostorNameColor, g_pl.setColorInfo);
    } else {
        if (!SameColor(now, it->second)) g_pl.setColor(text, it->second, g_pl.setColorInfo);
        g_recolored.erase(it);
    }
}

void ApplyGhost(void* player, void* info, bool localDead) {
    if (!g_pl.getVisible || !g_pl.setVisible) return;
    const bool dead = IsDead(info);
    const bool want = g_cfg.ghostsOn && dead && !localDead;
    auto it = g_ghostsShown.find(player);
    if (want) {
        if (!g_pl.getVisible(player, g_pl.getVisibleInfo)) g_pl.setVisible(player, true, g_pl.setVisibleInfo);
        if (it == g_ghostsShown.end()) {
            if (g_ghostsShown.size() >= kMaxTrackedPlayers) g_ghostsShown.clear();
            g_ghostsShown.insert(player);
        }
    } else if (it != g_ghostsShown.end()) {
        g_ghostsShown.erase(it);
        // Re-hide only what the game itself hides: a dead player seen by a living local player.
        if (dead && !localDead) g_pl.setVisible(player, false, g_pl.setVisibleInfo);
    }
}

// ---------------------------------------------------------------- hooks

void hkFixedUpdate(void* self, const void* mi) {
    o_fixedUpdate(self, mi);
    if (!self) return;
    void* local = nullptr;
    il2::StaticGet(g_localPlayerField, &local);
    if (!local) return;

    if (self != local) {
        if (g_feat.reveal || g_feat.ghosts) {
            void* info = PlayerInfo(self);
            if (g_feat.reveal) ApplyReveal(self, info);
            if (g_feat.ghosts) ApplyGhost(self, info, IsDead(PlayerInfo(local)));
        }
        return;
    }

    g_feat.localPlayerSeen = true;
    if (g_feat.speed) {
        if (void* physics = At<void*>(self, g_offPhysics)) ApplySpeed(physics);
    }
    if (g_feat.kill && g_cfg.killOn) At<float>(self, g_offKillTimer) = 0.f;
    if (g_feat.noclip) {
        if (void* col = At<void*>(self, g_offCollider)) {
            if (g_cfg.noclip) {
                g_setEnabled(col, false, g_setEnabledInfo);
                g_noclipApplied = true;
            } else if (g_noclipApplied) {
                g_setEnabled(col, true, g_setEnabledInfo);
                g_noclipApplied = false;
            }
        }
    }
    if (g_feat.zoom) ApplyZoom();
}

float hkLightRadius(void* self, void* player, const void* mi) {
    float r = o_lightRadius(self, player, mi);
    if (g_cfg.lightOn && std::isfinite(r)) r *= g_cfg.lightMul;
    return r;
}

// ---------------------------------------------------------------- init of the new features

bool InitZoom() {
    void* camera = il2::FindClass("UnityEngine", "Camera");
    Log("class UnityEngine.Camera %s", camera ? "OK" : "MISSING");
    if (!camera) return false;
    void* getMain = il2::Method(camera, "get_main", 0);
    void* getSize = il2::Method(camera, "get_orthographicSize", 0);
    void* setSize = il2::Method(camera, "set_orthographicSize", 1);
    return Returns(getMain, "UnityEngine.Camera", "Camera.get_main") &&
           Returns(getSize, "System.Single", "Camera.get_orthographicSize") &&
           Param0(setSize, "System.Single", "Camera.set_orthographicSize") &&
           Bind(getMain, "Camera.get_main", &g_cam.getMain, &g_cam.getMainInfo) &&
           Bind(getSize, "Camera.get_orthographicSize", &g_cam.getSize, &g_cam.getSizeInfo) &&
           Bind(setSize, "Camera.set_orthographicSize", &g_cam.setSize, &g_cam.setSizeInfo);
}

bool InitReveal(void* pc, void* infoClass) {
    void* roleField = il2::Field(infoClass, "Role");
    g_pl.offRole = il2::FieldOffset(roleField);
    if (!FieldIs(roleField, g_pl.offRole, 8, nullptr, "<PlayerInfo>.Role")) return false;
    void* roleClass = FieldClass(roleField);
    void* isImpostor = il2::Method(roleClass, "get_IsImpostor", 0);

    void* cosmeticsField = il2::Field(pc, "cosmetics");
    g_pl.offCosmetics = il2::FieldOffset(cosmeticsField);
    if (!FieldIs(cosmeticsField, g_pl.offCosmetics, 8, nullptr, "PlayerControl.cosmetics")) return false;
    void* nameField = il2::Field(FieldClass(cosmeticsField), "nameText");
    g_pl.offNameText = il2::FieldOffset(nameField);
    if (!FieldIs(nameField, g_pl.offNameText, 8, nullptr, "<cosmetics>.nameText")) return false;

    void* textClass = FieldClass(nameField);
    void* getColor = il2::Method(textClass, "get_color", 0);
    void* setColor = il2::Method(textClass, "set_color", 1);
    if (!getColor || !setColor) {
        void* tmpText = il2::FindClass("TMPro", "TMP_Text");
        Log("nameText class has no own color accessors, trying TMPro.TMP_Text (%s)", tmpText ? "OK" : "MISSING");
        getColor = il2::Method(tmpText, "get_color", 0);
        setColor = il2::Method(tmpText, "set_color", 1);
    }
    return Returns(isImpostor, "System.Boolean", "<Role>.get_IsImpostor") &&
           Returns(getColor, "UnityEngine.Color", "<nameText>.get_color") &&
           Param0(setColor, "UnityEngine.Color", "<nameText>.set_color") &&
           Bind(isImpostor, "<Role>.get_IsImpostor", &g_pl.isImpostor, &g_pl.isImpostorInfo) &&
           Bind(getColor, "<nameText>.get_color", &g_pl.getColor, &g_pl.getColorInfo) &&
           Bind(setColor, "<nameText>.set_color", &g_pl.setColor, &g_pl.setColorInfo);
}

bool InitGhosts(void* pc, void* infoClass) {
    void* deadField = il2::Field(infoClass, "IsDead");
    g_pl.offIsDead = il2::FieldOffset(deadField);
    if (!FieldIs(deadField, g_pl.offIsDead, 1, "System.Boolean", "<PlayerInfo>.IsDead")) return false;
    void* getVisible = il2::Method(pc, "get_Visible", 0);
    void* setVisible = il2::Method(pc, "set_Visible", 1);
    return Returns(getVisible, "System.Boolean", "PlayerControl.get_Visible") &&
           Param0(setVisible, "System.Boolean", "PlayerControl.set_Visible") &&
           Bind(getVisible, "PlayerControl.get_Visible", &g_pl.getVisible, &g_pl.getVisibleInfo) &&
           Bind(setVisible, "PlayerControl.set_Visible", &g_pl.setVisible, &g_pl.setVisibleInfo);
}

void InitPlayerExtras(void* pc, bool hookedFixed, bool* revealOk, bool* ghostsOk) {
    *revealOk = *ghostsOk = false;
    if (!pc) return;
    void* getData = il2::Method(pc, "get_Data", 0);
    Log("method PlayerControl.get_Data(0) %s, returns %s", getData ? "OK" : "MISSING",
        getData ? il2::ReturnTypeName(getData).c_str() : "-");
    void* infoClass = ReturnClass(getData);
    if (!infoClass || !Bind(getData, "PlayerControl.get_Data", &g_pl.getData, &g_pl.getDataInfo)) {
        Log("player info class unresolved; reveal and ghosts skipped");
        return;
    }
    *revealOk = hookedFixed && InitReveal(pc, infoClass);
    *ghostsOk = hookedFixed && InitGhosts(pc, infoClass);
}

}  // namespace

void InitFeatures() {
    Log("il2cpp ready, libil2cpp.so %p-%p", reinterpret_cast<void*>(il2::lib.base),
        reinterpret_cast<void*>(il2::lib.end));
    ResolveExtra(&g_x.class_from_type, "il2cpp_class_from_type");
    ResolveExtra(&g_x.field_get_type, "il2cpp_field_get_type");
    ResolveExtra(&g_x.method_get_return_type, "il2cpp_method_get_return_type");
    ResolveExtra(&g_x.type_get_name, "il2cpp_type_get_name");

    void* pc = il2::FindClassIn("Assembly-CSharp.dll", "", "PlayerControl");
    void* pp = il2::FindClassIn("Assembly-CSharp.dll", "", "PlayerPhysics");
    void* ship = il2::FindClassIn("Assembly-CSharp.dll", "", "ShipStatus");
    Log("class PlayerControl %s", pc ? "OK" : "MISSING");
    Log("class PlayerPhysics %s", pp ? "OK" : "MISSING");
    Log("class ShipStatus %s", ship ? "OK" : "MISSING");

    g_localPlayerField = il2::Field(pc, "LocalPlayer");
    g_offPhysics = il2::FieldOffset(il2::Field(pc, "MyPhysics"));
    g_offCollider = il2::FieldOffset(il2::Field(pc, "Collider"));
    g_offKillTimer = il2::FieldOffset(il2::Field(pc, "killTimer"));
    g_offSpeed = il2::FieldOffset(il2::Field(pp, "Speed"));
    Log("static PlayerControl.LocalPlayer %s", g_localPlayerField ? "OK" : "MISSING");
    Log("field PlayerControl.MyPhysics %d", g_offPhysics);
    Log("field PlayerControl.Collider %d", g_offCollider);
    Log("field PlayerControl.killTimer %d", g_offKillTimer);
    Log("field PlayerPhysics.Speed %d", g_offSpeed);

    void* behaviour = il2::FindClass("UnityEngine", "Behaviour");
    g_setEnabledInfo = il2::Method(behaviour, "set_enabled", 1);
    g_setEnabled = reinterpret_cast<SetEnabledFn>(il2::MethodPointer(g_setEnabledInfo));
    Log("UnityEngine.Behaviour.set_enabled %s", g_setEnabled ? "OK" : "MISSING");

    void* fixedUpdate = il2::Method(pc, "FixedUpdate", 0);
    Log("method PlayerControl.FixedUpdate(0) %s", fixedUpdate ? "OK" : "MISSING");
    bool hookedFixed = false;
    if (g_localPlayerField && !il2::FieldIsStatic(g_localPlayerField))
        Log("PlayerControl.LocalPlayer is not a static field on this build, FixedUpdate hook skipped");
    if (g_localPlayerField && il2::FieldIsStatic(g_localPlayerField) &&
        ValidTarget(fixedUpdate, "PlayerControl.FixedUpdate")) {
        hookedFixed = InstallHook("PlayerControl.FixedUpdate", il2::MethodPointer(fixedUpdate),
                                  hkFixedUpdate, &o_fixedUpdate);
    }

    // Everything the hook reads is resolved before its flag is published.
    const bool zoomOk = hookedFixed && InitZoom();
    bool revealOk = false, ghostsOk = false;
    InitPlayerExtras(pc, hookedFixed, &revealOk, &ghostsOk);

    g_feat.speed = hookedFixed && OffsetOk(g_offPhysics) && OffsetOk(g_offSpeed);
    g_feat.kill = hookedFixed && OffsetOk(g_offKillTimer);
    g_feat.noclip = hookedFixed && OffsetOk(g_offCollider) && g_setEnabled;
    g_feat.zoom = zoomOk;
    g_feat.reveal = revealOk;
    g_feat.ghosts = ghostsOk;

    void* light = il2::Method(ship, "CalculateLightRadius", 1);
    Log("method ShipStatus.CalculateLightRadius(1) %s", light ? "OK" : "MISSING");
    if (light) {
        std::string ret = il2::ReturnTypeName(light);
        bool refParam = il2::ParamIsReference(light, 0);
        Log("  returns %s, param0 %s%s", ret.c_str(), il2::ParamTypeName(light, 0).c_str(),
            refParam ? "" : " (not a reference type)");
        if (ret == "System.Single" && refParam && ValidTarget(light, "ShipStatus.CalculateLightRadius")) {
            g_feat.light = InstallHook("ShipStatus.CalculateLightRadius", il2::MethodPointer(light),
                                       hkLightRadius, &o_lightRadius);
        } else {
            Log("  signature differs from the expected float(ref); light hook skipped");
        }
    }

    Log("features: speed=%d noclip=%d light=%d kill=%d zoom=%d reveal=%d ghosts=%d", g_feat.speed,
        g_feat.noclip, g_feat.light, g_feat.kill, g_feat.zoom, g_feat.reveal, g_feat.ghosts);
}
