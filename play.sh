#!/bin/bash
# Starts The New Tetris: Reborn (Linux)
here="$(cd "$(dirname "$0")" && pwd)"

if [ -f "$here/TNTRecompiled" ]; then
    BIN="$here/TNTRecompiled"
elif [ -f "$here/TNTRecomp/out/build/linux/TNTRecompiled" ]; then
    BIN="$here/TNTRecomp/out/build/linux/TNTRecompiled"
elif [ -f "$here/build/TNTRecompiled" ]; then
    BIN="$here/build/TNTRecompiled"
else
    echo "[!] TNTRecompiled binary not found!"
    exit 1
fi

if [ -f "$here/baserom.us.z64" ]; then
    exec "$BIN" "$here/baserom.us.z64" "$@"
else
    exec "$BIN" "$@"
fi
