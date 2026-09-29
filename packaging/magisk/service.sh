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

# Create runtime directory
mkdir -p /data/wayland
chmod 777 /data/wayland

# Set WAYLAND_DISPLAY so other processes can discover the socket
# (write it to a known location; client apps source this)
echo "wayland-0" > /data/wayland/.display

# Start the compositor
export XDG_RUNTIME_DIR=/data/wayland
export WAYLAND_DISPLAY=wayland-0
export LD_LIBRARY_PATH=/system/lib64

/system/bin/andwayland --socket wayland-0 >> "$LOG" 2>&1 &
PID=$!
echo "ANativeDrawer started (pid $PID)" >> "$LOG"

# Start companion monitor foreground service
sleep 2
am start-foreground-service com.andwayland.companion/.CompositorMonitorService >> "$LOG" 2>&1 || true

