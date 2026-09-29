SKIPUNZIP=0

ui_print "****************************************"
ui_print "     ANativeDrawer Wayland Compositor   "
ui_print "****************************************"
ui_print "- Installing binaries and libraries..."

# Set permissions
set_perm "$MODPATH/system/bin/andwayland" 0 0 0755
if [ -f "$MODPATH/system/lib64/libwayland-server.so" ]; then
    set_perm "$MODPATH/system/lib64/libwayland-server.so" 0 0 0644
fi
if [ -f "$MODPATH/system/lib64/libc++_shared.so" ]; then
    set_perm "$MODPATH/system/lib64/libc++_shared.so" 0 0 0644
fi
set_perm "$MODPATH/service.sh" 0 0 0755

ui_print "- Setting up companion app..."
set_perm_recursive "$MODPATH/system/priv-app/ANativeDrawer" 0 0 0755 0644

ui_print "- Creating runtime directory..."
mkdir -p /data/wayland
chmod 777 /data/wayland

ui_print "- Installation complete! Reboot to activate."
