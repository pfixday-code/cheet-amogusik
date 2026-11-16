#include "overlay.h"
#include "common.h"
#include "features.h"
#include "hook.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cstdint>
#include <dlfcn.h>
#include <unistd.h>
#include "elf.h"
#include <sys/stat.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"

namespace {

EGLBoolean (*o_swap)(EGLDisplay, EGLSurface) = nullptr;
EGLBoolean (*o_swapDamage)(EGLDisplay, EGLSurface, EGLint*, EGLint) = nullptr;
thread_local bool t_inHook = false;

// How long a hook path gets to prove it is on Unity's render path before the next one is tried.
constexpr int kProbeSec = 15;

// Counts every entry into a swap hook, before any filtering. A patched GOT slot or an installed
// trampoline only proves the hook exists; this proves the game actually calls it.
std::atomic<uint64_t> g_hookCalls{0};
std::atomic<bool> g_loggedSkip{false};

EGLContext g_ctx = EGL_NO_CONTEXT;
bool g_ready = false;
bool g_menuOpen = false;
float g_fontPx = 0.f;
std::chrono::steady_clock::time_point g_last;
bool g_loggedFirstFrame = false;

bool FileExists(const char* p) { struct stat st; return stat(p, &st) == 0; }

void Shutdown() {
    if (!g_ready) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    g_ready = false;
}

void Setup(int height) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;

    g_fontPx = std::clamp(height * 0.028f, 16.f, 44.f);
    static const char* fonts[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                                  "/system/fonts/Roboto[wdth,wght].ttf", "/system/fonts/DroidSans.ttf"};
    ImFont* font = nullptr;
    for (const char* p : fonts) {
        if (FileExists(p)) { font = io.Fonts->AddFontFromFileTTF(p, g_fontPx); if (font) break; }
    }
    if (!font) {
        ImFontConfig cfg;
        cfg.SizePixels = g_fontPx;
        io.Fonts->AddFontDefault(&cfg);
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.ScaleAllSizes(g_fontPx / 16.f);
    st.WindowRounding = 10.f * (g_fontPx / 16.f);
    st.FrameRounding = 6.f * (g_fontPx / 16.f);
    st.GrabMinSize = g_fontPx * 1.2f;
    st.ScrollbarSize = g_fontPx * 0.9f;

    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_last = std::chrono::steady_clock::now();
    g_ready = true;
    Log("overlay: ImGui ready (font %.0fpx, %s)", g_fontPx, font ? "Roboto" : "built-in");
}

void FeedInput() {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    TouchEvent e;
    while (PopTouch(e)) {
        io.AddMousePosEvent(e.x, e.y);
        if (e.type == TouchEvent::Down) io.AddMouseButtonEvent(0, true);
        if (e.type == TouchEvent::Up) {
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);  // drop hover once the finger lifts
        }
    }
}

