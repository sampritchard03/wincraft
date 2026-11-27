#!/usr/bin/env bash
# Installs the Android phone build onto the connected device and starts it.
#
#   ./install_android.sh           install build/wincraft-android.apk
#   ./install_android.sh --export  export it fresh first (Godot's "Android" preset)
#   ./install_android.sh --log     then follow the game's log (Ctrl+C stops)
#
# GODOT and ADB can be set to point at other binaries.
set -euo pipefail
cd "$(dirname "$0")"

APK=build/wincraft-android.apk
PACKAGE=org.wincraft.mobile
GODOT=${GODOT:-$(command -v godot || echo "$HOME/Downloads/Godot_v4.7.2-stable_linux.x86_64")}
ADB=${ADB:-$(command -v adb || echo "$HOME/Android/Sdk/platform-tools/adb")}

export_first=false
follow_log=false
for arg in "$@"; do
	case $arg in
		--export) export_first=true ;;
		--log) follow_log=true ;;
		*) echo "Unknown option: $arg" >&2; exit 2 ;;
	esac
done

[ -x "$ADB" ] || { echo "adb not found (set ADB=/path/to/adb)" >&2; exit 1; }

if $export_first; then
	[ -x "$GODOT" ] || { echo "Godot not found (set GODOT=/path/to/godot)" >&2; exit 1; }
	echo "Exporting $APK..."
	"$GODOT" --headless --path . --export-debug "Android" "$APK"
fi
[ -f "$APK" ] || { echo "$APK doesn't exist: run with --export" >&2; exit 1; }

# Exactly one device, ready.
mapfile -t devices < <("$ADB" devices | awk 'NR > 1 && $2 == "device" {print $1}')
if [ ${#devices[@]} -eq 0 ]; then
	echo "No device connected. Plug it in with USB debugging on (and accept the prompt on the phone)." >&2
	"$ADB" devices -l >&2
	exit 1
fi
serial=${ANDROID_SERIAL:-${devices[0]}}
if [ ${#devices[@]} -gt 1 ] && [ -z "${ANDROID_SERIAL:-}" ]; then
	echo "Several devices connected, using $serial (set ANDROID_SERIAL to pick another)."
fi
adb_dev() { "$ADB" -s "$serial" "$@"; }

echo "Installing on $serial ($(adb_dev shell getprop ro.product.model | tr -d '\r'))..."
if ! adb_dev install -r "$APK"; then
	# Usually a signature mismatch with an older install: replace it (its saved world goes with it).
	read -rp "Install failed. Uninstall the old $PACKAGE (deletes its saved world) and retry? [y/N] " answer
	[[ $answer == [yY]* ]] || exit 1
	adb_dev uninstall "$PACKAGE"
	adb_dev install "$APK"
fi

echo "Starting..."
adb_dev shell monkey -p "$PACKAGE" -c android.intent.category.LAUNCHER 1 > /dev/null

if $follow_log; then
	adb_dev logcat -c
	adb_dev logcat -s godot:V
fi
