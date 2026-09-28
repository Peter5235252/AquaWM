#!/bin/sh
# run-tty: launch aquawm on real hardware from a TTY.
#
# Log out to a TTY (Ctrl+Alt+F3), log in as yourself (not root), and run
# ./run-tty.sh from your checkout. Unsetting WAYLAND_DISPLAY keeps backend
# autocreate from nesting into another session, so it takes the display
# via DRM/KMS instead. A normal TTY login provides the logind session
# compositors need for input and DRM access.
#
# Usage: ./run-tty.sh [-- aquawm args...]   (args are passed through)

unset WAYLAND_DISPLAY

BIN="$(dirname "$0")/result/bin/aquawm"
if [ ! -x "$BIN" ]; then
    BIN="$(dirname "$0")/build/aquawm"
fi
if [ ! -x "$BIN" ]; then
    echo "run-tty: $BIN not found; build first:" >&2
    echo "  cmake -S . -B build -G Ninja && cmake --build build" >&2
    echo "  (or: nix build, for ./result/bin/aquawm)" >&2
    exit 1
fi

exec "$BIN" "$@"
