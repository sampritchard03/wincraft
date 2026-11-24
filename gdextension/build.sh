#!/bin/sh
# Builds the native extension into gdextension/bin/.
# Usage: ./build.sh [template_debug|template_release] [linux|windows]
# Windows builds cross-compile with MinGW-w64 (sudo apt install mingw-w64).
set -e
cd "$(dirname "$0")"
TARGET="${1:-template_debug}"
PLATFORM="${2:-linux}"
if [ "$PLATFORM" = windows ]; then
	TOOLCHAIN="-DCMAKE_TOOLCHAIN_FILE=$(pwd)/mingw-toolchain.cmake"
	DIR="build/windows-$TARGET"
else
	TOOLCHAIN=""
	DIR="build/$TARGET"
fi
cmake -S . -B "$DIR" -DCMAKE_BUILD_TYPE=Release -DGODOTCPP_TARGET="$TARGET" $TOOLCHAIN
cmake --build "$DIR" -j"$(nproc)"
