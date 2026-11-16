#include "il2cpp.h"
#include "common.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace il2 {

LoadedModule lib;

namespace {

#define REQUIRED_EXPORTS(X) \
    X(void*, domain_get, ()) \
    X(void**, domain_get_assemblies, (void*, size_t*)) \
    X(void*, assembly_get_image, (void*)) \
    X(const char*, image_get_name, (void*)) \
    X(void*, class_from_name, (void*, const char*, const char*)) \
    X(void*, class_get_field_from_name, (void*, const char*)) \
    X(int, field_get_offset, (void*)) \
    X(void, field_static_get_value, (void*, void*)) \
    X(void*, class_get_method_from_name, (void*, const char*, int)) \
    X(void*, thread_attach, (void*))

#define OPTIONAL_EXPORTS(X) \
    X(void*, method_get_return_type, (void*)) \
    X(void*, method_get_param, (void*, uint32_t)) \
    X(uint32_t, method_get_param_count, (void*)) \
    X(const char*, method_get_name, (void*)) \
    X(const char*, method_get_param_name, (void*, uint32_t)) \
    X(const char*, type_get_name, (void*)) \
    X(int, type_get_type, (void*)) \
    X(size_t, image_get_class_count, (void*)) \
    X(void*, image_get_class, (void*, size_t)) \
    X(const char*, class_get_name, (void*)) \
    X(const char*, class_get_namespace, (void*)) \
    X(void*, class_get_parent, (void*)) \
    X(void*, class_get_fields, (void*, void**)) \
    X(void*, class_get_methods, (void*, void**)) \
    X(const char*, field_get_name, (void*)) \
    X(void*, field_get_type, (void*)) \
    X(int, field_get_flags, (void*)) \
    X(bool, is_vm_thread, (void*))

struct Fns {
#define DECL(ret, name, args) ret(*name) args = nullptr;
    REQUIRED_EXPORTS(DECL)
    OPTIONAL_EXPORTS(DECL)
#undef DECL
} f;

constexpr int kTypeClass = 0x12;
constexpr int kTypeString = 0x0e;
constexpr int kTypeObject = 0x1c;
constexpr int kTypeSzArray = 0x1d;
constexpr int kTypeGenericInst = 0x15;

}  // namespace

