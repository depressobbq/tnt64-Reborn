#!/bin/bash
# Starts The New Tetris: Reborn (Linux)
here="$(cd "$(dirname "$0")" && pwd)"

# Candidate binary locations
CANDIDATES=(
    "$here/TNTRecomp/out/build/linux/TNTRecompiled"
    "$here/build/TNTRecompiled"
    "$here/TNTRecompiled"
)

BIN=""
NEWEST_TIME=0

# Pick the newest binary
for f in "${CANDIDATES[@]}"; do
    if [ -f "$f" ]; then
        M_TIME=$(stat -c %Y "$f" 2>/dev/null || stat -f %m "$f" 2>/dev/null || echo 0)
        if [ "$M_TIME" -gt "$NEWEST_TIME" ]; then
            NEWEST_TIME="$M_TIME"
            BIN="$f"
        fi
    fi
done

if [ -z "$BIN" ]; then
    echo "[!] TNTRecompiled binary not found!"
    exit 1
fi

# Ensure root TNTRecompiled stays in sync with the newest build
if [ "$BIN" != "$here/TNTRecompiled" ]; then
    cp -f "$BIN" "$here/TNTRecompiled"
    BIN="$here/TNTRecompiled"
fi

if [ -f "$here/baserom.us.z64" ]; then
    exec "$BIN" "$here/baserom.us.z64" "$@"
else
    exec "$BIN" "$@"
fi
