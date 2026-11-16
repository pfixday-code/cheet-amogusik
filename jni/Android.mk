LOCAL_PATH := $(call my-dir)/..
include $(CLEAR_VARS)

LOCAL_MODULE := au_zygisk
LOCAL_SRC_FILES := \
    src/main.cpp src/log.cpp src/elf.cpp src/il2cpp.cpp src/features.cpp \
    src/overlay.cpp src/input.cpp src/java_menu.cpp \
    third_party/And64.cpp \
    third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp \
    third_party/imgui/backends/imgui_impl_opengl3.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)/third_party $(LOCAL_PATH)/third_party/imgui \
    $(LOCAL_PATH)/third_party/imgui/backends
LOCAL_CPPFLAGS := -std=c++20 -fno-exceptions -fno-rtti -fvisibility=hidden -O2 -Wall \
    -DIMGUI_IMPL_OPENGL_ES3 -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEBUG_TOOLS
LOCAL_CFLAGS := -fvisibility=hidden -O2
LOCAL_LDLIBS := -llog -lEGL -lGLESv3 -ldl
LOCAL_LDFLAGS := -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--version-script=$(LOCAL_PATH)/jni/exports.map

include $(BUILD_SHARED_LIBRARY)
