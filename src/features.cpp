#include "features.h"
#include "common.h"
#include "elf.h"
#include "hook.h"
#include "il2cpp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

FeatureStatus g_feat;

namespace {

void* g_localPlayerField = nullptr;
int g_offPhysics = -1, g_offCollider = -1, g_offKillTimer = -1;
int g_offSpeed = -1, g_offGhostSpeed = -1, g_offOnLadder = -1;
std::atomic<bool> g_ready{false};

using SetEnabledFn = void (*)(void*, bool, const void*);

template <typename Fn>
struct Managed {
    Fn fn = nullptr;
    const void* info = nullptr;
    explicit operator bool() const { return fn != nullptr; }
    template <typename... Args>
    auto operator()(Args... args) const -> decltype(fn(args...)) { return fn(args...); }
};

Managed<SetEnabledFn> g_setEnabled;
void (*o_fixedUpdate)(void*, const void*) = nullptr;
float (*o_lightRadius)(void*, void*, const void*) = nullptr;
void (*o_followerUpdate)(void*, const void*) = nullptr;

template <typename T>
T& At(void* obj, int offset) { return *reinterpret_cast<T*>(static_cast<char*>(obj) + offset); }

struct Vector2 { float x = 0.f, y = 0.f; };
struct Color { float r, g, b, a; };
bool SameColor(const Color& x, const Color& y) {
    return x.r == y.r && x.g == y.g && x.b == y.b && x.a == y.a;
}
constexpr Color kImpostorNameColor{1.f, 0.15f, 0.15f, 1.f};
constexpr size_t kMaxTrackedPlayers = 64;
constexpr int kMaxPlayers = 64;
constexpr int kMaxTasks = 128;

// ---------------------------------------------------------------- extra il2cpp exports

struct ExtraExports {
    void* (*class_from_type)(void*) = nullptr;
    void* (*class_get_parent)(void*) = nullptr;
    void* (*field_get_type)(void*) = nullptr;
    void* (*method_get_return_type)(void*) = nullptr;
    const char* (*type_get_name)(void*) = nullptr;
    const uint16_t* (*string_chars)(void*) = nullptr;
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

// DestroyableSingleton<T>.Instance lives on a generic base rather than on the derived class.
void* FindFieldUp(void* klass, const char* name) {
    for (void* k = klass; k;) {
        if (void* field = il2::Field(k, name)) return field;
        k = g_x.class_get_parent ? g_x.class_get_parent(k) : nullptr;
    }
    return nullptr;
}

void* ReadStatic(void* field) {
    void* value = nullptr;
    return field && il2::StaticGet(field, &value) ? value : nullptr;
}

// ---------------------------------------------------------------- validation helpers

bool OffsetOk(int offset, int align = 4) { return offset >= 0x10 && offset < 0x2000 && offset % align == 0; }

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
bool Bind(void* method, const char* what, Managed<Fn>* out) {
    if (!method || !ValidTarget(method, what)) return false;
    out->fn = reinterpret_cast<Fn>(il2::MethodPointer(method));
    out->info = method;
    Log("method %s OK", what);
    return true;
}

using GetObjectFn = void* (*)(void*, const void*);
using GetBoolFn = bool (*)(void*, const void*);
using GetFloatFn = float (*)(void*, const void*);
using SetBoolFn = void (*)(void*, bool, const void*);
using SetFloatFn = void (*)(void*, float, const void*);
using GetListItemFn = void* (*)(void*, int, const void*);

// ---------------------------------------------------------------- generic list/string adapters

struct ListApi {
    Managed<int (*)(void*, const void*)> count;
    Managed<GetListItemFn> item;
    explicit operator bool() const { return count && item; }
    int Count(void* list) const { return list && count ? std::max(0, count.fn(list, count.info)) : 0; }
    void* Item(void* list, int index) const { return list && item ? item.fn(list, index, item.info) : nullptr; }
};

bool InitList(ListApi& out, void* listField, const char* expectedItem, const char* what) {
    if (!listField) return false;
    void* listClass = FieldClass(listField);
    void* count = il2::Method(listClass, "get_Count", 0);
    void* item = il2::Method(listClass, "get_Item", 1);
    return Returns(count, "System.Int32", what) && Param0(item, "System.Int32", what) &&
           Returns(item, expectedItem, what) && Bind(count, what, &out.count) && Bind(item, what, &out.item);
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

std::string ManagedString(void* str) {
    if (!str || !g_x.string_chars) return {};
    const uint16_t* chars = g_x.string_chars(str);
    if (!chars) return {};
    std::string out;
    for (size_t i = 0; i < 96 && chars[i]; ++i) {
        const uint16_t c = chars[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == '|') out.push_back(' ');
        else if (c < 0xd800 || c > 0xdfff) AppendUtf8(out, c);
        else out.push_back('?');
        if (out.size() > 180) break;
    }
    return out;
}

// ---------------------------------------------------------------- speed / noclip / local invisibility

struct ValueState { bool applied = false; float base = 0.f; };
struct SpeedState { void* physics = nullptr; ValueState walk, ghost; } g_speed;

void ApplyValue(float& slot, float multiplier, bool on, ValueState& state) {
    if (on) {
        // Never turn a zero introduced by a meeting/cutscene into the permanent restore value.
        if (!state.applied && std::isfinite(slot) && slot > 0.01f) state.base = slot;
        if (state.base > 0.01f) slot = state.base * multiplier;
        state.applied = true;
    } else if (state.applied) {
        if (state.base > 0.01f) slot = state.base;
        state.applied = false;
    }
}

void ApplySpeed(void* physics) {
    if (!physics) return;
    if (g_speed.physics != physics) {
        g_speed.physics = physics;
        g_speed.walk = {};
        g_speed.ghost = {};
    }
    ApplyValue(At<float>(physics, g_offSpeed), g_cfg.speedMul, g_cfg.speedOn, g_speed.walk);
    if (OffsetOk(g_offGhostSpeed))
        ApplyValue(At<float>(physics, g_offGhostSpeed), g_cfg.speedMul, g_cfg.speedOn, g_speed.ghost);
}

struct ColliderState { void* collider = nullptr; bool applied = false; } g_collider;

void ApplyNoclip(void* local) {
    if (!g_feat.noclip || !local) return;
    void* collider = At<void*>(local, g_offCollider);
    if (!collider) return;
    const bool onLadder = OffsetOk(g_offOnLadder, 1) && At<bool>(local, g_offOnLadder);
    const bool want = g_cfg.noclip && !onLadder;
    if (want && (!g_collider.applied || collider != g_collider.collider)) {
        g_setEnabled(collider, false, g_setEnabled.info);
        g_collider.collider = collider;
        g_collider.applied = true;
    } else if (!want && g_collider.applied && collider == g_collider.collider) {
        g_setEnabled(collider, true, g_setEnabled.info);
        g_collider.applied = false;
    }
}

// ---------------------------------------------------------------- per-player ESP

struct PlayerApi {
    Managed<GetObjectFn> getData;
    Managed<GetBoolFn> isImpostor;
    Managed<Color (*)(void*, const void*)> getColor;
    Managed<void (*)(void*, Color, const void*)> setColor;
    Managed<GetBoolFn> getVisible;
    Managed<SetBoolFn> setVisible;
    int offRole = -1, offIsDead = -1, offCosmetics = -1, offNameText = -1;
} g_pl;

void* PlayerInfo(void* player) { return player && g_pl.getData ? g_pl.getData(player, g_pl.getData.info) : nullptr; }
bool IsDead(void* info) { return info && OffsetOk(g_pl.offIsDead, 1) && At<bool>(info, g_pl.offIsDead); }

void* NameText(void* player) {
    if (!player || !OffsetOk(g_pl.offCosmetics, 8) || !OffsetOk(g_pl.offNameText, 8)) return nullptr;
    void* cosmetics = At<void*>(player, g_pl.offCosmetics);
    return cosmetics ? At<void*>(cosmetics, g_pl.offNameText) : nullptr;
}

std::unordered_map<void*, Color> g_recolored;
std::unordered_set<void*> g_ghostsShown;

void RestoreRevealIfNeeded(void* player, void* info) {
    auto it = g_recolored.find(player);
    if (it == g_recolored.end()) return;
    if (!g_pl.setColor || !g_pl.getColor) { g_recolored.erase(it); return; }
    void* text = NameText(player);
    if (text && !SameColor(g_pl.getColor(text, g_pl.getColor.info), it->second))
        g_pl.setColor(text, it->second, g_pl.setColor.info);
    g_recolored.erase(it);
    (void) info;
}

void ApplyReveal(void* player, void* info) {
    if (!g_pl.isImpostor || !g_pl.getColor || !g_pl.setColor) return;
    bool impostor = false;
    if (g_cfg.revealOn && info) {
        void* role = At<void*>(info, g_pl.offRole);
        impostor = role && g_pl.isImpostor(role, g_pl.isImpostor.info);
    }
    if (!impostor) { RestoreRevealIfNeeded(player, info); return; }
    auto it = g_recolored.find(player);
    void* text = NameText(player);
    if (!text) return;
    const Color now = g_pl.getColor(text, g_pl.getColor.info);
    if (it == g_recolored.end()) {
        if (g_recolored.size() >= kMaxTrackedPlayers) g_recolored.clear();
        it = g_recolored.emplace(player, now).first;
    }
    if (!SameColor(now, kImpostorNameColor)) g_pl.setColor(text, kImpostorNameColor, g_pl.setColor.info);
}

void ApplyGhost(void* player, void* info, bool localDead) {
    if (!g_pl.getVisible || !g_pl.setVisible) return;
    const bool dead = IsDead(info);
    const bool want = g_cfg.ghostsOn && dead && !localDead;
    auto it = g_ghostsShown.find(player);
    if (want) {
        if (!g_pl.getVisible(player, g_pl.getVisible.info)) g_pl.setVisible(player, true, g_pl.setVisible.info);
        if (it == g_ghostsShown.end()) {
            if (g_ghostsShown.size() >= kMaxTrackedPlayers) g_ghostsShown.clear();
            g_ghostsShown.insert(player);
        }
    } else if (it != g_ghostsShown.end()) {
        g_ghostsShown.erase(it);
        if (dead && !localDead) g_pl.setVisible(player, false, g_pl.setVisible.info);
    }
}

struct LocalVisibility {
    void* player = nullptr;
    bool applied = false, previous = true;
    int ticks = 0;
} g_localVisibility;

void ApplyLocalInvisibility(void* local) {
    if (!g_feat.invisible) return;
    if (g_cfg.invisibleOn) {
        if (!g_localVisibility.applied || local != g_localVisibility.player) {
            g_localVisibility.player = local;
            g_localVisibility.previous = g_pl.getVisible(local, g_pl.getVisible.info);
            g_localVisibility.applied = true;
            g_localVisibility.ticks = 0;
        }
        if (g_localVisibility.previous && g_localVisibility.ticks-- <= 0) {
            if (g_pl.getVisible(local, g_pl.getVisible.info))
                g_pl.setVisible(local, false, g_pl.setVisible.info);
            g_localVisibility.ticks = 10;
        }
    } else if (g_localVisibility.applied) {
        if (local == g_localVisibility.player)
            g_pl.setVisible(local, g_localVisibility.previous, g_pl.setVisible.info);
        g_localVisibility = {};
    }
}

// ---------------------------------------------------------------- world camera and wall shadows

struct WorldApi {
    void* hudInstance = nullptr;
    void* meetingInstance = nullptr;
    void* minigameInstance = nullptr;
    int offFollower = -1, offCamera = -1, offShadow = -1;
    Managed<GetObjectFn> getGameObject;
    Managed<GetBoolFn> getActive;
    Managed<SetBoolFn> setActive;
    Managed<GetFloatFn> getSize;
    Managed<SetFloatFn> setSize;
    Managed<void (*)(void*, void*, const void*)> setTarget;
} g_world;

void* CurrentHud() { return ReadStatic(g_world.hudInstance); }
void* CurrentFollower() {
    void* hud = CurrentHud();
    return hud && OffsetOk(g_world.offFollower, 8) ? At<void*>(hud, g_world.offFollower) : nullptr;
}

bool OverlayActive() {
    return ReadStatic(g_world.meetingInstance) || ReadStatic(g_world.minigameInstance);
}

struct ZoomState { void* camera = nullptr; bool applied = false; float base = 3.f; } g_zoom;

void ApplyWorldZoom(void* follower) {
    if (!g_feat.zoom || !follower || !OffsetOk(g_world.offCamera, 8)) return;
    void* camera = At<void*>(follower, g_world.offCamera);
    if (!camera) return;
    const float size = g_world.getSize(camera, g_world.getSize.info);
    if (!std::isfinite(size) || size < 0.1f || size > 100.f) return;
    if (camera != g_zoom.camera) {
        g_zoom.camera = camera;
        g_zoom.base = size;
        g_zoom.applied = false;
    }
    if (!g_cfg.zoomOn || OverlayActive()) {
        if (g_zoom.applied) {
            g_world.setSize(camera, g_zoom.base, g_world.setSize.info);
            g_zoom.applied = false;
        }
        return;
    }
    if (!g_zoom.applied) g_zoom.base = size;
    const float base = g_zoom.base;
    const float target = base * g_cfg.zoomMul;
    if (std::isfinite(target) && target >= 0.1f && target <= 100.f) {
        // UICamera, ShadowCamera and their Canvas layout are deliberately never touched.
        if (size != target) g_world.setSize(camera, target, g_world.setSize.info);
        g_zoom.applied = true;
    }
}

struct ShadowState {
    void* hud = nullptr;
    void* gameObject = nullptr;
    bool hidden = false;
    bool previousActive = true;
} g_shadow;

void ApplyWallShadow(void* hud) {
    if (!g_feat.wallVision || !hud || !OffsetOk(g_world.offShadow, 8)) return;
    if (hud != g_shadow.hud) {
        // A new HUD can reuse Unity object addresses. Never restore or hide through a previous scene.
        g_shadow = {};
        g_shadow.hud = hud;
    }
    void* renderer = At<void*>(hud, g_world.offShadow);
    void* go = renderer ? g_world.getGameObject(renderer, g_world.getGameObject.info) : nullptr;
    const bool wantHidden = g_cfg.noclip || g_cfg.wallVisionOn;
    if (go && wantHidden) {
        if (!g_shadow.hidden || go != g_shadow.gameObject) {
            g_shadow.previousActive = g_world.getActive(go, g_world.getActive.info);
            if (g_shadow.previousActive) g_world.setActive(go, false, g_world.setActive.info);
            g_shadow.gameObject = go;
            g_shadow.hidden = true;
        }
    } else if (g_shadow.hidden) {
        if (!go || go == g_shadow.gameObject)
            if (go) g_world.setActive(go, g_shadow.previousActive, g_world.setActive.info);
        g_shadow.gameObject = nullptr;
        g_shadow.hidden = false;
    }
}

// ---------------------------------------------------- player registry and queued actions

struct PlayerRegistry {
    void* allPlayersField = nullptr;
    void* clientInstance = nullptr;
    ListApi allPlayers, tasks;
    int offId = -1, offNetTransform = -1, offName = -1, offDisconnected = -1;
    int offMoveable = -1, offTasks = -1, offNetworkMode = -1, offAmHost = -1;
    Managed<Vector2 (*)(void*, const void*)> getTruePosition;
    Managed<void (*)(void*, Vector2, const void*)> rpcSnapTo;
    Managed<void (*)(void*, void*, const void*)> setTarget;
    Managed<void (*)(void*, void*, const void*)> cmdReport;
    Managed<void (*)(void*, int, const void*)> rpcCompleteTask;
    Managed<int (*)(void*, const void*)> getTaskId;
} g_players;

struct RuntimePlayer {
    void* object = nullptr;
    void* info = nullptr;
    int id = -1;
    std::string name;
    bool dead = false;
    bool disconnected = false;
};

std::vector<RuntimePlayer> EnumeratePlayers(void* local) {
    std::vector<RuntimePlayer> out;
    void* list = ReadStatic(g_players.allPlayersField);
    const int count = std::min(kMaxPlayers, g_players.allPlayers.Count(list));
    if (!list || !g_pl.getData) return out;
    for (int i = 0; i < count; ++i) {
        void* object = g_players.allPlayers.Item(list, i);
        void* info = object ? g_pl.getData(object, g_pl.getData.info) : nullptr;
        if (!object || !info) continue;
        RuntimePlayer p;
        p.object = object;
        p.info = info;
        p.id = object != local && OffsetOk(g_players.offId, 1) ? At<uint8_t>(object, g_players.offId) : 255;
        p.name = OffsetOk(g_players.offName, 8) ? ManagedString(At<void*>(info, g_players.offName)) : "Player";
        if (p.name.empty()) p.name = "Player " + std::to_string(p.id);
        p.dead = IsDead(info);
        p.disconnected = OffsetOk(g_players.offDisconnected, 1) && At<bool>(info, g_players.offDisconnected);
        if (!p.disconnected) out.push_back(p);
    }
    return out;
}

std::mutex g_snapshotMu;
std::vector<PlayerEntry> g_snapshot;
std::atomic<bool> g_refreshPlayers{true};

std::mutex g_actionMu;
struct ActionCommand { uint32_t sequence = 0; int action = 0, target = -1; } g_command;
std::atomic<bool> g_actionPending{false};
std::string g_actionMessage = "Open in a match and tap the target to cycle players";
uint32_t g_actionsCompleted = 0;

std::atomic<bool> g_spectating{false};
void* g_spectatedLocal = nullptr;
bool g_spectatePreviousMoveable = true;

std::string ActionName(const RuntimePlayer& p) {
    return "#" + std::to_string(p.id) + " " + p.name + (p.dead ? " (dead)" : "");
}

bool HostOrFreeplay() {
    void* client = ReadStatic(g_players.clientInstance);
    if (!client) return false;
    const bool host = OffsetOk(g_players.offAmHost, 1) && At<bool>(client, g_players.offAmHost);
    // NetworkModes: OnlineGame = 0, LocalGame = 1, FreePlay = 2.
    const bool freeplay = OffsetOk(g_players.offNetworkMode) && At<int32_t>(client, g_players.offNetworkMode) == 2;
    return host || freeplay;
}

void CompleteTasks(void* local) {
    if (!OffsetOk(g_players.offTasks, 8) || !g_players.rpcCompleteTask || !g_players.getTaskId) return;
    void* list = At<void*>(local, g_players.offTasks);
    const int count = std::min(kMaxTasks, g_players.tasks.Count(list));
    for (int i = 0; i < count; ++i) {
        void* task = g_players.tasks.Item(list, i);
        if (task) g_players.rpcCompleteTask(local, g_players.getTaskId(task, g_players.getTaskId.info),
                                             g_players.rpcCompleteTask.info);
    }
}

std::string ExecuteAction(void* local, int action, int targetId) {
    if (!local) return "No local player; enter a match or Freeplay";
    if (action == ACTION_COMPLETE_TASKS) {
        if (!g_feat.completeTasks) return "Task completion is unavailable on this build";
        CompleteTasks(local);
        return "Complete-task RPC sent for every owned task";
    }
    if (action == ACTION_RETURN_CAMERA) {
        if (!g_feat.spectate) return "Camera controls are unavailable on this build";
        if (void* follower = CurrentFollower())
            g_players.setTarget(follower, local, g_players.setTarget.info);
        if (g_spectatedLocal == local && OffsetOk(g_players.offMoveable, 1))
            At<bool>(local, g_players.offMoveable) = g_spectatePreviousMoveable;
        g_spectatedLocal = local;
        g_spectating.store(false);
        return "Camera returned to local player and movement restored";
    }

    auto all = EnumeratePlayers(local);
    auto it = std::find_if(all.begin(), all.end(), [targetId](const RuntimePlayer& p) { return p.id == targetId; });
    if (it == all.end()) return "Target left the match; refresh the player list";
    RuntimePlayer& target = *it;
    if (target.object == local) return "Select another player, not the local player";

    switch (action) {
        case ACTION_TELEPORT_TO: {
            if (!g_feat.teleport) return "Teleport is unavailable on this build";
            void* net = At<void*>(local, g_players.offNetTransform);
            if (!net) return "Local networked transform is not ready; try again after spawning";
            const Vector2 pos = g_players.getTruePosition(target.object, g_players.getTruePosition.info);
            g_players.rpcSnapTo(net, pos, g_players.rpcSnapTo.info);
            return "Teleported to " + ActionName(target) + "; online host may correct the position";
        }
        case ACTION_BRING: {
            if (!g_feat.bring) return "Bring-player is unavailable on this build";
            if (!HostOrFreeplay()) return "Bring-player is available only to the host or in Freeplay";
            void* net = At<void*>(target.object, g_players.offNetTransform);
            if (!net) return "Target networked transform is not ready";
            const Vector2 pos = g_players.getTruePosition(local, g_players.getTruePosition.info);
            g_players.rpcSnapTo(net, pos, g_players.rpcSnapTo.info);
            return "Bring-player RPC sent for " + ActionName(target);
        }
        case ACTION_SPECTATE: {
            if (!g_feat.spectate) return "Spectate is unavailable on this build";
            void* follower = CurrentFollower();
            if (!follower) return "Player camera is not active; try again in gameplay";
            if (OffsetOk(g_players.offMoveable, 1)) {
                g_spectatePreviousMoveable = At<bool>(local, g_players.offMoveable);
                At<bool>(local, g_players.offMoveable) = false;
            }
            g_players.setTarget(follower, target.object, g_players.setTarget.info);
            g_spectatedLocal = local;
            g_spectating.store(true);
            return "Spectating " + ActionName(target) + "; use Return camera to restore movement";
        }
        case ACTION_REPORT_BODY: {
            if (!g_feat.report) return "Body report is unavailable on this build";
            if (!target.dead) return "Select a dead player to report a body";
            g_players.cmdReport(local, target.info, g_players.cmdReport.info);
            return "Body report sent for " + ActionName(target);
        }
        default: return "Unknown player action";
    }
}

void ProcessAction(void* local) {
    ActionCommand cmd;
    {
        std::lock_guard<std::mutex> lk(g_actionMu);
        if (!g_actionPending.load()) return;
        cmd = g_command;
    }
    const std::string result = ExecuteAction(local, cmd.action, cmd.target);
    {
        std::lock_guard<std::mutex> lk(g_actionMu);
        g_actionMessage = result;
        g_actionsCompleted = cmd.sequence;
        g_actionPending.store(false);
    }
    Log("player action %d target %d: %s", cmd.action, cmd.target, result.c_str());
}

void ProcessSceneSpectate(void* local) {
    if (!g_spectating.load() || g_spectatedLocal == local || !g_feat.spectate) return;
    if (void* follower = CurrentFollower())
        g_players.setTarget(follower, local, g_players.setTarget.info);
    g_spectatedLocal = local;
    g_spectating.store(false);
    // Do not force moveable=true on a newly spawned player: the scene may intentionally freeze it.
}

// ---------------------------------------------------------------- hooks

void hkFixedUpdate(void* self, const void* mi) {
    o_fixedUpdate(self, mi);
    if (!self || !g_ready.load(std::memory_order_acquire)) return;
    void* local = ReadStatic(g_localPlayerField);
    if (!local) return;

    if (self != local) {
        // Do not traverse every player's role/cosmetics at 50 Hz when all ESP toggles are off.
        const bool needsInfo = (g_cfg.revealOn || g_cfg.ghostsOn || !g_recolored.empty() || !g_ghostsShown.empty());
        if (!needsInfo) return;
        void* info = PlayerInfo(self);
        if (g_feat.reveal) ApplyReveal(self, info);
        if (g_feat.ghosts) ApplyGhost(self, info, IsDead(PlayerInfo(local)));
        return;
    }

    g_feat.localPlayerSeen = true;
    if (g_feat.speed) ApplySpeed(At<void*>(self, g_offPhysics));
    if (g_feat.kill && g_cfg.killOn) At<float>(self, g_offKillTimer) = 0.f;
    ApplyNoclip(self);
    ApplyLocalInvisibility(self);
    ProcessSceneSpectate(self);
    ProcessAction(self);
    if (g_refreshPlayers.exchange(false) && g_feat.players) {
        std::vector<PlayerEntry> entries;
        for (const RuntimePlayer& p : EnumeratePlayers(self)) {
            PlayerEntry e;
            e.id = p.object == self ? 255 : p.id;
            e.name = p.name;
            e.dead = p.dead;
            e.disconnected = p.disconnected;
            e.local = p.object == self;
            entries.push_back(std::move(e));
        }
        {
            std::lock_guard<std::mutex> lk(g_snapshotMu);
            g_snapshot.swap(entries);
        }
    }
    if (void* hud = CurrentHud()) ApplyWallShadow(hud);
}

void hkFollowerUpdate(void* self, const void* mi) {
    o_followerUpdate(self, mi);
    if (g_ready.load(std::memory_order_acquire)) ApplyWorldZoom(self);
}

float hkLightRadius(void* self, void* player, const void* mi) {
    float radius = o_lightRadius(self, player, mi);
    if (g_cfg.lightOn && std::isfinite(radius)) radius *= g_cfg.lightMul;
    return radius;
}

// ---------------------------------------------------------------- initializers

bool InitWorld() {
    void* camera = il2::FindClass("UnityEngine", "Camera");
    void* component = il2::FindClass("UnityEngine", "Component");
    void* gameObject = il2::FindClass("UnityEngine", "GameObject");
    void* hud = il2::FindClassIn("Assembly-CSharp.dll", "", "HudManager");
    g_world.hudInstance = FindFieldUp(hud, "Instance");
    Log("class HudManager %s, inherited Instance %s", hud ? "OK" : "MISSING", g_world.hudInstance ? "OK" : "MISSING");

    void* followerField = il2::Field(hud, "PlayerCam");
    g_world.offFollower = il2::FieldOffset(followerField);
    void* follower = FieldClass(followerField);
    void* camField = il2::Field(follower, "cam");
    g_world.offCamera = il2::FieldOffset(camField);
    const bool followerOk = FieldIs(followerField, g_world.offFollower, 8, nullptr, "HudManager.PlayerCam") &&
                            FieldIs(camField, g_world.offCamera, 8, "UnityEngine.Camera", "FollowerCamera.cam");
    void* getSize = il2::Method(camera, "get_orthographicSize", 0);
    void* setSize = il2::Method(camera, "set_orthographicSize", 1);
    const bool cameraOk = Returns(getSize, "System.Single", "Camera.get_orthographicSize") &&
                          Param0(setSize, "System.Single", "Camera.set_orthographicSize") &&
                          Bind(getSize, "Camera.get_orthographicSize", &g_world.getSize) &&
                          Bind(setSize, "Camera.set_orthographicSize", &g_world.setSize);
    if (cameraOk && g_world.hudInstance) {
        void* setTarget = il2::Method(follower, "SetTarget", 1);
        if (Param0(setTarget, "UnityEngine.MonoBehaviour", "FollowerCamera.SetTarget") &&
            Returns(setTarget, "System.Void", "FollowerCamera.SetTarget"))
            Bind(setTarget, "FollowerCamera.SetTarget", &g_world.setTarget);
    }
    void* followerUpdate = il2::Method(follower, "Update", 0);
    if (followerOk && cameraOk && g_world.hudInstance && ValidTarget(followerUpdate, "FollowerCamera.Update")) {
        g_feat.zoom = InstallHook("FollowerCamera.Update", il2::MethodPointer(followerUpdate),
                                  hkFollowerUpdate, &o_followerUpdate);
    }

    void* getGo = il2::Method(component, "get_gameObject", 0);
    void* getActive = il2::Method(gameObject, "get_activeSelf", 0);
    void* setActive = il2::Method(gameObject, "SetActive", 1);
    const bool goOk = Returns(getGo, "UnityEngine.GameObject", "Component.get_gameObject") &&
                      Returns(getActive, "System.Boolean", "GameObject.get_activeSelf") &&
                      Param0(setActive, "System.Boolean", "GameObject.SetActive") &&
                      Bind(getGo, "Component.get_gameObject", &g_world.getGameObject) &&
                      Bind(getActive, "GameObject.get_activeSelf", &g_world.getActive) &&
                      Bind(setActive, "GameObject.SetActive", &g_world.setActive);
    void* shadowField = il2::Field(hud, "ShadowQuad");
    g_world.offShadow = il2::FieldOffset(shadowField);
    g_feat.wallVision = goOk && g_world.hudInstance &&
                        FieldIs(shadowField, g_world.offShadow, 8, nullptr, "HudManager.ShadowQuad");
    g_world.meetingInstance = FindFieldUp(il2::FindClassIn("Assembly-CSharp.dll", "", "MeetingHud"), "Instance");
    g_world.minigameInstance = FindFieldUp(il2::FindClassIn("Assembly-CSharp.dll", "", "Minigame"), "Instance");
    return g_feat.zoom || g_feat.wallVision || g_world.setTarget;
}

bool InitReveal(void* pc, void* infoClass) {
    void* roleField = il2::Field(infoClass, "Role");
    g_pl.offRole = il2::FieldOffset(roleField);
    if (!FieldIs(roleField, g_pl.offRole, 8, nullptr, "<PlayerInfo>.Role")) return false;
    void* isImpostor = il2::Method(FieldClass(roleField), "get_IsImpostor", 0);
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
        getColor = il2::Method(tmpText, "get_color", 0);
        setColor = il2::Method(tmpText, "set_color", 1);
    }
    return Returns(isImpostor, "System.Boolean", "<Role>.get_IsImpostor") &&
           Returns(getColor, "UnityEngine.Color", "<nameText>.get_color") &&
           Param0(setColor, "UnityEngine.Color", "<nameText>.set_color") &&
           Bind(isImpostor, "<Role>.get_IsImpostor", &g_pl.isImpostor) &&
           Bind(getColor, "<nameText>.get_color", &g_pl.getColor) &&
           Bind(setColor, "<nameText>.set_color", &g_pl.setColor);
}

bool InitGhosts(void* pc, void* infoClass) {
    void* deadField = il2::Field(infoClass, "IsDead");
    g_pl.offIsDead = il2::FieldOffset(deadField);
    if (!FieldIs(deadField, g_pl.offIsDead, 1, "System.Boolean", "<PlayerInfo>.IsDead")) return false;
    void* getVisible = il2::Method(pc, "get_Visible", 0);
    void* setVisible = il2::Method(pc, "set_Visible", 1);
    return Returns(getVisible, "System.Boolean", "PlayerControl.get_Visible") &&
           Param0(setVisible, "System.Boolean", "PlayerControl.set_Visible") &&
           Bind(getVisible, "PlayerControl.get_Visible", &g_pl.getVisible) &&
           Bind(setVisible, "PlayerControl.set_Visible", &g_pl.setVisible);
}

void InitPlayerExtras(void* pc, bool hookedFixed, void** infoOut) {
    *infoOut = nullptr;
    void* getData = il2::Method(pc, "get_Data", 0);
    Log("method PlayerControl.get_Data(0) %s, returns %s", getData ? "OK" : "MISSING",
        getData ? il2::ReturnTypeName(getData).c_str() : "-");
    void* infoClass = ReturnClass(getData);
    *infoOut = infoClass;
    if (!infoClass || !Bind(getData, "PlayerControl.get_Data", &g_pl.getData)) return;
    g_feat.reveal = hookedFixed && InitReveal(pc, infoClass);
    g_feat.ghosts = hookedFixed && InitGhosts(pc, infoClass);
    g_feat.invisible = g_feat.ghosts;
}

void InitPlayerActions(void* pc, void* infoClass, bool hookedFixed) {
    g_players.allPlayersField = il2::Field(pc, "AllPlayerControls");
    if (!g_players.allPlayersField || !il2::FieldIsStatic(g_players.allPlayersField)) return;
    bool listOk = InitList(g_players.allPlayers, g_players.allPlayersField, "PlayerControl", "AllPlayerControls list");
    void* idField = il2::Field(pc, "PlayerId");
    g_players.offId = il2::FieldOffset(idField);
    bool idOk = FieldIs(idField, g_players.offId, 1, "System.Byte", "PlayerControl.PlayerId");
    void* nameField = il2::Field(infoClass, "PlayerName");
    g_players.offName = il2::FieldOffset(nameField);
    bool nameOk = FieldIs(nameField, g_players.offName, 8, "System.String", "NetworkedPlayerInfo.PlayerName");
    void* disconnectedField = il2::Field(infoClass, "Disconnected");
    g_players.offDisconnected = il2::FieldOffset(disconnectedField);
    bool disconnectedOk = FieldIs(disconnectedField, g_players.offDisconnected, 1, "System.Boolean",
                                  "NetworkedPlayerInfo.Disconnected");
    g_feat.players = hookedFixed && listOk && idOk && nameOk && disconnectedOk && g_x.string_chars && g_pl.getData;

    void* netField = il2::Field(pc, "NetTransform");
    g_players.offNetTransform = il2::FieldOffset(netField);
    bool netOk = FieldIs(netField, g_players.offNetTransform, 8, nullptr, "PlayerControl.NetTransform");
    void* truePosition = il2::Method(pc, "GetTruePosition", 0);
    bool positionOk = Returns(truePosition, "UnityEngine.Vector2", "PlayerControl.GetTruePosition") &&
                      Bind(truePosition, "PlayerControl.GetTruePosition", &g_players.getTruePosition);
    void* snap = il2::Method(FieldClass(netField), "RpcSnapTo", 1);
    bool snapOk = Param0(snap, "UnityEngine.Vector2", "NetTransform.RpcSnapTo") &&
                  Returns(snap, "System.Void", "NetTransform.RpcSnapTo") &&
                  Bind(snap, "NetTransform.RpcSnapTo(1)", &g_players.rpcSnapTo);
    g_feat.teleport = g_feat.players && netOk && positionOk && snapOk;
    g_feat.bring = g_feat.teleport;

    void* moveableField = il2::Field(pc, "moveable");
    g_players.offMoveable = il2::FieldOffset(moveableField);
    bool moveableOk = FieldIs(moveableField, g_players.offMoveable, 1, "System.Boolean", "PlayerControl.moveable");
    g_players.setTarget = g_world.setTarget;
    g_feat.spectate = g_feat.players && moveableOk && g_world.setTarget && OffsetOk(g_world.offFollower, 8);

    void* report = il2::Method(pc, "CmdReportDeadBody", 1);
    g_feat.report = g_feat.players && Param0(report, "NetworkedPlayerInfo", "PlayerControl.CmdReportDeadBody") &&
                    Returns(report, "System.Void", "PlayerControl.CmdReportDeadBody") &&
                    Bind(report, "PlayerControl.CmdReportDeadBody", &g_players.cmdReport);

    void* tasksField = il2::Field(pc, "myTasks");
    g_players.offTasks = il2::FieldOffset(tasksField);
    void* complete = il2::Method(pc, "RpcCompleteTask", 1);
    bool taskListOk = FieldIs(tasksField, g_players.offTasks, 8, nullptr, "PlayerControl.myTasks") &&
                      InitList(g_players.tasks, tasksField, "PlayerTask", "myTasks list");
    bool completeOk = Param0(complete, "System.Int32", "PlayerControl.RpcCompleteTask") &&
                      Returns(complete, "System.Void", "PlayerControl.RpcCompleteTask") &&
                      Bind(complete, "PlayerControl.RpcCompleteTask", &g_players.rpcCompleteTask);
    void* taskType = FieldClass(tasksField);
    (void) taskType;  // The generic item class is reached through a completed task object below.
    // Resolve PlayerTask directly: FieldClass(myTasks) is List<PlayerTask>, not the task class.
    void* playerTaskClass = il2::FindClassIn("Assembly-CSharp.dll", "", "PlayerTask");
    void* taskId = il2::Method(playerTaskClass, "get_Id", 0);
    bool idReadOk = Returns(taskId, "System.Int32", "PlayerTask.get_Id") &&
                    Bind(taskId, "PlayerTask.get_Id", &g_players.getTaskId);
    g_feat.completeTasks = hookedFixed && taskListOk && completeOk && idReadOk;

    void* clientClass = il2::FindClassIn("Assembly-CSharp.dll", "", "AmongUsClient");
    g_players.clientInstance = FindFieldUp(clientClass, "Instance");
    void* modeField = il2::Field(clientClass, "NetworkMode");
    g_players.offNetworkMode = il2::FieldOffset(modeField);
    bool modeOk = FieldIs(modeField, g_players.offNetworkMode, 4, nullptr, "AmongUsClient.NetworkMode");
    void* hostField = il2::Field(clientClass, "AmHost");
    g_players.offAmHost = il2::FieldOffset(hostField);
    bool hostOk = FieldIs(hostField, g_players.offAmHost, 1, "System.Boolean", "AmongUsClient.AmHost");
    if (!modeOk) g_players.offNetworkMode = -1;
    if (!hostOk) g_players.offAmHost = -1;
}

}  // namespace

std::vector<PlayerEntry> GetPlayersSnapshot() {
    g_refreshPlayers.store(true);
    std::lock_guard<std::mutex> lk(g_snapshotMu);
    return g_snapshot;  // value copy: only ids, UTF-8 strings and flags, never Unity pointers
}

bool QueuePlayerAction(int action, int targetId) {
    if (action < ACTION_TELEPORT_TO || action > ACTION_COMPLETE_TASKS) return false;
    const bool needsTarget = action != ACTION_RETURN_CAMERA && action != ACTION_COMPLETE_TASKS;
    if (needsTarget && (targetId < 0 || targetId > 255)) return false;
    std::lock_guard<std::mutex> lk(g_actionMu);
    if (g_actionPending.load()) return false;
    static uint32_t issued = 0;
    g_command = {++issued, action, targetId};
    g_actionPending.store(true);
    g_actionMessage = "Player action queued for the next Unity tick";
    return true;
}

PlayerActionState GetPlayerActionState() {
    std::lock_guard<std::mutex> lk(g_actionMu);
    PlayerActionState state;
    state.completed = g_actionsCompleted;
    state.pending = g_actionPending.load();
    state.spectating = g_spectating.load();
    state.message = g_actionMessage;
    return state;
}

void InitFeatures() {
    Log("il2cpp ready, libil2cpp.so %p-%p", reinterpret_cast<void*>(il2::lib.base),
        reinterpret_cast<void*>(il2::lib.end));
    ResolveExtra(&g_x.class_from_type, "il2cpp_class_from_type");
    ResolveExtra(&g_x.class_get_parent, "il2cpp_class_get_parent");
    ResolveExtra(&g_x.field_get_type, "il2cpp_field_get_type");
    ResolveExtra(&g_x.method_get_return_type, "il2cpp_method_get_return_type");
    ResolveExtra(&g_x.type_get_name, "il2cpp_type_get_name");
    ResolveExtra(&g_x.string_chars, "il2cpp_string_chars");

    void* pc = il2::FindClassIn("Assembly-CSharp.dll", "", "PlayerControl");
    void* pp = il2::FindClassIn("Assembly-CSharp.dll", "", "PlayerPhysics");
    void* ship = il2::FindClassIn("Assembly-CSharp.dll", "", "ShipStatus");
    Log("classes: PlayerControl %s PlayerPhysics %s ShipStatus %s", pc ? "OK" : "MISSING", pp ? "OK" : "MISSING",
        ship ? "OK" : "MISSING");

    g_localPlayerField = il2::Field(pc, "LocalPlayer");
    void* physicsField = il2::Field(pc, "MyPhysics");
    void* colliderField = il2::Field(pc, "Collider");
    void* killField = il2::Field(pc, "killTimer");
    void* speedField = il2::Field(pp, "Speed");
    void* ghostSpeedField = il2::Field(pp, "GhostSpeed");
    void* ladderField = il2::Field(pc, "onLadder");
    g_offPhysics = il2::FieldOffset(physicsField);
    g_offCollider = il2::FieldOffset(colliderField);
    g_offKillTimer = il2::FieldOffset(killField);
    g_offSpeed = il2::FieldOffset(speedField);
    g_offGhostSpeed = il2::FieldOffset(ghostSpeedField);
    g_offOnLadder = il2::FieldOffset(ladderField);
    FieldIs(physicsField, g_offPhysics, 8, nullptr, "PlayerControl.MyPhysics");
    FieldIs(colliderField, g_offCollider, 8, nullptr, "PlayerControl.Collider");
    FieldIs(killField, g_offKillTimer, 4, "System.Single", "PlayerControl.killTimer");
    FieldIs(speedField, g_offSpeed, 4, "System.Single", "PlayerPhysics.Speed");
    if (!FieldIs(ghostSpeedField, g_offGhostSpeed, 4, "System.Single", "PlayerPhysics.GhostSpeed")) g_offGhostSpeed = -1;
    if (!FieldIs(ladderField, g_offOnLadder, 1, "System.Boolean", "PlayerControl.onLadder")) g_offOnLadder = -1;

    void* behaviour = il2::FindClass("UnityEngine", "Behaviour");
    void* setEnabled = il2::Method(behaviour, "set_enabled", 1);
    if (Param0(setEnabled, "System.Boolean", "Behaviour.set_enabled"))
        Bind(setEnabled, "UnityEngine.Behaviour.set_enabled", &g_setEnabled);

    void* fixedUpdate = il2::Method(pc, "FixedUpdate", 0);
    bool hookedFixed = false;
    if (g_localPlayerField && il2::FieldIsStatic(g_localPlayerField) && ValidTarget(fixedUpdate, "PlayerControl.FixedUpdate"))
        hookedFixed = InstallHook("PlayerControl.FixedUpdate", il2::MethodPointer(fixedUpdate), hkFixedUpdate,
                                  &o_fixedUpdate);

    g_feat.speed = hookedFixed && OffsetOk(g_offPhysics, 8) && OffsetOk(g_offSpeed);
    g_feat.kill = hookedFixed && OffsetOk(g_offKillTimer);
    g_feat.noclip = hookedFixed && OffsetOk(g_offCollider, 8) && g_setEnabled;
    InitWorld();
    void* infoClass = nullptr;
    InitPlayerExtras(pc, hookedFixed, &infoClass);
    InitPlayerActions(pc, infoClass, hookedFixed);

    void* light = il2::Method(ship, "CalculateLightRadius", 1);
    const bool lightOk = light && il2::ReturnTypeName(light) == "System.Single" && il2::ParamIsReference(light, 0) &&
                         ValidTarget(light, "ShipStatus.CalculateLightRadius");
    if (lightOk) g_feat.light = InstallHook("ShipStatus.CalculateLightRadius", il2::MethodPointer(light),
                                           hkLightRadius, &o_lightRadius);

    g_ready.store(true, std::memory_order_release);
    Log("features: speed=%d noclip=%d light=%d kill=%d safeZoom=%d wallVision=%d invisible=%d reveal=%d ghosts=%d",
        g_feat.speed, g_feat.noclip, g_feat.light, g_feat.kill, g_feat.zoom, g_feat.wallVision, g_feat.invisible,
        g_feat.reveal, g_feat.ghosts);
    Log("players=%d teleport=%d bring=%d spectate=%d report=%d completeTasks=%d", g_feat.players, g_feat.teleport,
        g_feat.bring, g_feat.spectate, g_feat.report, g_feat.completeTasks);
}
