#!/bin/sh
set -eu
unset LD_PRELOAD LD_AUDIT LD_LIBRARY_PATH
APP=/media/developer/apps/usr/palm/applications/org.webosbrew.brave.b9

fail() {
    echo "Brave: $*" >&2
    if ! luna-send-pub -n 1 -w 2000 \
        luna://com.webos.notification/createToast \
        '{"sourceId":"org.webosbrew.brave.b9","message":"Brave could not start. Check that the TV is rooted and the complete package is installed."}' >&2; then
        echo 'Brave: could not display the startup error notification.' >&2
    fi
    exit 1
}

if [ "$(id -u)" != 0 ]; then
    REQUEST=$(printf '{"command":"%s/start-native.sh %s"}' "$APP" "$$")
    RESPONSE=$(luna-send-pub -n 1 -w 10000 \
        luna://org.webosbrew.hbchannel.service/exec "$REQUEST") ||
        fail 'The Homebrew root service could not start Brave.'
    case "$RESPONSE" in
        *BRAVE_B9_STARTED*) ;;
        *) printf '%s\n' "$RESPONSE" >&2; fail 'The root launcher reported an error.' ;;
    esac
    exec luna-send-pub -i \
        luna://com.webos.applicationManager/registerNativeApp \
        '{"id":"org.webosbrew.brave.b9"}'
fi
for directory in "$APP" "$APP/engine" "$APP/runtime" "$APP/display"; do
    [ -d "$directory" ] && [ ! -L "$directory" ] &&
        [ "$(stat -c %u "$directory")" = 0 ] &&
        [ "$(stat -c %a "$directory")" = 755 ] ||
        fail 'Application directory has unsafe ownership or permissions.'
done
for file in "$APP/engine/brave" "$APP/webos-display" "$APP/webos-media" "$APP/browser-exec" \
            "$APP/runtime/lib/ld-linux-aarch64.so.1" "$APP/launch-engine.sh"; do
    [ -f "$file" ] && [ ! -L "$file" ] &&
        [ "$(stat -c %u "$file")" = 0 ] &&
        [ "$(stat -c %a "$file")" = 755 ] ||
        fail 'A bundled executable is missing or unsafe.'
done
exec "$APP/launch-engine.sh"
