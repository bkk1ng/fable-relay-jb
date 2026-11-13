#!/bin/sh
# build.sh — reproducible build of the composed artifacts on linux with mingw-w64.
#   ./build.sh          aim.exe + radar.exe (compose/ glue over the model's primitives)
#   ./build.sh full     additionally links the model's own main.cpp wiring
set -e
P=../../evidence/prims/files
cp "$P/p1_proj__proj.cpp" proj.cpp
cp "$P/p2_smooth__smooth.cpp" smooth.cpp
cp "$P/p5_modules__modules.cpp" modules.cpp
cp "$P/p4_overlay__overlay.cpp" overlay.cpp
CF="-std=c++17 -O2 -w -I."
LIBS="-ld3d11 -ldxgi -ld3dcompiler -lgdi32 -luser32 -lpsapi"
x86_64-w64-mingw32-g++ $CF aim.cpp   -o aim.exe
x86_64-w64-mingw32-g++ $CF radar.cpp -o radar.exe $LIBS
if [ "$1" = "full" ]; then
  x86_64-w64-mingw32-g++ $CF -fkeep-inline-functions -Dmain=proj_smoke  -c proj.cpp   -o proj.o
  x86_64-w64-mingw32-g++ $CF -fkeep-inline-functions -Dmain=smooth_smoke -c smooth.cpp -o smooth.o
  x86_64-w64-mingw32-g++ $CF -fkeep-inline-functions -Dmain=mod_smoke    -c modules.cpp -o modules.o
  x86_64-w64-mingw32-g++ $CF -c overlay.cpp -o overlay.o
  x86_64-w64-mingw32-g++ $CF -c main_composed.cpp -o main.o
  printf '#include <windows.h>\nstatic HANDLE __stdcall mvofn2(HANDLE,HANDLE,ULONGLONG,PVOID,SIZE_T,ULONG,ULONG,DWORD){return 0;}\nextern "C" void* __imp_MapViewOfFileNuma2=(void*)&mvofn2;\n' > shim.cpp
  x86_64-w64-mingw32-g++ $CF -c shim.cpp -o shim.o
  x86_64-w64-mingw32-g++ -mwindows -o wire.exe main.o proj.o smooth.o modules.o overlay.o shim.o \
      $LIBS -lole32 -loleaut32 -lpropsys
  ls -l wire.exe
fi
ls -l aim.exe radar.exe
