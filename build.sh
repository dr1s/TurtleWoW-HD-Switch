#!/bin/sh
# Build dll/HDToggle.dll. Needs Python 3 and i686-w64-mingw32-gcc.
set -e
cd "$(dirname "$0")"
python3 tools/embed_lua.py dll/glue.lua dll/glue_lua.h GLUE_LUA
i686-w64-mingw32-gcc -O2 -Wall -Wextra -std=c11 -shared -s -static-libgcc -mcrtdll=msvcrt-os \
	-Wl,--no-insert-timestamp -Wl,--disable-auto-image-base -Wl,--image-base,0x6E400000 \
	-o dll/HDToggle.dll dll/hdtoggle.c -lwininet
echo "built dll/HDToggle.dll"
