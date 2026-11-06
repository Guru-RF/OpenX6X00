#!/bin/sh
# Inside the container: run the GUI on a virtual display, optionally press keys, save a PNG.
#   host/docker/shot.sh out.png [seconds] [steps...]
# A step is an xdotool key name (the PC's keys, host/README.md), sleep:<s>, or click:<x>,<y> (a touch
# on the 800x480 screen)
OUT=${1:-/src/build-docker/shot.png}; WAIT=${2:-6}
# not "shift 2" with fewer: a special built-in's error, which ends the script in dash (Debian's sh)
if [ $# -ge 2 ]; then shift 2; else shift $#; fi
Xvfb :99 -screen 0 1024x600x24 >/dev/null 2>&1 &
export DISPLAY=:99 SDL_AUDIODRIVER=dummy
pulseaudio -D --exit-idle-time=-1 --system=false >/dev/null 2>&1; pactl load-module module-null-sink >/dev/null 2>&1
sleep 1
stdbuf -oL -eL host/build.sh run > /tmp/gui.log 2>&1 &
sleep "$WAIT"
for k in "$@"; do
  case $k in
    sleep:*) sleep ${k#sleep:} ;;
    click:*) W=$(xdotool search --onlyvisible --class . | head -1); xy=${k#click:}
             xdotool mousemove --window "$W" "${xy%,*}" "${xy#*,}" mousedown 1 sleep 0.15 mouseup 1
             sleep 0.4 ;;
    *) xdotool search --sync --onlyvisible --class . windowactivate >/dev/null 2>&1
       xdotool key "$k"; sleep 0.4 ;;
  esac
done
sleep 1
W=$(xdotool search --onlyvisible --class . | head -1)
import -window "${W:-root}" "$OUT"
pkill -f x6100_gui 2>/dev/null; pkill Xvfb 2>/dev/null; sleep 0.5; tail -5 /tmp/gui.log
