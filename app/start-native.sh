#!/bin/sh
set -eu
unset LD_PRELOAD LD_AUDIT LD_LIBRARY_PATH
umask 077
APP=/media/developer/apps/usr/palm/applications/org.webosbrew.brave.b9
STATE=/var/lib/webosbrew/brave-b9
CLIENT=${1-}
[ "$(id -u)" = 0 ] || { echo 'The Homebrew root service is required.' >&2; exit 1; }
case "$CLIENT" in
    ''|0|*[!0-9]*) echo 'Invalid native launcher PID.' >&2; exit 1 ;;
esac
tr '\000' '\n' < "/proc/$CLIENT/cmdline" | grep -Fx "$APP/launch.sh" >/dev/null ||
    { echo 'The requesting native launcher is not running.' >&2; exit 1; }
if [ ! -e "$STATE" ]; then
    mkdir -m 700 "$STATE"
fi
[ -d "$STATE" ] && [ ! -L "$STATE" ] &&
    [ "$(stat -c %u "$STATE")" = 0 ] &&
    [ "$(stat -c %a "$STATE")" = 700 ] ||
    { echo 'Unexpected native launcher state-directory permissions.' >&2; exit 1; }
exec 9>"$STATE/lock"
if ! flock -w 8 9; then
    echo 'The previous native browser session is still closing.' >&2
    exit 1
fi
kill -0 "$CLIENT"
LOG="$STATE/session-$(date +%s)-$$.log"
(
    APPID=org.webosbrew.brave.b9 "$APP/launch.sh" 9>&- &
    HOST=$!
    while [ -r "/proc/$HOST/stat" ] && kill -0 "$CLIENT" 2>/dev/null; do
        STATUS=$(awk '{print $3}' "/proc/$HOST/stat")
        [ "$STATUS" != Z ] || break
        sleep 2
    done
    if [ -r "/proc/$HOST/stat" ]; then
        STATUS=$(awk '{print $3}' "/proc/$HOST/stat")
        if [ "$STATUS" != Z ]; then
            kill -TERM "$HOST"
        fi
    fi
    RESULT=0
    wait "$HOST" || RESULT=$?
    echo "Native browser exit status: $RESULT"
    if [ -r "/proc/$CLIENT/cmdline" ] &&
        tr '\000' '\n' < "/proc/$CLIENT/cmdline" |
        grep -Fx '{"id":"org.webosbrew.brave.b9"}' >/dev/null; then
        REQUEST=$(printf '{"processId":"%s"}' "$CLIENT")
        if ! luna-send -n 1 -w 5000 luna://com.webos.applicationManager/close "$REQUEST"; then
            echo 'Could not close the native app registration.' >&2
        fi
    fi
    UPDATER="$APP/launch.sh"
    if [ -f "$APP/launch-engine.sh" ]; then
        UPDATER="$APP/launch-engine.sh"
    fi
    (
        exec 9>&-
        exec 8>"$STATE/adblock-update.lock"
        if ! flock -n 8; then
            echo 'An ad-block download is already running.'
            exit 0
        fi
        UPDATE_RESULT=0
        "$UPDATER" --fetch-adblock || UPDATE_RESULT=$?
        echo "Post-close ad-block download status: $UPDATE_RESULT"
    ) >> "$STATE/adblock-updates.log" 2>&1 < /dev/null &
) > "$LOG" 2>&1 < /dev/null &
echo "BRAVE_B9_STARTED supervisor=$! log=$LOG"
