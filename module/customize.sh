#!/system/bin/sh
SKIPUNZIP=0
[ "$ARCH" = "arm64" ] || abort "! arm64 only"
[ "$ZYGISK_ENABLED" = "true" ] || ui_print "! Enable Zygisk in Magisk settings, then reboot"
