#!/bin/bash

# === Hikari Release Launcher ===

unset DISPLAY
unset XAUTHORITY
unset WAYLAND_DISPLAY
export WLR_BACKENDS=drm,libinput

if [ -z "$XDG_RUNTIME_DIR" ]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi

exec hikari -c ~/.config/hikari.conf > /dev/null 2>&1
