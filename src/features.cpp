#include "features.h"
#include "common.h"
#include "hook.h"
#include "il2cpp.h"
#include <atomic>
#include <cmath>
#include <cstring>

FeatureStatus g_feat;

namespace {

void* g_localPlayerField = nullptr;
int g_offPhysics = -1, g_offCollider = -1, g_offKillTimer = -1, g_offSpeed = -1;
using SetEnabledFn = void (*)(void*, bool, const void*);
SetEnabledFn g_setEnabled = nullptr;
void* g_setEnabledInfo = nullptr;

void (*o_fixedUpdate)(void*, const void*) = nullptr;
float (*o_lightRadius)(void*, void*, const void*) = nullptr;

// One-shot proof that each installed hook is really on the game's call path.
std::atomic<bool> g_seenFixedUpdate{false};
std::atomic<bool> g_seenLocalPlayer{false};
std::atomic<bool> g_seenLightRadius{false};

bool FirstTime(std::atomic<bool>& flag) {
    return !flag.load(std::memory_order_relaxed) && !flag.exchange(true);
}

template <typename T>
T& At(void* obj, int offset) { return *reinterpret_cast<T*>(static_cast<char*>(obj) + offset); }

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

void hkFixedUpdate(void* self, const void* mi) {
    o_fixedUpdate(self, mi);
    if (FirstTime(g_seenFixedUpdate)) Log("hook PlayerControl.FixedUpdate: first call (self %p)", self);
    void* local = nullptr;
    il2::StaticGet(g_localPlayerField, &local);
    if (!self || self != local) return;
    g_feat.localPlayerSeen = true;
    if (FirstTime(g_seenLocalPlayer))
        Log("features: local player matched in FixedUpdate (%p); cfg speed=%d noclip=%d light=%d kill=%d", self,
            g_cfg.speedOn, g_cfg.noclip, g_cfg.lightOn, g_cfg.killOn);

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
}

float hkLightRadius(void* self, void* player, const void* mi) {
    float r = o_lightRadius(self, player, mi);
    if (FirstTime(g_seenLightRadius)) Log("hook ShipStatus.CalculateLightRadius: first call, radius %.3f", r);
    if (g_cfg.lightOn && std::isfinite(r)) r *= g_cfg.lightMul;
    return r;
}

bool OffsetOk(int offset) { return offset >= 0x10 && offset < 0x2000 && (offset % 4) == 0; }

bool ValidTarget(void* method, const char* what) {
    void* p = il2::MethodPointer(method);
    if (!p || !il2::lib.contains(p)) {
        Log("%s: method pointer %p outside libil2cpp", what, p);
        return false;
    }
    return true;
}

}  // namespace

void InitFeatures() {
    Log("il2cpp ready, libil2cpp.so %p-%p", reinterpret_cast<void*>(il2::lib.base),
        reinterpret_cast<void*>(il2::lib.end));

    void* pc = il2::FindClassIn("Assembly-CSharp.dll", "", "PlayerControl");
    void* pp = il2::FindClassIn("Assembly-CSharp.dll", "", "PlayerPhysics");
    void* ship = il2::FindClassIn("Assembly-CSharp.dll", "", "ShipStatus");
    Log("class PlayerControl  %s", pc ? "OK" : "MISSING");
    Log("class PlayerPhysics  %s", pp ? "OK" : "MISSING");
    Log("class ShipStatus     %s", ship ? "OK" : "MISSING");

    g_localPlayerField = il2::Field(pc, "LocalPlayer");
    g_offPhysics = il2::FieldOffset(il2::Field(pc, "MyPhysics"));
    g_offCollider = il2::FieldOffset(il2::Field(pc, "Collider"));
    g_offKillTimer = il2::FieldOffset(il2::Field(pc, "killTimer"));
    g_offSpeed = il2::FieldOffset(il2::Field(pp, "Speed"));
    Log("static PlayerControl.LocalPlayer %s", g_localPlayerField ? "OK" : "MISSING");
    Log("field PlayerControl.MyPhysics    %d", g_offPhysics);
    Log("field PlayerControl.Collider     %d", g_offCollider);
    Log("field PlayerControl.killTimer     %d", g_offKillTimer);
    Log("field PlayerPhysics.Speed        %d", g_offSpeed);

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
    g_feat.speed = hookedFixed && OffsetOk(g_offPhysics) && OffsetOk(g_offSpeed);
    g_feat.kill = hookedFixed && OffsetOk(g_offKillTimer);
    g_feat.noclip = hookedFixed && OffsetOk(g_offCollider) && g_setEnabled;

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

    Log("features: speed=%d noclip=%d light=%d kill=%d", g_feat.speed, g_feat.noclip, g_feat.light,
        g_feat.kill);
}
