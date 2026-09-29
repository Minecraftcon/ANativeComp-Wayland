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

# Wait for system_server / ActivityManager to complete boot
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 2
done

# Start companion monitor foreground service
sleep 1
am start-foreground-service com.andwayland.companion/.CompositorMonitorService >> "$LOG" 2>&1 || true

