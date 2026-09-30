#!/system/bin/sh
# ─────────────────────────────────────────────────────────────────────────────
# ANativeDrawer Magisk Module — service.sh
#
# Called by Magisk after boot. Starts andwayland as a background service.
# ─────────────────────────────────────────────────────────────────────────────

MODDIR="${0%/*}"
LOG="/data/wayland/andwayland.log"

# Wait for SurfaceFlinger to be ready
while ! service list 2>/dev/null | grep -q SurfaceFlinger; do
    sleep 1
done

# Create runtime directory and compatibility symlink for clients
mkdir -p /data/wayland
chmod 777 /data/wayland
ln -sf /data/wayland /data/local/tmp/wayland

# Set WAYLAND_DISPLAY so other processes can discover the socket
# (write it to a known location; client apps source this)
echo "wayland-0" > /data/wayland/.display

# Determine binary location (overlay or module directory)
BIN="$MODDIR/system/bin/andwayland"
if [ -s "/system/bin/andwayland" ] && [ -x "/system/bin/andwayland" ]; then
    BIN="/system/bin/andwayland"
fi

# Start the compositor
export XDG_RUNTIME_DIR=/data/wayland
export WAYLAND_DISPLAY=wayland-0
export LD_LIBRARY_PATH=$MODDIR/system/lib64:/system/lib64

# Clean stale sockets, locks, and status from any previous unclean shutdown
rm -f /data/wayland/wayland-0* /data/wayland/status.json /data/wayland/xwls-* /data/wayland/.xdisplay
rm -f /tmp/.X11-unix/X* 2>/dev/null || true
TERMUX_PREFIX="/data/data/com.termux/files/usr"
rm -f "$TERMUX_PREFIX/tmp/.X11-unix/X*" 2>/dev/null || true

nohup "$BIN" --socket wayland-0 >> "$LOG" 2>&1 < /dev/null &
PID=$!
echo "ANativeDrawer started (pid $PID)" >> "$LOG"

# Wait for system_server / ActivityManager to complete boot
# (Ensures user storage /data/data/com.termux is unlocked and ready)
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 2
done

# ─── Rootless Xwayland Autostart (via xwayland-satellite) ──────────────────────
# Wait for the Wayland socket to appear before launching the bridge.
SOCKET_WAIT=0
while [ ! -S "/data/wayland/wayland-0" ] && [ $SOCKET_WAIT -lt 30 ]; do
    sleep 1
    SOCKET_WAIT=$((SOCKET_WAIT + 1))
done

# Ensure socket & runtime directory are accessible to all clients
chmod 777 /data/wayland
chmod 777 /data/wayland/wayland-0* 2>/dev/null || true

# Note: andwayland automatically starts and supervises xwayland-satellite :1.
# Fallback check after 5 seconds:
sleep 5
TERMUX_DIR="/data/data/com.termux/files"
TERMUX_UID=$(stat -c %u "$TERMUX_DIR" 2>/dev/null)
[ -z "$TERMUX_UID" ] && TERMUX_UID=$(pm list packages -U com.termux 2>/dev/null | grep -E '^package:com\.termux ' | sed -n 's/.*uid:\([0-9]*\).*/\1/p')

if ! pidof xwayland-satellite >/dev/null 2>&1; then
    SATELLITE_BIN="$TERMUX_PREFIX/bin/xwayland-satellite"
    [ ! -x "$SATELLITE_BIN" ] && SATELLITE_BIN="/system/bin/xwayland-satellite"
    [ ! -x "$SATELLITE_BIN" ] && SATELLITE_BIN="$MODDIR/system/bin/xwayland-satellite"

    if [ -x "$SATELLITE_BIN" ] && [ -n "$TERMUX_UID" ]; then
        echo ":1" > /data/wayland/.xdisplay
        chmod 666 /data/wayland/.xdisplay
        cat << EOF > /data/wayland/run_satellite.sh
#!/system/bin/sh
export PATH="$TERMUX_PREFIX/bin:\$PATH"
export LD_LIBRARY_PATH="$TERMUX_PREFIX/lib:/system/lib64:\$LD_LIBRARY_PATH"
export XDG_RUNTIME_DIR=/data/wayland
export WAYLAND_DISPLAY=wayland-0
export DISPLAY=:1
nohup $SATELLITE_BIN :1 > $TERMUX_DIR/home/satellite.log 2>&1 &
EOF
        chmod 755 /data/wayland/run_satellite.sh
        chown $TERMUX_UID:$TERMUX_UID /data/wayland/run_satellite.sh
        su $TERMUX_UID -c /data/wayland/run_satellite.sh
        echo "xwayland-satellite fallback started for DISPLAY=:1 (UID $TERMUX_UID)" >> "$LOG"
    fi
fi

# ─── Virtual Keyboard Autostart (hidden & auto-activated via IME) ─────────────
WVKBD_BIN="$TERMUX_PREFIX/bin/wvkbd-mobintl"
if [ -x "$WVKBD_BIN" ] && [ -n "$TERMUX_UID" ]; then
    # Generate GTK3 immodules cache if missing so GTK automatically uses Wayland IM
    if [ -x "$TERMUX_PREFIX/bin/gtk-query-immodules-3.0" ] && [ ! -f "$TERMUX_PREFIX/lib/gtk-3.0/3.0.0/immodules.cache" ]; then
        su $TERMUX_UID -c "$TERMUX_PREFIX/bin/gtk-query-immodules-3.0 > $TERMUX_PREFIX/lib/gtk-3.0/3.0.0/immodules.cache" 2>/dev/null || true
    fi

    cat << EOF > /data/wayland/run_wvkbd.sh
#!/system/bin/sh
export PATH="$TERMUX_PREFIX/bin:\$PATH"
export LD_LIBRARY_PATH="$TERMUX_PREFIX/lib:$MODDIR/system/lib64:\$LD_LIBRARY_PATH"
export XDG_RUNTIME_DIR=/data/wayland
export WAYLAND_DISPLAY=wayland-0
exec $WVKBD_BIN --hidden --auto -H 350
EOF
    chmod 755 /data/wayland/run_wvkbd.sh
    chown $TERMUX_UID:$TERMUX_UID /data/wayland/run_wvkbd.sh
    pkill -f wvkbd-mobintl 2>/dev/null || true
    su $TERMUX_UID -c /data/wayland/run_wvkbd.sh >/dev/null 2>&1 &
    echo "wvkbd-mobintl started in background (--hidden --auto) (UID $TERMUX_UID)" >> "$LOG"
fi
# ─────────────────────────────────────────────────────────────────────────────

# Ensure Magisk SU grant toasts are silenced for companion app
COMPANION_UID=$(pm list packages -U 2>/dev/null | grep com.andwayland.companion | sed 's/.*uid://' | head -n 1)
if [ -n "$COMPANION_UID" ]; then
    magisk --sqlite "INSERT OR REPLACE INTO policies (uid, policy, until, logging, notification) VALUES ($COMPANION_UID, 2, 0, 1, 0);" 2>/dev/null || true
fi

# Start companion monitor foreground service
sleep 1
am start-foreground-service com.andwayland.companion/.CompositorMonitorService >> "$LOG" 2>&1 || true

