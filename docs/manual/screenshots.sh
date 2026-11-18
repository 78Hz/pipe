#!/usr/bin/env bash
# Opens the built CLAP's interface on a virtual display and saves one picture of each screen
# into docs/manual/img. Needs Xvfb, xdotool and ImageMagick (Linux).
#   docs/manual/screenshots.sh build/bin/OpusXIV.clap
set -euo pipefail
CLAP="$(realpath "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
OUT="$HERE/img"
g++ -O1 -std=c++17 -I"$ROOT/dpf/distrho/src/clap" "$ROOT/tests/clap_gui_host.cpp" -o /tmp/pipe_gui_host -ldl -lX11 -lpthread
Xvfb :97 -screen 0 1400x900x24 >/dev/null 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null || true' EXIT
export DISPLAY=:97
sleep 2
/tmp/pipe_gui_host "$CLAP" 90 &
HOSTPID=$!
sleep 5
shot() { xdotool mousemove "$1" "${3:-773}" click 1; sleep 1.2; import -window root -crop 1280x800+0+0 +repage "$OUT/$2.png"; echo "  $2.png"; }
# the console first (a click on an empty part of the header), then each drawer tab along the foot
# (see drawDrawerTabs in plugin/PipeUI.cpp); a tab clicked while another drawer is open switches to it
shot 700 console 40
shot ${TAB_WIND:-150} wind; shot ${TAB_TUNING:-240} tuning; shot ${TAB_ROOM:-325} room
shot ${TAB_EXPR:-430} expression; shot ${TAB_VOICING:-545} voicing; shot ${TAB_PRESETS:-650} presets
kill $HOSTPID 2>/dev/null || true
