#include "common.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

std::string g_dataDir;
Config g_cfg;
std::atomic<bool> g_requestDump{false};

namespace {
std::mutex g_mu;     // report lines and report files
std::mutex g_cfgMu;  // au_cfg.txt; ConfigSave can be called from two UI threads
std::vector<std::string> g_lines;

constexpr size_t kMaxReportLines = 300;
constexpr int kMaxConfigLines = 64;

std::string PrimaryDir() { return g_dataDir + "/files"; }
std::string ExternalParent() { return std::string("/sdcard/Android/data/") + AU_PACKAGE; }
std::string ExternalDir() { return ExternalParent() + "/files"; }
std::string ConfigPath() { return PrimaryDir() + "/au_cfg.txt"; }

void MkDir(const std::string& d) { mkdir(d.c_str(), 0771); }

void AppendFile(const std::string& dir, const char* line) {
    FILE* f = fopen((dir + "/au_report.txt").c_str(), "a");
    if (!f) return;
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

float Sanitize(float v, float lo, float hi, float fallback) {
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

bool BlankLine(const char* s) {
    for (; *s; ++s)
        if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return false;
    return true;
}
}  // namespace

void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, AU_TAG, "%s", buf);
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_lines.size() >= kMaxReportLines) g_lines.erase(g_lines.begin());
    g_lines.emplace_back(buf);
    if (!g_dataDir.empty()) {
        AppendFile(PrimaryDir(), buf);
        AppendFile(ExternalDir(), buf);
    }
}

std::vector<std::string> ReportSnapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_lines;
}

void ConfigLoad() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    FILE* f = fopen(ConfigPath().c_str(), "r");
    if (!f) return;
    const Config defaults;
    char line[128];
    int lines = 0, bad = 0;
    while (lines < kMaxConfigLines && fgets(line, sizeof(line), f)) {
        ++lines;
        if (BlankLine(line)) continue;
        char key[64];
        float v = 0.f;
        if (sscanf(line, " %63[^= \t] = %f", key, &v) != 2 || !std::isfinite(v)) {
            ++bad;
            continue;
        }
        if (!strcmp(key, "speedOn")) g_cfg.speedOn = v != 0.f;
        else if (!strcmp(key, "speedMul")) g_cfg.speedMul = v;
        else if (!strcmp(key, "noclip")) g_cfg.noclip = v != 0.f;
        else if (!strcmp(key, "lightOn")) g_cfg.lightOn = v != 0.f;
        else if (!strcmp(key, "lightMul")) g_cfg.lightMul = v;
        else if (!strcmp(key, "killOn")) g_cfg.killOn = v != 0.f;
        else if (!strcmp(key, "zoomOn")) g_cfg.zoomOn = v != 0.f;
        else if (!strcmp(key, "zoomMul")) g_cfg.zoomMul = v;
        else if (!strcmp(key, "revealOn")) g_cfg.revealOn = v != 0.f;
        else if (!strcmp(key, "ghostsOn")) g_cfg.ghostsOn = v != 0.f;
        else if (!strcmp(key, "wallVisionOn")) g_cfg.wallVisionOn = v != 0.f;
        else if (!strcmp(key, "invisibleOn")) g_cfg.invisibleOn = v != 0.f;
        else ++bad;
    }
    fclose(f);
    // A hand-edited or corrupted file must not push NaN or absurd multipliers into the game.
    g_cfg.speedMul = Sanitize(g_cfg.speedMul, kSpeedMulMin, kSpeedMulMax, defaults.speedMul);
    g_cfg.lightMul = Sanitize(g_cfg.lightMul, kLightMulMin, kLightMulMax, defaults.lightMul);
    g_cfg.zoomMul = Sanitize(g_cfg.zoomMul, kZoomMulMin, kZoomMulMax, defaults.zoomMul);
    if (bad) Log("config: %d unreadable line(s) in au_cfg.txt ignored", bad);
}

void ConfigSave() {
    std::lock_guard<std::mutex> lk(g_cfgMu);
    const Config c = g_cfg;  // one snapshot, so the file is self-consistent
    const std::string path = ConfigPath();
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        Log("config: cannot write %s (%s)", tmp.c_str(), strerror(errno));
        return;
    }
    const int n = fprintf(f,
                          "speedOn=%d\nspeedMul=%.3f\nnoclip=%d\nlightOn=%d\nlightMul=%.3f\nkillOn=%d\n"
                          "zoomOn=%d\nzoomMul=%.3f\nrevealOn=%d\nghostsOn=%d\nwallVisionOn=%d\ninvisibleOn=%d\n",
                          c.speedOn, c.speedMul, c.noclip, c.lightOn, c.lightMul, c.killOn, c.zoomOn,
                          c.zoomMul, c.revealOn, c.ghostsOn, c.wallVisionOn, c.invisibleOn);
    const bool written = n > 0 && fflush(f) == 0;
    if (fclose(f) != 0 || !written) {
        Log("config: write to %s failed", tmp.c_str());
        remove(tmp.c_str());
        return;
    }
    // rename() replaces the old file in one step: a crash mid-save leaves the previous config intact.
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        Log("config: rename to %s failed (%s)", path.c_str(), strerror(errno));
        remove(tmp.c_str());
    }
}

void PrepareDirs() {
    MkDir(PrimaryDir());
    MkDir(ExternalParent());  // mkdir() does not create missing parents
    MkDir(ExternalDir());
    remove((PrimaryDir() + "/au_report.txt").c_str());
    remove((ExternalDir() + "/au_report.txt").c_str());
}
