#!/bin/sh

# Ensure XDG_RUNDIME_DIR is set (Wayland requirement)
if [ -z "$XDG_RUNTIME_DIR" ]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi

# Export standard D-Bus variable if not present, to appease Snaps
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"
fi

# desktop variables
export LANG="en_US.UTF-8"
export XDG_CURRENT_DESKTOP=wlroots
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_SESSION_TYPE=wayland
export XDG_DATA_DIRS=/usr/local/share:/usr/share:/var/lib/flatpak/exports/share:/var/lib/snapd/desktop
export XLOCALEDIR="/usr/share/X11/locale"

# toolkit backend variables
export MOZ_ENABLE_WAYLAND=1
export GDK_BACKEND=wayland
export QT_QPA_PLATFORM=wayland

# hardcoded location of tinywl binary
COMPOSITOR="/home/stephen/tinywl/tinywl"
AUTOSTART="/home/stephen/tinywl/autostart"
LOG="/home/stephen/tinywl/tinywl.log"

exec "$COMPOSITOR" -s "$AUTOSTART" > "$LOG"
