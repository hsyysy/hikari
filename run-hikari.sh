#!/bin/bash

# === Hikari Launcher ===

unset DISPLAY
unset XAUTHORITY
unset WAYLAND_DISPLAY
export WLR_BACKENDS=drm,libinput

if [ -z "$XDG_RUNTIME_DIR" ]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi

# --- fcitx5 environment ---
export XMODIFIERS=@im=fcitx
# Uncomment below if some apps don't pick up the input method:
# export GTK_IM_MODULE=fcitx
# export QT_IM_MODULE=fcitx

exec hikari -c ~/.config/hikari.conf
