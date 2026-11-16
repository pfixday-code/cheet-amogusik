#include "common.h"
#include "features.h"
#include "il2cpp.h"
#include "java_menu.h"
#include "overlay.h"
#include "zygisk.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

// Extra margin after il2cpp_init has finished, before the first lookup.
constexpr int kRuntimeSettleMs = 3000;

std::atomic<bool> g_started{false};

void Il2cppWorker() {
    Log("stage: waiting for libil2cpp.so");
    LoadedModule m;
    for (int i = 0; i < 12000 && !FindModule("libil2cpp.so", m); ++i) usleep(50 * 1000);
    if (!m.valid()) {
        Log("libil2cpp.so never appeared");
        return;
    }
    Log("stage: libil2cpp.so mapped at %p-%p", reinterpret_cast<void*>(m.base), reinterpret_cast<void*>(m.end));
    if (!il2::Load()) {
        Log("required il2cpp exports are missing, features disabled");
        return;
    }
    Log("stage: il2cpp exports resolved");

    if (!il2::WaitRuntime(600)) {
        Log("il2cpp_init did not finish within 600 s");
        return;
    }
    Log("stage: il2cpp_init finished, settling %d ms before touching the runtime", kRuntimeSettleMs);
    usleep(kRuntimeSettleMs * 1000);
    if (!il2::Domain()) {
        Log("il2cpp domain is null");
        return;
    }

    // Every il2cpp call below needs a registered thread; calling class lookups from an unregistered
    // thread while the runtime starts is what killed the process before.
    il2::AttachThread();
    if (!il2::WaitReady(180)) {
        Log("Assembly-CSharp / PlayerControl not visible before timeout");
        return;
    }
    Log("stage: Assembly-CSharp ready, installing features");
    InitFeatures();
    Log("stage: features initialised");

    // Renderer-independent menu; the ImGui overlay stays as a second UI when GLES frames reach it.
    StartJavaMenu();

    const std::string dumpPath = g_dataDir + "/files/au_dump.txt";
    for (;;) {
        if (g_requestDump.exchange(false)) il2::Dump(dumpPath.c_str());
        sleep(1);
    }
}

void InputWatchdog() {
    for (int i = 0; i < 60 && !InputWrapped(); ++i) sleep(1);
    if (!InputWrapped())
        Log("input: UnityPlayer.nativeInjectEvent was NOT wrapped within 60 s, the menu will not receive touches");
}

void StartModule(JNIEnv* env, const std::string& dataDir) {
    if (g_started.exchange(true)) return;
    g_dataDir = dataDir;
    PrepareDirs();
    ConfigLoad();
    Log("module loaded in %s (build %s %s)", AU_PACKAGE, __DATE__, __TIME__);
    if (env && env->GetJavaVM(&g_vm) != JNI_OK) {
        g_vm = nullptr;
        Log("java menu: GetJavaVM failed");
    }
    InstallInputHook(env);
    std::thread(InstallEglHooks).detach();
    std::thread(Il2cppWorker).detach();
    std::thread(InputWatchdog).detach();
}

class AmongUsModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        // nice_name / app_data_dir can be null for some zygote children.
        const char* nice = args->nice_name ? env_->GetStringUTFChars(args->nice_name, nullptr) : nullptr;
        const char* dir = args->app_data_dir ? env_->GetStringUTFChars(args->app_data_dir, nullptr) : nullptr;
        target_ = nice && strcmp(nice, AU_PACKAGE) == 0;
        if (target_ && dir) dataDir_ = dir;
        if (nice) env_->ReleaseStringUTFChars(args->nice_name, nice);
        if (dir) env_->ReleaseStringUTFChars(args->app_data_dir, dir);
        if (!target_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs*) override {
        if (!target_ || dataDir_.empty()) return;
        StartModule(env_, dataDir_);
    }

private:
    zygisk::Api* api_ = nullptr;
    JNIEnv* env_ = nullptr;
    bool target_ = false;
    std::string dataDir_;
};

bool PackageFromCmdline(char* out, size_t cap) {
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return n > 0;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void zygisk_module_entry(
    zygisk::internal::api_table* table, JNIEnv* env) {
    zygisk::internal::entry_impl<AmongUsModule>(table, env);
}

// No-root entry: when the library is packed into a repackaged APK and loaded with System.loadLibrary
// before libmain/libunity, the VM calls this on the main thread. Zygisk never calls it.
extern "C" __attribute__((visibility("default"))) jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) return JNI_VERSION_1_6;
    char pkg[128];
    if (!PackageFromCmdline(pkg, sizeof(pkg)) || strcmp(pkg, AU_PACKAGE) != 0) return JNI_VERSION_1_6;
    std::string dir = std::string("/data/user/0/") + pkg;
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) dir = std::string("/data/data/") + pkg;
    StartModule(env, dir);
    return JNI_VERSION_1_6;
}
