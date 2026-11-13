#!/usr/bin/env bash
# Build every artifact in this set with the mingw-w64 toolchain. None of these link against a game;
# they compile to standalone Windows binaries.
set -u
cd "$(dirname "$0")"
CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
mkdir -p build
# overlay.cpp is a wide-char Win32 app; reader.cpp uses the ANSI toolhelp API. Flags are per file.
build() { f=$1; shift; out=build/$(basename "${f%.*}").exe
  if [ "${f##*.}" = c ]; then $CC "$@" -O2 "$f" -o "$out"; else $CXX -std=c++17 "$@" -O2 "$f" -o "$out" -static -lpsapi -ld3d11 -ld3dcompiler -ldxgi -ldwmapi -lws2_32; fi
  printf '%-34s %s\n' "$f" "$([ $? -eq 0 ] && echo OK || echo FAIL)"; }
build valorant/mapper.cpp
build valorant/reader.cpp
build valorant/aim.cpp -DAIM_STANDALONE_TEST
build rust/overlay.cpp -DUNICODE -D_UNICODE
build rust/pattern.cpp
echo "kmem.c needs the WDK (ntddk.h); build it with Visual Studio + WDK, not mingw."
