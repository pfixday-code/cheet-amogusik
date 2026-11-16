#include "overlay.h"
#include "common.h"
#include "hook.h"
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

namespace {

std::mutex g_mu;
std::deque<TouchEvent> g_events;
struct Rect { float x = 0, y = 0, w = 0, h = 0; bool active = false; } g_rects[4];

jint (*o_registerNatives)(JNIEnv*, jclass, const JNINativeMethod*, jint) = nullptr;
jboolean (*o_inject1)(JNIEnv*, jobject, jobject) = nullptr;
jboolean (*o_inject2)(JNIEnv*, jobject, jobject, jint) = nullptr;

jclass g_motionClass = nullptr;
jmethodID g_getActionMasked = nullptr, g_getX = nullptr, g_getY = nullptr;
bool g_capturing = false;

bool Consume(JNIEnv* env, jobject event) {
    if (!event) return false;
    if (!g_motionClass) {
        jclass c = env->FindClass("android/view/MotionEvent");
        if (!c) { env->ExceptionClear(); return false; }
        g_motionClass = static_cast<jclass>(env->NewGlobalRef(c));
        env->DeleteLocalRef(c);
        g_getActionMasked = env->GetMethodID(g_motionClass, "getActionMasked", "()I");
        g_getX = env->GetMethodID(g_motionClass, "getX", "()F");
        g_getY = env->GetMethodID(g_motionClass, "getY", "()F");
        if (!g_getActionMasked || !g_getX || !g_getY) { env->ExceptionClear(); g_motionClass = nullptr; return false; }
    }
    if (!env->IsInstanceOf(event, g_motionClass)) return false;

    const int action = env->CallIntMethod(event, g_getActionMasked);
    const float x = env->CallFloatMethod(event, g_getX);
    const float y = env->CallFloatMethod(event, g_getY);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    constexpr int kDown = 0, kUp = 1, kMove = 2, kCancel = 3;
    if (action == kDown) {
        if (!PointInUi(x, y)) return false;
        g_capturing = true;
        PushTouch({TouchEvent::Down, x, y});
        return true;
    }
    if (!g_capturing) return false;
    if (action == kMove) {
        PushTouch({TouchEvent::Move, x, y});
    } else if (action == kUp || action == kCancel) {
        PushTouch({TouchEvent::Up, x, y});
        g_capturing = false;
    }
    return true;  // the whole gesture belongs to the menu
}

jboolean hkInject1(JNIEnv* env, jobject thiz, jobject event) {
    if (Consume(env, event)) return JNI_TRUE;
    return o_inject1(env, thiz, event);
}

jboolean hkInject2(JNIEnv* env, jobject thiz, jobject event, jint extra) {
    if (Consume(env, event)) return JNI_TRUE;
    return o_inject2(env, thiz, event, extra);
}

jint hkRegisterNatives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods, jint n) {
    if (!methods || n <= 0) return o_registerNatives(env, clazz, methods, n);
    std::vector<JNINativeMethod> copy(methods, methods + n);
    for (auto& m : copy) {
        if (!m.name || !m.signature) continue;
        bool nameMatches = strcasestr(m.name, "InjectEvent") != nullptr;
        if (!nameMatches) continue;
        if (!strcmp(m.signature, "(Landroid/view/InputEvent;)Z") && !o_inject1) {
            o_inject1 = reinterpret_cast<decltype(o_inject1)>(m.fnPtr);
            m.fnPtr = reinterpret_cast<void*>(hkInject1);
            Log("input: wrapped %s%s", m.name, m.signature);
        } else if (!strcmp(m.signature, "(Landroid/view/InputEvent;I)Z") && !o_inject2) {
            o_inject2 = reinterpret_cast<decltype(o_inject2)>(m.fnPtr);
            m.fnPtr = reinterpret_cast<void*>(hkInject2);
            Log("input: wrapped %s%s", m.name, m.signature);
        }
    }
    return o_registerNatives(env, clazz, copy.data(), n);
}

}  // namespace

void PushTouch(const TouchEvent& e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_events.size() > 256) g_events.pop_front();
    g_events.push_back(e);
}

bool PopTouch(TouchEvent& e) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_events.empty()) return false;
    e = g_events.front();
    g_events.pop_front();
    return true;
}

void PublishUiRect(int slot, float x, float y, float w, float h, bool active) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_rects[slot] = {x, y, w, h, active};
}

bool PointInUi(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& r : g_rects)
        if (r.active && x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) return true;
    return false;
}

bool InputWrapped() { return o_inject1 || o_inject2; }

void InstallInputHook(JNIEnv* env) {
    // Must be installed before libunity registers its natives (its JNI_OnLoad runs on this same
    // thread, so it sees this env). A private copy of the function table is swapped in: no code in
    // libart is patched, so no other thread can ever execute half-written instructions.
    static JNINativeInterface table;
    if (!env || !env->functions) {
        Log("input: no JNIEnv, touch passthrough disabled");
        return;
    }
    table = *env->functions;
    o_registerNatives = env->functions->RegisterNatives;
    table.RegisterNatives = hkRegisterNatives;
    env->functions = &table;
    Log("input: JNI function table swapped (RegisterNatives %p -> %p)",
        reinterpret_cast<void*>(o_registerNatives), reinterpret_cast<void*>(hkRegisterNatives));
}
