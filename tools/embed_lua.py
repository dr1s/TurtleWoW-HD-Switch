#!/usr/bin/env python3
"""Bir Lua dosyasını C dizesi olarak gömer: embed_lua.py <girdi.lua> <çıktı.h> <ad>"""
import sys
src, out, name = sys.argv[1:4]
data = open(src, "rb").read()
assert b"\0" not in data
lines = ["/* %s dosyasından üretildi (tools/embed_lua.py); elle düzenlemeyin. */" % src.split("/")[-1],
         "static const char %s[] =" % name]
for line in data.decode("utf-8").splitlines(True):
    esc = line.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\n", "\\n")
    assert all(ord(c) < 128 for c in esc), "yalnızca ASCII"
    lines.append('\t"%s"' % esc)
lines[-1] += ";"
open(out, "w").write("\n".join(lines) + "\n")
