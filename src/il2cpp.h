#pragma once
#include "elf.h"
#include <string>

namespace il2 {

extern LoadedModule lib;

// Resolves the exports. Returns false (and logs which ones) if a required one is missing.
bool Load();
// Blocks until il2cpp_init has finished. Nothing else may be called before this returns true.
bool WaitRuntime(int timeoutSec);
void* Domain();
void AttachThread();

// Waits until the runtime is initialised and Assembly-CSharp is visible.
bool WaitReady(int timeoutSec);
// Cheap check (no class initialisation): is an assembly with this image name registered?
bool HasImage(const char* imageName);
bool FieldIsStatic(void* field);

void* FindClass(const char* ns, const char* name);                 // all images
void* FindClassIn(const char* imageName, const char* ns, const char* name);
void* Field(void* klass, const char* name);
int FieldOffset(void* field);
bool StaticGet(void* field, void* out);
void* Method(void* klass, const char* name, int argc);
inline void* MethodPointer(void* method) { return method ? *reinterpret_cast<void**>(method) : nullptr; }
std::string ReturnTypeName(void* method);
std::string ParamTypeName(void* method, int index);
bool ParamIsReference(void* method, int index);  // class / object / string / array

void Dump(const char* path);

}  // namespace il2
