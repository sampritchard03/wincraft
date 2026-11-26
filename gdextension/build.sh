#!/bin/sh
# Builds the native extension into gdextension/bin/.
# Usage: ./build.sh [template_debug|template_release] [linux|windows|android]
# Windows builds cross-compile with MinGW-w64 (sudo apt install mingw-w64).
# Android (Meta Quest, arm64) builds with the NDK: set ANDROID_NDK_ROOT, or it
# uses the newest under ~/Android/Sdk/ndk.
set -e
cd "$(dirname "$0")"
TARGET="${1:-template_debug}"
PLATFORM="${2:-linux}"
if [ "$PLATFORM" = android ]; then
	NDK="${ANDROID_NDK_ROOT:-$(ls -d "$HOME"/Android/Sdk/ndk/* 2>/dev/null | sort -V | tail -1)}"
	[ -d "$NDK" ] || { echo "No Android NDK: set ANDROID_NDK_ROOT" >&2; exit 1; }
	TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static"
	DIR="build/android-$TARGET"
elif [ "$PLATFORM" = windows ]; then
	TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=$(pwd)/mingw-toolchain.cmake"
	DIR="build/windows-$TARGET"
else
	TOOLCHAIN=""
	DIR="build/$TARGET"
fi
cmake -S . -B "$DIR" -DCMAKE_BUILD_TYPE=Release -DGODOTCPP_TARGET="$TARGET" $TOOLCHAIN
cmake --build "$DIR" -j"$(nproc)"
