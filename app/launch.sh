#!/bin/sh
set -eu
unset LD_PRELOAD LD_AUDIT LD_LIBRARY_PATH
umask 077
APP=/media/developer/apps/usr/palm/applications/org.webosbrew.brave.b9
DATA=/media/developer/brave-b9-data
BROWSER="$APP/engine/brave"
CACHE=/media/developer/brave-b9-hot
UID_BROWSER=$(id -u nobody)
GID_BROWSER=$(id -g nobody)
if [ "$(id -u)" != 0 ]; then
    echo 'The native display launcher requires the normal webOS native-app identity.' >&2
    exit 1
fi
if [ "$DATA" = /media/developer/brave-b9-usb/data ] &&
   { [ -e "$CACHE" ] || [ -L "$CACHE" ]; }; then
    for directory in "$CACHE" "$CACHE/engine"; do
        [ -d "$directory" ] && [ ! -L "$directory" ] &&
            [ "$(stat -c %u "$directory")" = 0 ] &&
            [ "$(stat -c %a "$directory")" = 755 ] ||
            { echo 'Internal browser copy has unsafe directory permissions.' >&2; exit 1; }
    done
    for file in "$CACHE/engine/brave" "$CACHE/source-brave.sha256"; do
        [ -f "$file" ] && [ ! -L "$file" ] &&
            [ "$(stat -c %u "$file")" = 0 ] ||
            { echo 'Internal browser copy is incomplete or unsafe.' >&2; exit 1; }
    done
    [ "$(stat -c %a "$CACHE/engine/brave")" = 755 ] &&
        [ "$(stat -c %a "$CACHE/source-brave.sha256")" = 644 ] ||
        { echo 'Internal browser copy has unsafe file permissions.' >&2; exit 1; }
    read -r CACHED_SOURCE < "$CACHE/source-brave.sha256"
    CURRENT_SOURCE=$(awk '$2 == "engine/brave" {print $1}' "$APP/engine/../SHA256SUMS")
    [ "${#CACHED_SOURCE}" = 64 ] && [ "${#CURRENT_SOURCE}" = 64 ] ||
        { echo 'Internal browser build identity is invalid.' >&2; exit 1; }
    if [ "$CACHED_SOURCE" = "$CURRENT_SOURCE" ]; then
        BROWSER="$CACHE/engine/brave"
        echo 'Starting internal browser executable; profile and resources stay on USB.' >&2
    else
        echo 'Internal browser copy is from an older build; using the USB executable.' >&2
    fi
fi
if [ ! -d "$DATA" ]; then
    mkdir -m 700 "$DATA"
    chown "$UID_BROWSER:$GID_BROWSER" "$DATA"
fi
if [ "$(stat -c %u "$DATA")" != "$UID_BROWSER" ] ||
   [ "$(stat -c %a "$DATA")" != 700 ]; then
    echo 'Browser data directory has unexpected ownership or permissions.' >&2
    exit 1
fi
if [ -f "$APP/runtime/ca-certificates.crt" ]; then
    export CURL_CA_BUNDLE="$APP/runtime/ca-certificates.crt"
fi
case "${1-}" in
    --fetch-adblock)
        exec nice -n 10 "$APP/browser-exec" /usr/bin/node \
            "$APP/update-adblock.js" fetch "$DATA/profile" "$DATA/adblock"
        ;;
    '') ;;
    *) echo 'Unknown browser maintenance mode.' >&2; exit 1 ;;
esac
if [ -f "$APP/initialize-profile.js" ]; then
    "$APP/browser-exec" /usr/bin/node "$APP/initialize-profile.js" \
        "$DATA/profile" "$APP/defaults/profile"
fi
if ! "$APP/browser-exec" /usr/bin/node "$APP/update-adblock.js" \
    apply "$DATA/profile" "$DATA/adblock"; then
    echo 'Ad-block update failed; retaining the previously installed filter data.' >&2
    if ! luna-send -n 1 -w 2000 luna://com.webos.notification/createToast \
        '{"sourceId":"org.webosbrew.brave.b9","message":"Brave could not apply its ad-block update. Previous filters are retained."}' >&2; then
        echo 'Could not display the ad-block update error notification.' >&2
    fi
fi
RUN=$(mktemp -d /dev/shm/brave-b9-XXXXXX)
chgrp "$GID_BROWSER" "$RUN"
chmod 710 "$RUN"
mkdir -m 700 "$RUN/frames" "$RUN/user"
chown "$UID_BROWSER:$GID_BROWSER" "$RUN/frames" "$RUN/user"
export HOME="$DATA"
export XDG_CONFIG_HOME="$DATA/config"
export XDG_CACHE_HOME="$DATA/cache"
export XDG_RUNTIME_DIR=/tmp/xdg
export BRAVE_WEBOS_BROWSER_RUNTIME_DIR="$RUN/user"
export BRAVE_WEBOS_DISPLAY_SOCKET="$RUN/display.sock"
export LD_LIBRARY_PATH="$APP/display/lib"
export FONTCONFIG_FILE="$APP/runtime/fonts.conf"
export FONTCONFIG_PATH="$APP/runtime"
export PULSE_SERVER=unix:/var/run/pulse/native
cd "$APP"
# The tile-memory override also applies to software rasterization.
exec "$APP/webos-display" "$BRAVE_WEBOS_DISPLAY_SOCKET" "$UID_BROWSER" -- \
    "$APP/browser-exec" "$BROWSER" \
    --ozone-platform=webos --ui-toolkit=fallback --ozone-override-screen-size=1280,720 \
    --window-size=1280,720 --window-position=0,0 --force-device-scale-factor=1 \
    --disable-gpu --no-sandbox --no-first-run --no-default-browser-check \
    --component-updater=url-source=https://update.googleapis.com/service/update2/json,disable-pings \
    --force-gpu-mem-available-mb=96 --skia-resource-cache-limit-mb=16 \
    --user-data-dir="$DATA/profile" --disk-cache-size=33554432 \
    --renderer-process-limit=2
