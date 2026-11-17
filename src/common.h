#pragma once
#include <android/log.h>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#define AU_PACKAGE "com.innersloth.spacemafia"
#define AU_TAG "AUMod"

// Logs to logcat, to the in-game report list and to files/au_report.txt.
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> ReportSnapshot();
void PrepareDirs();

extern std::string g_dataDir;  // /data/user/0/<pkg>

// Multiplier ranges shared by both menus and by ConfigLoad's validation.
constexpr float kSpeedMulMin = 0.5f, kSpeedMulMax = 4.0f;
constexpr float kLightMulMin = 0.5f, kLightMulMax = 8.0f;
constexpr float kZoomMulMin = 1.0f, kZoomMulMax = 4.0f;

// Written by the menu thread (Java UI thread or the ImGui render thread), read by the game thread
// in the hooks. Plain bool/float fields: a torn read is impossible for these sizes on arm64, and a
// stale read only delays a toggle by one frame.
struct Config {
    bool speedOn = false;
    float speedMul = 1.5f;
    bool noclip = false;
    bool lightOn = false;
    float lightMul = 3.0f;
    bool killOn = false;
    bool zoomOn = false;
    float zoomMul = 1.5f;
    bool revealOn = false;
    bool ghostsOn = false;
};
extern Config g_cfg;
void ConfigLoad();
void ConfigSave();  // thread-safe; replaces files/au_cfg.txt atomically

// Set from the menu, consumed by the worker thread.
extern std::atomic<bool> g_requestDump;
