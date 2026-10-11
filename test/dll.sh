#!/bin/sh
# Rebuild the DLL and run the harness under Wine.
#   WINE   wine binary (default: wine)
#   LUA50  optional Lua 5.0 interpreter; runs the glue script tests when set
set -e
cd "$(dirname "$0")/.."
WINE=${WINE:-wine}
export WINEDEBUG=-all
export WINEPREFIX=${WINEPREFIX:-"$HOME/.cache/hdtoggle-wine"}

if [ -n "${LUA50:-}" ]; then
	echo "== glue"
	"${LUA50%/*}/luac" -p dll/glue.lua
	"$LUA50" test/test_glue.lua dll/glue.lua
fi

echo "== DLL"
python3 tools/embed_lua.py dll/glue.lua dll/glue_lua.h GLUE_LUA
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror -std=c11 -shared -s -static-libgcc -mcrtdll=msvcrt-os \
	-Wl,--no-insert-timestamp -Wl,--disable-auto-image-base -Wl,--image-base,0x6E400000 \
	-o dll/HDToggle.dll dll/hdtoggle.c -lwininet
i686-w64-mingw32-gcc -O1 -s -Wall -std=c11 -static-libgcc -mcrtdll=msvcrt-os -Wl,--image-base,0x400000 \
	-Wl,--section-start=.wow=0x00401000 -Wl,--section-start=.text=0x00D10000 -Wl,--section-start=.data=0x00D40000 \
	-Wl,--section-start=.rdata=0x00D50000 -Wl,--section-start=.bss=0x00D60000 -Wl,--section-start=.idata=0x00D70000 \
	-Wl,--section-start=.tls=0x00D80000 -Wl,--section-start=.CRT=0x00D90000 -Wl,--disable-dynamicbase \
	-Wl,--disable-reloc-section \
	-o test/harness.exe test/harness.c

TMP=$(mktemp -d)
cp dll/HDToggle.dll test/harness.exe "$TMP/"
WEB=$(mktemp -d)
printf '%s\n' '{"tag_name":"v9.9.9","name":"x"}' > "$WEB/newer.json"
printf '%s\n' '{"tag_name":"v0.0.1"}' > "$WEB/older.json"
printf '%s\n' '{"tag_name":"v1.2.3;rm"}' > "$WEB/bad.json"
PORT=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])')
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$WEB" >/dev/null 2>&1 &
WEBPID=$!
trap 'kill "$WEBPID" 2>/dev/null; rm -rf "$TMP" "$WEB"' EXIT
export HDT_TEST_PORT=$PORT
sleep 0.5
mkdir -p "$WINEPREFIX"
(cd "$TMP" && "$WINE" harness.exe HDToggle.dll 2>&1 | grep -v -i -E 'freetype|truetype|2\.0\.5')
