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
BIN="/system/bin/andwayland"
[ ! -x "$BIN" ] && BIN="$MODDIR/system/bin/andwayland"

# Start the compositor
export XDG_RUNTIME_DIR=/data/wayland
export WAYLAND_DISPLAY=wayland-0
export LD_LIBRARY_PATH=/system/lib64:$MODDIR/system/lib64

# Clean stale socket & status if left from an unclean shutdown
rm -f /data/wayland/wayland-0* /data/wayland/status.json

nohup "$BIN" --socket wayland-0 >> "$LOG" 2>&1 < /dev/null &
PID=$!
echo "ANativeDrawer started (pid $PID)" >> "$LOG"

# ─── Rootless Xwayland (via xwayland-satellite) ───────────────────────────────
# Wait for the Wayland socket to actually appear before launching the bridge.
# xwayland-satellite connects as a Wayland client; it must not race the compositor.
XWLOG="/data/wayland/xwayland-satellite.log"
XWBIN="/system/bin/xwayland-satellite"
[ ! -x "$XWBIN" ] && XWBIN="$MODDIR/system/bin/xwayland-satellite"

if [ -x "$XWBIN" ]; then
    SOCKET_WAIT=0
    while [ ! -S "/data/wayland/wayland-0" ] && [ $SOCKET_WAIT -lt 30 ]; do
        sleep 1
        SOCKET_WAIT=$((SOCKET_WAIT + 1))
    done

    if [ -S "/data/wayland/wayland-0" ]; then
        # DISPLAY=:0  — xwayland-satellite owns this X display
        # Xwayland is spawned on-demand when the first X11 client connects.
        export DISPLAY=:0
        echo ":0" > /data/wayland/.xdisplay
        nohup "$XWBIN" >> "$XWLOG" 2>&1 < /dev/null &
        XWPID=$!
        echo "xwayland-satellite started (pid $XWPID, DISPLAY=:0)" >> "$LOG"
    else
        echo "xwayland-satellite skipped — Wayland socket not ready after 30s" >> "$LOG"
    fi
else
    echo "xwayland-satellite not found, skipping" >> "$LOG"
fi
# ─────────────────────────────────────────────────────────────────────────────

# Wait for system_server / ActivityManager to complete boot
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 2
done

# Ensure Magisk SU grant toasts are silenced for companion app
COMPANION_UID=$(pm list packages -U 2>/dev/null | grep com.andwayland.companion | sed 's/.*uid://' | head -n 1)
if [ -n "$COMPANION_UID" ]; then
    magisk --sqlite "INSERT OR REPLACE INTO policies (uid, policy, until, logging, notification) VALUES ($COMPANION_UID, 2, 0, 1, 0);" 2>/dev/null || true
fi

# Start companion monitor foreground service
sleep 1
am start-foreground-service com.andwayland.companion/.CompositorMonitorService >> "$LOG" 2>&1 || true