void DrawMenu(float w, float h) {
    const float btn = g_fontPx * 2.4f;
    ImGui::SetNextWindowPos(ImVec2(g_fontPx * 0.5f, g_fontPx * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(btn, btn));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##au_toggle", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);
    if (ImGui::Button(g_menuOpen ? "X" : "AU", ImVec2(btn, btn))) g_menuOpen = !g_menuOpen;
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    PublishUiRect(0, p.x, p.y, s.x, s.y, true);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (!g_menuOpen) {
        PublishUiRect(1, 0, 0, 0, 0, false);
        return;
    }

    const float pw = std::min(w * 0.55f, g_fontPx * 22.f);
    const float ph = std::min(h * 0.85f, g_fontPx * 28.f);
    ImGui::SetNextWindowPos(ImVec2(g_fontPx * 0.5f, g_fontPx * 0.5f + btn + g_fontPx * 0.4f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_FirstUseEver);
    ImGui::Begin("Among Us", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);

    bool changed = false;
    ImGui::PushItemWidth(-1);

    ImGui::BeginDisabled(!g_feat.speed);
    changed |= ImGui::Checkbox("Speed", &g_cfg.speedOn);
    changed |= ImGui::SliderFloat("##speed", &g_cfg.speedMul, kSpeedMulMin, kSpeedMulMax, "x%.2f");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!g_feat.light);
    changed |= ImGui::Checkbox("Vision radius", &g_cfg.lightOn);
    changed |= ImGui::SliderFloat("##light", &g_cfg.lightMul, kLightMulMin, kLightMulMax, "x%.2f");
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!g_feat.noclip);
    changed |= ImGui::Checkbox("Noclip", &g_cfg.noclip);
    ImGui::EndDisabled();

    ImGui::BeginDisabled(!g_feat.kill);
    changed |= ImGui::Checkbox("Zero kill cooldown (local)", &g_cfg.killOn);
    ImGui::EndDisabled();
    ImGui::PopItemWidth();

    if (changed) ConfigSave();

    ImGui::Separator();
    ImGui::TextDisabled("Local player: %s", g_feat.localPlayerSeen ? "found" : "not in a game");
    if (ImGui::Button("Dump classes to au_dump.txt")) g_requestDump = true;

    if (ImGui::CollapsingHeader("Report")) {
        ImGui::BeginChild("##report", ImVec2(0, g_fontPx * 10.f), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& line : ReportSnapshot()) ImGui::TextUnformatted(line.c_str());
        ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }

    ImVec2 p2 = ImGui::GetWindowPos(), s2 = ImGui::GetWindowSize();
    PublishUiRect(1, p2.x, p2.y, s2.x, s2.y, true);
    ImGui::End();
}

void LogSkipOnce(const char* why, EGLint w, EGLint h) {
    if (!g_loggedSkip.exchange(true)) Log("overlay: swap hook reached but frame skipped: %s (%dx%d)", why, w, h);
}

void RenderFrame(EGLDisplay dpy, EGLSurface surf) {
    EGLint w = 0, h = 0;
    if (!eglQuerySurface(dpy, surf, EGL_WIDTH, &w) || !eglQuerySurface(dpy, surf, EGL_HEIGHT, &h)) {
        LogSkipOnce("eglQuerySurface failed", w, h);
        return;
    }
    if (w < 200 || h < 200) {
        LogSkipOnce("surface smaller than 200px", w, h);
        return;
    }
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) {
        LogSkipOnce("no current EGL context", w, h);
        return;
    }

    if (ctx != g_ctx) {
        Shutdown();
        g_ctx = ctx;
        Setup(h);
    }
    if (!g_loggedFirstFrame) {
        g_loggedFirstFrame = true;
        Log("overlay: first frame %dx%d", w, h);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::max(1e-4f, std::chrono::duration<float>(now - g_last).count());
    g_last = now;

    FeedInput();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    DrawMenu(static_cast<float>(w), static_cast<float>(h));
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

EGLBoolean hkSwap(EGLDisplay d, EGLSurface s) {
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);
    // Android's eglSwapBuffers calls eglSwapBuffersWithDamageKHR internally; keep the guard
    // set across the original call so the overlay is drawn once per frame. The same guard keeps
    // the frame single when both the GOT path and the inline path end up active.
    if (t_inHook) return o_swap(d, s);
    t_inHook = true;
    RenderFrame(d, s);
    EGLBoolean r = o_swap(d, s);
    t_inHook = false;
    return r;
}

EGLBoolean hkSwapDamage(EGLDisplay d, EGLSurface s, EGLint* rects, EGLint n) {
    g_hookCalls.fetch_add(1, std::memory_order_relaxed);
    if (t_inHook) return o_swapDamage(d, s, rects, n);
    t_inHook = true;
    RenderFrame(d, s);
    EGLBoolean r = o_swapDamage(d, s, rects, n);
    t_inHook = false;
    return r;
}

bool WaitForSwapCalls(int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (g_hookCalls.load(std::memory_order_relaxed) > 0) return true;
        usleep(100 * 1000);
    }
    return false;
}

