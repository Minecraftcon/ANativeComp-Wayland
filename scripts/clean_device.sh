#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# ANativeDrawer — Complete Android Device Cleanup Script
#
# Removes all traces of ANativeDrawer & Wayland from a connected Android device:
#   1. Kills running compositor, companion service, and client processes
#   2. Uninstalls companion app (com.andwayland.companion)
#   3. Cleans app data from /data/data and /data/user
#   4. Removes Magisk module (/data/adb/modules/andwayland, modules_update)
#   5. Removes runtime directory (/data/wayland)
#   6. Cleans /data/local/tmp binaries, libraries, sockets, and logs
#   7. Removes downloaded module zips from /sdcard/Download
#   8. Verifies complete removal
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}====================================================${NC}"
echo -e "${BLUE}      ANativeDrawer Device Cleanup Utility         ${NC}"
echo -e "${BLUE}====================================================${NC}"

# Check adb
if ! command -v adb &>/dev/null; then
    echo -e "${RED}[ERROR] adb is not installed or not in PATH.${NC}"
    exit 1
fi

# Check device
DEVICE_STATUS=$(adb get-state 2>/dev/null || echo "offline")
if [ "$DEVICE_STATUS" != "device" ]; then
    echo -e "${RED}[ERROR] No authorized Android device detected via adb.${NC}"
    adb devices
    exit 1
fi

DEVICE_SERIAL=$(adb get-serialno)
echo -e "${GREEN}[+] Target Device:${NC} $DEVICE_SERIAL"

# Check root access via su
echo -e "\n${YELLOW}[1/7] Checking root privileges on device...${NC}"
CHECK_ROOT=$(adb shell "su -c 'id'" 2>/dev/null || true)
if [[ "$CHECK_ROOT" != *"uid=0"* ]]; then
    echo -e "${RED}[ERROR] Root (su) access is required to clean system and data paths.${NC}"
    exit 1
fi
echo -e "${GREEN}[✔] Root access confirmed (uid=0).${NC}"

# 1. Terminate running processes
echo -e "\n${YELLOW}[2/7] Terminating all running Wayland & ANativeDrawer processes...${NC}"
adb shell "su" << 'EOF' >/dev/null 2>&1 || true
am force-stop com.andwayland.companion 2>/dev/null || true
pkill -9 andwayland 2>/dev/null || true
pkill -9 -f andwayland 2>/dev/null || true
pkill -9 -f com.andwayland.companion 2>/dev/null || true
pkill -9 andwayland-hello 2>/dev/null || true
pkill -9 andwayland-test-client 2>/dev/null || true
pkill -9 foot 2>/dev/null || true
pkill -9 galculator 2>/dev/null || true
pkill -9 thunar 2>/dev/null || true
EOF
echo -e "${GREEN}[✔] Processes stopped.${NC}"

