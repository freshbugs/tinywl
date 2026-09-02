#!/bin/sh

# Export standard D-Bus variable if not present, to appease Snaps
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"
fi

# export desktop variables
export LANG="en_US.UTF-8"
export XDG_CURRENT_DESKTOP=wlroots
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_SESSION_TYPE=wayland
export XDG_DATA_DIRS=/usr/local/share:/usr/share:/var/lib/flatpak/exports/share:/var/lib/snapd/desktop
export XLOCALEDIR="/usr/share/X11/locale"
export MOZ_ENABLE_WAYLAND=1
export GDK_BACKEND=wayland

exec /home/stephen/tinywl/tinywl -s "
  dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP=wlroots
  foot"
  >/home/stephen/tinywl/tinywl.log 2>&1
