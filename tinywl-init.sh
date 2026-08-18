#!/bin/sh
# This script runs INSIDE the tinywl session right after the display socket initializes

# Update dbus now that WAYLAND_DISPLAY actually exists in the environment
dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP

# Launch your terminal emulator client
exec foot

