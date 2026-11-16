#pragma once
#include <jni.h>

struct TouchEvent {
    enum Type { Down, Move, Up } type;
    float x, y;
};

// Render side (eglSwapBuffers hook, ImGui). Blocks while waiting for libunity.so, run it on its own thread.
void InstallEglHooks();

// Input side (JNI RegisterNatives hook on UnityPlayer's injectEvent).
void InstallInputHook(JNIEnv* env);
bool InputWrapped();

// Shared between the two.
void PushTouch(const TouchEvent& e);
bool PopTouch(TouchEvent& e);
void PublishUiRect(int slot, float x, float y, float w, float h, bool active);
bool PointInUi(float x, float y);