bool Load() {
    if (!FindModule("libil2cpp.so", lib)) return false;
    bool ok = true;
#define RESOLVE_REQ(ret, name, args) \
    f.name = reinterpret_cast<decltype(f.name)>(FindExport(lib, "il2cpp_" #name)); \
    if (!f.name) { Log("missing required export il2cpp_" #name); ok = false; }
#define RESOLVE_OPT(ret, name, args) \
    f.name = reinterpret_cast<decltype(f.name)>(FindExport(lib, "il2cpp_" #name)); \
    if (!f.name) Log("missing optional export il2cpp_" #name);
    REQUIRED_EXPORTS(RESOLVE_REQ)
    OPTIONAL_EXPORTS(RESOLVE_OPT)
#undef RESOLVE_REQ
#undef RESOLVE_OPT
    return ok;
}

void* Domain() { return f.domain_get ? f.domain_get() : nullptr; }

// il2cpp_domain_get() must not be used as the readiness probe: before il2cpp_init it allocates the
// domain from a garbage collector that is not initialised yet. il2cpp_is_vm_thread(nullptr) only
// reads a flag and turns true once il2cpp_init has finished.
bool WaitRuntime(int timeoutSec) {
    if (!f.is_vm_thread) {
        Log("il2cpp: il2cpp_is_vm_thread missing, falling back to a fixed 20 s delay");
        sleep(20);
        return true;
    }
    for (int i = 0; i < timeoutSec * 10; ++i) {
        if (f.is_vm_thread(nullptr)) return true;
        usleep(100 * 1000);
    }
    return false;
}

void AttachThread() {
    if (void* d = Domain()) {
        void* t = f.thread_attach(d);
        Log("il2cpp: thread attached (%p)", t);
    }
}

void* FindClassIn(const char* imageName, const char* ns, const char* name) {
    void* dom = Domain();
    if (!dom) return nullptr;
    size_t n = 0;
    void** asms = f.domain_get_assemblies(dom, &n);
    for (size_t i = 0; asms && i < n; ++i) {
        void* img = f.assembly_get_image(asms[i]);
        const char* in = img ? f.image_get_name(img) : nullptr;
        if (!in) continue;
        if (imageName && strcmp(in, imageName) != 0) continue;
        if (void* k = f.class_from_name(img, ns, name)) return k;
    }
    return nullptr;
}

void* FindClass(const char* ns, const char* name) { return FindClassIn(nullptr, ns, name); }

bool HasImage(const char* imageName) {
    void* dom = Domain();
    if (!dom) return false;
    size_t n = 0;
    void** asms = f.domain_get_assemblies(dom, &n);
    for (size_t i = 0; asms && i < n; ++i) {
        void* img = f.assembly_get_image(asms[i]);
        const char* in = img ? f.image_get_name(img) : nullptr;
        if (in && strcmp(in, imageName) == 0) return true;
    }
    return false;
}

bool FieldIsStatic(void* field) {
    if (!field) return false;
    if (!f.field_get_flags) return true;  // cannot verify
    return (f.field_get_flags(field) & 0x10) != 0;
}

// Must run on a thread that has been attached with AttachThread().
bool WaitReady(int timeoutSec) {
    for (int i = 0; i < timeoutSec * 5; ++i) {
        if (HasImage("Assembly-CSharp.dll") && FindClassIn("Assembly-CSharp.dll", "", "PlayerControl")) return true;
        usleep(200 * 1000);
    }
    return false;
}

void* Field(void* klass, const char* name) {
    return klass ? f.class_get_field_from_name(klass, name) : nullptr;
}
int FieldOffset(void* field) { return field ? f.field_get_offset(field) : -1; }
bool StaticGet(void* field, void* out) {
    if (!field) return false;
    f.field_static_get_value(field, out);
    return true;
}
void* Method(void* klass, const char* name, int argc) {
    return klass ? f.class_get_method_from_name(klass, name, argc) : nullptr;
}

std::string ReturnTypeName(void* method) {
    if (!method || !f.method_get_return_type || !f.type_get_name) return "?";
    void* t = f.method_get_return_type(method);
    const char* n = t ? f.type_get_name(t) : nullptr;
    return n ? n : "?";
}

std::string ParamTypeName(void* method, int index) {
    if (!method || !f.method_get_param || !f.type_get_name) return "?";
    void* t = f.method_get_param(method, index);
    const char* n = t ? f.type_get_name(t) : nullptr;
    return n ? n : "?";
}

bool ParamIsReference(void* method, int index) {
    if (!method || !f.method_get_param || !f.type_get_type) return false;
    void* t = f.method_get_param(method, index);
    if (!t) return false;
    int ty = f.type_get_type(t);
    return ty == kTypeClass || ty == kTypeString || ty == kTypeObject || ty == kTypeSzArray ||
           ty == kTypeGenericInst;
}

void Dump(const char* path) {
    if (!f.image_get_class_count || !f.image_get_class || !f.class_get_name || !f.class_get_fields ||
        !f.class_get_methods) {
        Log("dump: optional il2cpp exports missing");
        return;
    }
    FILE* out = fopen(path, "w");
    if (!out) {
        Log("dump: cannot open %s", path);
        return;
    }
    fprintf(out, "// libil2cpp %p-%p\n", reinterpret_cast<void*>(lib.base), reinterpret_cast<void*>(lib.end));
    void* dom = Domain();
    size_t na = 0;
    void** asms = f.domain_get_assemblies(dom, &na);
    size_t classes = 0;
    for (size_t a = 0; asms && a < na; ++a) {
        void* img = f.assembly_get_image(asms[a]);
        const char* in = img ? f.image_get_name(img) : nullptr;
        if (!in || strcmp(in, "Assembly-CSharp.dll") != 0) continue;
        fprintf(out, "// image %s\n", in);
        size_t nc = f.image_get_class_count(img);
        for (size_t c = 0; c < nc; ++c) {
            void* k = f.image_get_class(img, c);
            if (!k) continue;
            ++classes;
            void* parent = f.class_get_parent ? f.class_get_parent(k) : nullptr;
            const char* ns = f.class_get_namespace ? f.class_get_namespace(k) : "";
            fprintf(out, "\nclass %s%s%s : %s\n", ns && *ns ? ns : "", ns && *ns ? "." : "",
                    f.class_get_name(k), parent ? f.class_get_name(parent) : "-");
            void* it = nullptr;
            while (void* fld = f.class_get_fields(k, &it)) {
                void* t = f.field_get_type ? f.field_get_type(fld) : nullptr;
                const char* tn = (t && f.type_get_name) ? f.type_get_name(t) : "?";
                int flags = f.field_get_flags ? f.field_get_flags(fld) : 0;
                fprintf(out, "  %s%s %s; // 0x%x\n", (flags & 0x10) ? "static " : "", tn ? tn : "?",
                        f.field_get_name ? f.field_get_name(fld) : "?", f.field_get_offset(fld));
            }
            it = nullptr;
            while (void* m = f.class_get_methods(k, &it)) {
                void* mp = MethodPointer(m);
                uint32_t pc = f.method_get_param_count ? f.method_get_param_count(m) : 0;
                fprintf(out, "  %s %s(", ReturnTypeName(m).c_str(), f.method_get_name ? f.method_get_name(m) : "?");
                for (uint32_t p = 0; p < pc; ++p) {
                    const char* pn = f.method_get_param_name ? f.method_get_param_name(m, p) : "";
                    fprintf(out, "%s%s %s", p ? ", " : "", ParamTypeName(m, p).c_str(), pn ? pn : "");
                }
                if (mp && lib.contains(mp))
                    fprintf(out, "); // RVA 0x%zx\n", reinterpret_cast<uintptr_t>(mp) - lib.base);
                else
                    fprintf(out, "); // RVA -\n");
            }
        }
    }
    fclose(out);
    Log("dump: wrote %zu classes to %s", classes, path);
}

}  // namespace il2
