#!/bin/sh

# Launch tinywl naturally. 
# We use the '-s' startup flag to run a custom mini-script INSIDE tinywl.
exec ./tinywl -s "
  # Inside here, tinywl has already loaded wayland-1!
  # We update D-Bus right now so applications spawned next use it.
  dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_CURRENT_DESKTOP=tinywl
  
  # Launch foot terminal trapped cleanly inside
  foot
"