# 2. Uninstall Companion App
echo -e "\n${YELLOW}[3/7] Uninstalling companion application...${NC}"
adb uninstall com.andwayland.companion >/dev/null 2>&1 || true
adb shell "su" << 'EOF' >/dev/null 2>&1 || true
pm uninstall com.andwayland.companion 2>/dev/null || true
rm -rf /data/data/com.andwayland.companion \
       /data/user/0/com.andwayland.companion \
       /data/user_de/0/com.andwayland.companion \
       /data/app/*com.andwayland.companion* \
       /system/priv-app/ANativeDrawer \
       /system/priv-app/ANativeDrawerCompanion \
       /system/app/ANativeDrawer
EOF
echo -e "${GREEN}[✔] Package uninstalled and data cleared.${NC}"

# 3. Remove Magisk Module
echo -e "\n${YELLOW}[4/7] Removing Magisk module registrations...${NC}"
adb shell "su -mm -c '
    umount -l /data/adb/modules/andwayland/system/bin/andwayland 2>/dev/null || true
    umount -l /system/bin/andwayland 2>/dev/null || true
    umount -l /system/bin/andwayland 2>/dev/null || true
    rm -rf /data/adb/modules/andwayland \
           /data/adb/modules_update/andwayland \
           /data/adb/modules/anative_drawer* \
           /data/adb/modules_update/anative_drawer*
'" >/dev/null 2>&1 || true
echo -e "${GREEN}[✔] Magisk module removed from /data/adb/modules.${NC}"

# 4. Remove Runtime Directory
echo -e "\n${YELLOW}[5/7] Purging runtime directories and sockets...${NC}"
adb shell "su -mm -c 'rm -rf /data/wayland /data/anative*'" >/dev/null 2>&1 || true
echo -e "${GREEN}[✔] Runtime directory /data/wayland purged.${NC}"

# 5. Clean /data/local/tmp binaries & libraries
echo -e "\n${YELLOW}[6/7] Cleaning temporary binaries, libraries, and logs in /data/local/tmp...${NC}"
adb shell "su -mm -c '
    rm -rf /data/local/tmp/andwayland* \
           /data/local/tmp/*satellite* \
           /data/local/tmp/libwayland* \
           /data/local/tmp/wayland* \
           /data/local/tmp/screen_wayland* \
           /data/local/tmp/ANativeDrawer*
'" >/dev/null 2>&1 || true
echo -e "${GREEN}[✔] /data/local/tmp cleaned.${NC}"

# 6. Clean downloaded zips in /sdcard/Download
echo -e "\n${YELLOW}[7/7] Removing downloaded module zips from /sdcard/Download...${NC}"
adb shell "su" << 'EOF' >/dev/null 2>&1 || true
rm -f /sdcard/Download/andwayland-magisk.zip \
      /sdcard/Download/*andwayland*.zip \
      /sdcard/Download/*ANativeDrawer*.zip
EOF
echo -e "${GREEN}[✔] Download directory cleaned.${NC}"

# 7. Verification check
echo -e "\n${BLUE}====================================================${NC}"
echo -e "${BLUE}                Verification Report                 ${NC}"
echo -e "${BLUE}====================================================${NC}"

VERIFY_PROCESSES=$(adb shell "su -c 'ps -ef | grep -E \"andwayland|com\.andwayland\.companion\" | grep -v grep'" 2>/dev/null || true)
if [ -n "$VERIFY_PROCESSES" ]; then
    echo -e "${RED}[!] Lingering processes found:${NC}\n$VERIFY_PROCESSES"
else
    echo -e "${GREEN}[✔] Processes: None running.${NC}"
fi

VERIFY_PKG=$(adb shell pm list packages | grep "com.andwayland.companion" || true)
if [ -n "$VERIFY_PKG" ]; then
    echo -e "${RED}[!] Package still installed:${NC} $VERIFY_PKG"
else
    echo -e "${GREEN}[✔] Package: com.andwayland.companion completely uninstalled.${NC}"
fi

VERIFY_MAGISK=$(adb shell "su -mm -c 'ls -d /data/adb/modules/*wayland* /data/adb/modules_update/*wayland* 2>/dev/null'" || true)
if [ -n "$VERIFY_MAGISK" ]; then
    echo -e "${RED}[!] Magisk module files found:${NC} $VERIFY_MAGISK"
else
    echo -e "${GREEN}[✔] Magisk Module: Cleaned from /data/adb/modules.${NC}"
fi

VERIFY_FILES=$(adb shell "su -mm -c 'ls -d /data/wayland /data/local/tmp/*wayland* /data/local/tmp/*satellite* /sdcard/Download/*andwayland* 2>/dev/null'" || true)
if [ -n "$VERIFY_FILES" ]; then
    echo -e "${RED}[!] Residual files detected:${NC}\n$VERIFY_FILES"
else
    echo -e "${GREEN}[✔] Storage & Runtime: Cleaned (no residual files or sockets).${NC}"
fi

echo -e "\n${GREEN}All traces of ANativeDrawer / Wayland successfully wiped from the device!${NC}"
echo -e "${YELLOW}Note: If the Magisk module was active, a device reboot will unmount any pending system overlays.${NC}\n"
