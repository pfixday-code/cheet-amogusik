LOCAL_PATH := $(call my-dir)/..
include $(CLEAR_VARS)

LOCAL_MODULE := au_zygisk
LOCAL_SRC_FILES := \
    src/main.cpp src/log.cpp src/elf.cpp src/il2cpp.cpp src/features.cpp \
    src/java_menu.cpp third_party/And64.cpp

LOCAL_C_INCLUDES := $(LOCAL_PATH)/third_party
LOCAL_CPPFLAGS := -std=c++20 -fno-exceptions -fno-rtti -fvisibility=hidden -O2 -Wall
LOCAL_CFLAGS := -fvisibility=hidden -O2
LOCAL_LDLIBS := -llog -ldl
LOCAL_LDFLAGS := -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,--version-script=$(LOCAL_PATH)/jni/exports.map

include $(BUILD_SHARED_LIBRARY)
