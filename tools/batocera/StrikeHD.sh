#!/bin/bash
# Batocera: copy to /userdata/roms/ports/ to start the game from Ports.
# picom (Batocera's compositor) copies the game's picture on its own clock and,
# with NVIDIA, shows a torn band in fullscreen: it is stopped while the game
# runs and always started again afterwards (also if the game is killed).
cd /userdata/system/strikehd || exit 1
mkdir -p /userdata/system/logs
PICOM_ARGS=$(ps -o args | grep '^picom ' | head -1 | cut -d' ' -f2-)
restore() {
    if [ -n "$PICOM_ARGS" ] && ! pidof picom > /dev/null; then
        picom $PICOM_ARGS
    fi
}
trap restore EXIT
trap 'kill $GAME 2> /dev/null; exit 1' INT TERM HUP
[ -n "$PICOM_ARGS" ] && killall picom 2> /dev/null
./StrikeCollectionHD > /userdata/system/logs/strikehd.log 2>&1 &
GAME=$!
wait $GAME