// Inline hooks in libEGL. And64 writes 16-20 bytes, so a function shorter than that
// (eglSwapBuffers is usually a 3-instruction wrapper around WithDamage) must not be hooked.
// Only reached when the GOT path delivered zero calls during the probe, so nothing is forwarding
// through o_swap / o_swapDamage while InstallHook rewrites them (see the caveat in hook.h).
bool InstallInlineFallback() {
    constexpr size_t kMinHookBytes = 20;
    LoadedModule eglMod;
    if (!FindModule("libEGL.so", eglMod)) {
        Log("overlay: libEGL.so module not found, overlay disabled");
        return false;
    }
    const size_t sizeSwap = FindExportSize(eglMod, "eglSwapBuffers");
    const size_t sizeDamage = FindExportSize(eglMod, "eglSwapBuffersWithDamageKHR");
    Log("overlay: inline hook, sizes: eglSwapBuffers=%zu WithDamage=%zu (need >= %zu)", sizeSwap,
        sizeDamage, kMinHookBytes);
    bool any = false;
    if (o_swapDamage && sizeDamage >= kMinHookBytes)
        any |= InstallHook("eglSwapBuffersWithDamageKHR", reinterpret_cast<void*>(o_swapDamage), hkSwapDamage, &o_swapDamage);
    if (sizeSwap >= kMinHookBytes)
        any |= InstallHook("eglSwapBuffers", reinterpret_cast<void*>(o_swap), hkSwap, &o_swap);
    else
        Log("overlay: eglSwapBuffers is too small to hook, relying on its call into WithDamage");
    if (!any) Log("overlay: no safe inline hook point, overlay disabled");
    return any;
}

}  // namespace

void InstallEglHooks() {
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) {
        Log("overlay: dlopen libEGL.so failed");
        return;
    }
    // Real implementations, taken from libEGL by name; they are what the hooks forward to.
    o_swap = reinterpret_cast<decltype(o_swap)>(dlsym(egl, "eglSwapBuffers"));
    o_swapDamage = reinterpret_cast<decltype(o_swapDamage)>(dlsym(egl, "eglSwapBuffersWithDamageKHR"));
    Log("overlay: libEGL eglSwapBuffers=%p eglSwapBuffersWithDamageKHR=%p",
        reinterpret_cast<void*>(o_swap), reinterpret_cast<void*>(o_swapDamage));
    if (!o_swap) return;

    bool gotPatched = false;
    LoadedModule unity;
    for (int i = 0; i < 12000 && !FindModule("libunity.so", unity); ++i) usleep(5 * 1000);
    if (unity.valid()) {
        Log("overlay: libunity.so at %p-%p", reinterpret_cast<void*>(unity.base), reinterpret_cast<void*>(unity.end));
        const int a = PatchImports(unity, "eglSwapBuffers", reinterpret_cast<void*>(hkSwap));
        const int b = o_swapDamage ? PatchImports(unity, "eglSwapBuffersWithDamageKHR",
                                                  reinterpret_cast<void*>(hkSwapDamage)) : 0;
        Log("overlay: libunity GOT patched: eglSwapBuffers=%d eglSwapBuffersWithDamageKHR=%d", a, b);
        gotPatched = a + b > 0;
    } else {
        Log("overlay: libunity.so not found");
    }

    // A patched slot is not proof: Unity can call EGL through a pointer it fetched with
    // dlsym/eglGetProcAddress, which never touches the GOT. Require real calls before trusting it.
    if (gotPatched) {
        if (WaitForSwapCalls(kProbeSec)) {
            Log("overlay: GOT path is live (%llu swap calls)",
                static_cast<unsigned long long>(g_hookCalls.load()));
            return;
        }
        Log("overlay: GOT slots patched but no swap went through them in %d s; adding inline hooks", kProbeSec);
    }

    if (!InstallInlineFallback()) return;
    if (WaitForSwapCalls(kProbeSec)) {
        Log("overlay: inline path is live (%llu swap calls)", static_cast<unsigned long long>(g_hookCalls.load()));
        return;
    }
    LoadedModule vk;
    const bool vulkanMapped = FindModule("libvulkan.so", vk);
    Log("overlay: no eglSwapBuffers call in %d s after inline hooks (libvulkan.so %s). If Unity renders with "
        "Vulkan on this device, this GLES overlay cannot draw; check the graphics API in `adb logcat -s Unity`",
        kProbeSec, vulkanMapped ? "mapped" : "not mapped");
}
