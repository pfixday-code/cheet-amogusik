#pragma once
#include "common.h"
#include "And64.h"

// Installs an inline hook.
// And64InlineHook (Rprop) writes the trampoline pointer into *result before it patches the target,
// and builds the trampoline before the patch goes live. Passing `original` straight through means
// the hook can never run while *original still points at the freshly patched target (which would
// recurse into the hook). On failure And64 stores NULL; the previous value is put back so a caller
// that was already forwarding through *original keeps working.
// Caveat: between And64 storing *result and finishing the trampoline, *original points at an
// unfinished trampoline. Callers must not hook a function whose *original is being called
// concurrently through another path (see InstallInlineFallback in overlay.cpp).
template <typename Fn>
bool InstallHook(const char* what, void* target, Fn replacement, Fn* original) {
    if (!target) {
        Log("hook %s: NOT hooked (no target)", what);
        return false;
    }
    const Fn previous = *original;
    A64HookFunction(target, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(original));
    if (!*original) {
        *original = previous;
        Log("hook %s: NOT hooked (trampoline failed)", what);
        return false;
    }
    Log("hook %s: OK at %p", what, target);
    return true;
}
