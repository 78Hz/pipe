#!/usr/bin/env bash
# Opens the built CLAP's interface on a virtual display and saves one picture of each screen
# into docs/manual/img. Needs Xvfb, xdotool and ImageMagick (Linux).
#   docs/manual/screenshots.sh build/bin/Pipe.clap
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
shot() { xdotool mousemove "$1" 40 click 1; sleep 1.2; import -window root -crop 1280x800+0+0 +repage "$OUT/$2.png"; echo "  $2.png"; }
# tab centres along the header (see drawHeader in plugin/PipeUI.cpp)
shot 170 console; shot 268 expression; shot 353 wind; shot 428 voicing
shot 509 tuning;  shot 580 room;       shot 654 presets
kill $HOSTPID 2>/dev/null || true
