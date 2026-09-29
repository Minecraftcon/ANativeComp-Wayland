SKIPUNZIP=0

ui_print "****************************************"
ui_print "     ANativeDrawer Wayland Compositor   "
ui_print "****************************************"
ui_print "- Installing binaries and libraries..."

# Set permissions
set_perm "$MODPATH/system/bin/andwayland" 0 0 0755
if [ -f "$MODPATH/system/bin/xwayland-satellite" ]; then
    set_perm "$MODPATH/system/bin/xwayland-satellite" 0 0 0755
fi
if [ -f "$MODPATH/system/bin/start-xwayland" ]; then
    set_perm "$MODPATH/system/bin/start-xwayland" 0 0 0755
fi
if [ -f "$MODPATH/system/lib64/libwayland-server.so" ]; then
    set_perm "$MODPATH/system/lib64/libwayland-server.so" 0 0 0644
fi
if [ -f "$MODPATH/system/lib64/libc++_shared.so" ]; then
    set_perm "$MODPATH/system/lib64/libc++_shared.so" 0 0 0644
fi
set_perm "$MODPATH/service.sh" 0 0 0755

# Setup Termux integration if Termux is present
TERMUX_BIN="/data/data/com.termux/files/usr/bin"
if [ -d "$TERMUX_BIN" ]; then
    ui_print "- Integrating rootless Xwayland with Termux..."
    TERMUX_UID=$(stat -c '%u' "$TERMUX_BIN" 2>/dev/null || echo 1000)
    TERMUX_GID=$(stat -c '%g' "$TERMUX_BIN" 2>/dev/null || echo 1000)
    if [ -f "$MODPATH/system/bin/xwayland-satellite" ]; then
        cp -f "$MODPATH/system/bin/xwayland-satellite" "$TERMUX_BIN/xwayland-satellite"
        chown $TERMUX_UID:$TERMUX_GID "$TERMUX_BIN/xwayland-satellite"
        chmod 755 "$TERMUX_BIN/xwayland-satellite"
    fi
    if [ -f "$MODPATH/system/bin/start-xwayland" ]; then
        cp -f "$MODPATH/system/bin/start-xwayland" "$TERMUX_BIN/start-xwayland"
        chown $TERMUX_UID:$TERMUX_GID "$TERMUX_BIN/start-xwayland"
        chmod 755 "$TERMUX_BIN/start-xwayland"
    fi
fi

ui_print "- Setting up companion app..."
set_perm_recursive "$MODPATH/system/priv-app/ANativeDrawer" 0 0 0755 0644

ui_print "- Creating runtime directory..."
mkdir -p /data/wayland
chmod 777 /data/wayland

# Silence Magisk SU grant toasts for companion app to prevent popup notification spam
COMPANION_UID=$(pm list packages -U 2>/dev/null | grep com.andwayland.companion | sed 's/.*uid://' | head -n 1)
if [ -n "$COMPANION_UID" ]; then
    magisk --sqlite "INSERT OR REPLACE INTO policies (uid, policy, until, logging, notification) VALUES ($COMPANION_UID, 2, 0, 1, 0);" 2>/dev/null || true
fi

ui_print "- Installation complete! Reboot to activate."
