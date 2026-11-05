#!/bin/bash
# Starts The New Tetris: Reborn (Linux)
here="$(cd "$(dirname "$0")" && pwd)"
exec "$here/TNTRecomp/out/build/linux/TNTRecompiled" "$@"
